// SPDX-License-Identifier: GPL-2.0-only

#include <linux/iosys-map.h>
#include <linux/minmax.h>
#include <linux/slab.h>

#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_rect.h>

#include "trigger6.h"

static const u32 trigger6_frame_addr[TRIGGER6_NUM_OUTPUTS] = {
	0x30, 0x2500400
};

static void trigger6_urb_complete(struct urb *urb)
{
	struct trigger6_transfer *transfer = urb->context;

	switch (urb->status) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		break;
	case 0:
		if (urb->actual_length == urb->transfer_buffer_length)
			break;
		fallthrough;
	default:
		drm_err_ratelimited(&transfer->trigger6->drm,
				    "USB transfer failed: %d (%u/%u bytes)\n",
				    urb->status, urb->actual_length,
				    urb->transfer_buffer_length);
		break;
	}

	complete(&transfer->done);
}

struct trigger6_transfer *trigger6_get_next_transfer(struct trigger6_pool *pool)
{
	struct trigger6_transfer *transfer = &pool->transfers[pool->next];
	unsigned long timeout = msecs_to_jiffies(TRIGGER6_BULK_TIMEOUT_MS);

	timeout = wait_for_completion_timeout(&transfer->done, timeout);
	if (!timeout)
		goto err_timeout;

	if (!wait_for_completion_timeout(&transfer->done, timeout)) {
		/* Give back the completion of the URB that did finish */
		complete(&transfer->done);
		goto err_timeout;
	}

	pool->next = (pool->next + 1) % TRIGGER6_NUM_TRANSFERS;

	return transfer;

err_timeout:
	drm_err_ratelimited(&transfer->trigger6->drm,
			    "USB transfer timed out\n");
	return ERR_PTR(-ETIMEDOUT);
}

/* Returns NULL instead of waiting if the next transfer is still in flight */
struct trigger6_transfer *
trigger6_try_get_next_transfer(struct trigger6_pool *pool)
{
	struct trigger6_transfer *transfer = &pool->transfers[pool->next];

	if (!try_wait_for_completion(&transfer->done))
		return NULL;

	if (!try_wait_for_completion(&transfer->done)) {
		complete(&transfer->done);
		return NULL;
	}

	pool->next = (pool->next + 1) % TRIGGER6_NUM_TRANSFERS;

	return transfer;
}

void trigger6_fill_session(struct trigger6_transfer *transfer, u32 number,
			   u32 dest, size_t payload_len, size_t offset,
			   size_t len)
{
	struct trigger6_session *session =
		transfer->session_urb->transfer_buffer;

	memset(session, 0, sizeof(*session));
	session->session_number = cpu_to_le32(number);
	session->payload_length = cpu_to_le32(payload_len);
	session->dest_addr = cpu_to_le32(dest);
	session->fragment_length = cpu_to_le32(len);
	session->offset = cpu_to_le32(offset);
	session->output_index = cpu_to_le32(0);
}

int trigger6_submit_transfer(struct trigger6_transfer *transfer, size_t len)
{
	struct trigger6_device *trigger6 = transfer->trigger6;
	unsigned long flags;
	int ret;

	transfer->data_urb->transfer_buffer_length = len;

	spin_lock_irqsave(&trigger6->bulk_lock, flags);
	ret = usb_submit_urb(transfer->session_urb, GFP_ATOMIC);
	if (ret)
		complete(&transfer->done);
	else
		ret = usb_submit_urb(transfer->data_urb, GFP_ATOMIC);
	spin_unlock_irqrestore(&trigger6->bulk_lock, flags);

	if (ret) {
		complete(&transfer->done);
		drm_err_once(&trigger6->drm,
			     "failed to submit USB transfer: %d\n", ret);
	}

	return ret;
}

void trigger6_kill_pool(struct trigger6_pool *pool)
{
	int i;

	for (i = 0; i < TRIGGER6_NUM_TRANSFERS; i++) {
		usb_kill_urb(pool->transfers[i].session_urb);
		usb_kill_urb(pool->transfers[i].data_urb);
	}
}

void trigger6_free_pool(struct trigger6_pool *pool)
{
	int i;

	for (i = 0; i < TRIGGER6_NUM_TRANSFERS; i++) {
		usb_free_urb(pool->transfers[i].session_urb);
		usb_free_urb(pool->transfers[i].data_urb);
		pool->transfers[i].session_urb = NULL;
		pool->transfers[i].data_urb = NULL;
	}
}

static struct urb *trigger6_alloc_urb(struct trigger6_transfer *transfer,
				      size_t size)
{
	struct usb_device *udev = interface_to_usbdev(transfer->trigger6->intf);
	struct urb *urb;
	void *buf;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return NULL;

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf) {
		usb_free_urb(urb);
		return NULL;
	}

	usb_fill_bulk_urb(urb, udev,
			  usb_sndbulkpipe(udev, TRIGGER6_ENDPOINT_BULK_OUT),
			  buf, size, trigger6_urb_complete, transfer);
	urb->transfer_flags |= URB_FREE_BUFFER;

	return urb;
}

int trigger6_alloc_pool(struct trigger6_device *trigger6,
			struct trigger6_pool *pool, size_t size)
{
	struct trigger6_transfer *transfer;
	int i;

	for (i = 0; i < TRIGGER6_NUM_TRANSFERS; i++) {
		transfer = &pool->transfers[i];
		transfer->trigger6 = trigger6;

		transfer->session_urb =
			trigger6_alloc_urb(transfer,
					   sizeof(struct trigger6_session));
		if (!transfer->session_urb)
			return -ENOMEM;

		transfer->data_urb = trigger6_alloc_urb(transfer, size);
		if (!transfer->data_urb)
			return -ENOMEM;

		init_completion(&transfer->done);
		complete(&transfer->done);
		complete(&transfer->done);
	}

	pool->next = 0;

	return 0;
}

void trigger6_send_frame(struct trigger6_output *output,
			 struct drm_framebuffer *fb, const struct iosys_map *src,
			 const struct drm_rect *rect,
			 struct drm_format_conv_state *fmtcnv_state)
{
	struct trigger6_device *trigger6 = output->trigger6;
	u32 dest = trigger6_frame_addr[output->index];
	u32 start = dest + sizeof(struct trigger6_video_header);
	struct trigger6_transfer *transfer;
	struct trigger6_video_header *hdr;
	struct iosys_map data_map;
	struct drm_rect clip;
	unsigned int pitch;
	size_t payload_len, offset, len;
	int lines_per_urb, y, idx;

	if (!drm_dev_enter(&trigger6->drm, &idx))
		return;

	/* Rows need to be 16-byte aligned */
	pitch = ALIGN(drm_rect_width(rect) * 3, 16);
	payload_len = sizeof(*hdr) + pitch * drm_rect_height(rect);
	lines_per_urb = TRIGGER6_URB_BUF_SIZE / pitch;

	if (WARN_ON_ONCE(!lines_per_urb))
		goto exit;

	transfer = trigger6_get_next_transfer(&output->pool);
	if (IS_ERR(transfer))
		goto exit;

	trigger6_fill_session(transfer, TRIGGER6_SESSION_VIDEO, dest,
			      payload_len, 0, sizeof(*hdr));

	hdr = transfer->data_urb->transfer_buffer;
	memset(hdr, 0, sizeof(*hdr));
	hdr->type = cpu_to_le32(3 + output->index);
	hdr->data_length = cpu_to_le32(payload_len);
	hdr->sequence_counter = cpu_to_le32(1);
	hdr->unk4 = cpu_to_le32(0x9);
	/* Guessed from pcap */
	hdr->width = cpu_to_le16(pitch);
	hdr->height = cpu_to_le16(0);
	hdr->start_address = cpu_to_le32(start);
	hdr->end_address = cpu_to_le32(start);
	hdr->image_format = cpu_to_le32(TRIGGER6_BGR24_FORMAT);

	if (trigger6_submit_transfer(transfer, sizeof(*hdr)))
		goto exit;

	offset = sizeof(*hdr);
	clip = *rect;
	for (y = rect->y1; y < rect->y2; y += lines_per_urb) {
		clip.y1 = y;
		clip.y2 = min(y + lines_per_urb, rect->y2);
		len = pitch * drm_rect_height(&clip);

		transfer = trigger6_get_next_transfer(&output->pool);
		if (IS_ERR(transfer))
			break;

		trigger6_fill_session(transfer, TRIGGER6_SESSION_VIDEO, dest,
				      payload_len, offset, len);

		iosys_map_set_vaddr(&data_map,
				    transfer->data_urb->transfer_buffer);
		drm_fb_xrgb8888_to_rgb888(&data_map, &pitch, src, fb, &clip,
					  fmtcnv_state);

		if (trigger6_submit_transfer(transfer, len))
			break;

		offset += len;
	}

exit:
	drm_dev_exit(idx);
}
