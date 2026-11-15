// SPDX-License-Identifier: GPL-2.0-only

#include <linux/iosys-map.h>
#include <linux/minmax.h>
#include <linux/slab.h>

#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_rect.h>

#include "trigger6.h"

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

static struct trigger6_transfer *
trigger6_get_next_urb(struct trigger6_device *trigger6)
{
	struct trigger6_transfer *transfer =
		&trigger6->transfers[trigger6->next_transfer];

	if (!wait_for_completion_timeout(&transfer->done,
					 msecs_to_jiffies(TRIGGER6_BULK_TIMEOUT_MS))) {
		drm_err_ratelimited(&trigger6->drm, "USB transfer timed out\n");
		return ERR_PTR(-ETIMEDOUT);
	}

	trigger6->next_transfer =
		(trigger6->next_transfer + 1) % TRIGGER6_NUM_URBS;

	return transfer;
}

static int trigger6_submit_urb(struct trigger6_transfer *transfer, size_t len)
{
	int ret;

	transfer->urb->transfer_buffer_length = len;
	ret = usb_submit_urb(transfer->urb, GFP_KERNEL);
	if (ret) {
		drm_err_once(&transfer->trigger6->drm,
			     "failed to submit USB transfer: %d\n", ret);
		complete(&transfer->done);
	}

	return ret;
}

void trigger6_stop_io(struct trigger6_device *trigger6)
{
	int i;

	for (i = 0; i < TRIGGER6_NUM_URBS; i++)
		usb_kill_urb(trigger6->transfers[i].urb);
}

void trigger6_free_urbs(struct trigger6_device *trigger6)
{
	int i;

	for (i = 0; i < TRIGGER6_NUM_URBS; i++) {
		usb_free_urb(trigger6->transfers[i].urb);
		trigger6->transfers[i].urb = NULL;
	}
}

int trigger6_alloc_urbs(struct trigger6_device *trigger6)
{
	struct usb_device *udev = interface_to_usbdev(trigger6->intf);
	struct trigger6_transfer *transfer;
	struct urb *urb;
	void *buf;
	int i;

	for (i = 0; i < TRIGGER6_NUM_URBS; i++) {
		transfer = &trigger6->transfers[i];

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;
		transfer->urb = urb;

		buf = kmalloc(TRIGGER6_URB_BUF_SIZE, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		usb_fill_bulk_urb(urb, udev,
				  usb_sndbulkpipe(udev, TRIGGER6_ENDPOINT_BULK_OUT),
				  buf,
				  TRIGGER6_URB_BUF_SIZE, trigger6_urb_complete,
				  transfer);
		urb->transfer_flags |= URB_FREE_BUFFER;

		transfer->trigger6 = trigger6;
		init_completion(&transfer->done);
		complete(&transfer->done);
	}

	trigger6->next_transfer = 0;

	return 0;
}

static int trigger6_send_session(struct trigger6_device *trigger6,
				 size_t payload_len, size_t offset, size_t len)
{
	struct trigger6_transfer *transfer;
	struct trigger6_session *session;

	transfer = trigger6_get_next_urb(trigger6);
	if (IS_ERR(transfer))
		return PTR_ERR(transfer);

	session = transfer->urb->transfer_buffer;
	memset(session, 0, sizeof(*session));
	session->payload_length = cpu_to_le32(payload_len);
	session->dest_addr = cpu_to_le32(0x30);
	session->fragment_length = cpu_to_le32(len);
	session->offset = cpu_to_le32(offset);
	session->output_index = cpu_to_le32(0);

	return trigger6_submit_urb(transfer, sizeof(*session));
}

void trigger6_send_frame(struct trigger6_device *trigger6,
			 struct drm_framebuffer *fb, const struct iosys_map *src,
			 const struct drm_rect *rect,
			 struct drm_format_conv_state *fmtcnv_state)
{
	struct trigger6_transfer *transfer;
	struct trigger6_video_header *hdr;
	struct iosys_map data_map;
	struct drm_rect clip;
	size_t pitch, payload_len, offset, len;
	int lines_per_urb, y, idx;

	if (!drm_dev_enter(&trigger6->drm, &idx))
		return;

	pitch = (size_t)drm_rect_width(rect) * 3;
	payload_len = sizeof(*hdr) + pitch * drm_rect_height(rect);
	lines_per_urb = TRIGGER6_URB_BUF_SIZE / pitch;

	if (WARN_ON_ONCE(!lines_per_urb))
		goto exit;

	if (trigger6_send_session(trigger6, payload_len, 0, sizeof(*hdr)))
		goto exit;

	transfer = trigger6_get_next_urb(trigger6);
	if (IS_ERR(transfer))
		goto exit;

	hdr = transfer->urb->transfer_buffer;
	memset(hdr, 0, sizeof(*hdr));
	hdr->type = cpu_to_le32(0x3);
	hdr->data_length = cpu_to_le32(payload_len);
	hdr->sequence_counter = cpu_to_le32(1);
	hdr->unk4 = cpu_to_le32(0x9);
	/* Guessed from pcap */
	hdr->width = cpu_to_le16(pitch);
	hdr->height = cpu_to_le16(0);
	hdr->start_address = cpu_to_le32(0x60);
	hdr->end_address = cpu_to_le32(0x60);
	hdr->image_format = cpu_to_le32(TRIGGER6_BGR24_FORMAT);

	if (trigger6_submit_urb(transfer, sizeof(*hdr)))
		goto exit;

	offset = sizeof(*hdr);
	clip = *rect;
	for (y = rect->y1; y < rect->y2; y += lines_per_urb) {
		clip.y1 = y;
		clip.y2 = min(y + lines_per_urb, rect->y2);
		len = pitch * drm_rect_height(&clip);

		if (trigger6_send_session(trigger6, payload_len, offset, len))
			break;

		transfer = trigger6_get_next_urb(trigger6);
		if (IS_ERR(transfer))
			break;

		iosys_map_set_vaddr(&data_map, transfer->urb->transfer_buffer);
		drm_fb_xrgb8888_to_rgb888(&data_map, NULL, src, fb, &clip,
					  fmtcnv_state);

		if (trigger6_submit_urb(transfer, len))
			break;

		offset += len;
	}

exit:
	drm_dev_exit(idx);
}
