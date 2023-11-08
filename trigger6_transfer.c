
#include <linux/dma-buf.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "trigger6.h"

void trigger6_free_urb(struct trigger6_device *trigger6)
{
	int i, blocks;
	struct trigger6_urb *urb_entry;
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);
	blocks = trigger6->num_urbs;
	for (i = 0; i < blocks; i++) {
		down(&trigger6->urb_available_list_sem);

		spin_lock_irq(&trigger6->urb_available_list_lock);
		urb_entry = list_first_entry(&trigger6->urb_available_list,
					     struct trigger6_urb, entry);
		list_del(&urb_entry->entry);
		spin_unlock_irq(&trigger6->urb_available_list_lock);

		usb_free_coherent(usb_dev, TRIGGER6_MAX_TRANSFER_LENGTH,
				  urb_entry->urb->transfer_buffer,
				  urb_entry->urb->transfer_dma);
		usb_free_urb(urb_entry->urb);
		kfree(urb_entry);
	}
}

void trigger6_urb_completion(struct urb *urb)
{
	struct trigger6_urb *urb_entry = urb->context;
	struct trigger6_device *trigger6 = urb_entry->parent;
	unsigned long flags;

	spin_lock_irqsave(&trigger6->urb_available_list_lock, flags);
	list_add_tail(&urb_entry->entry, &trigger6->urb_available_list);
	spin_unlock_irqrestore(&trigger6->urb_available_list_lock, flags);
	up(&trigger6->urb_available_list_sem);
}

int trigger6_init_urb(struct trigger6_device *trigger6, size_t total_size)
{
	int i, blocks;
	struct trigger6_urb *urb_entry;
	struct urb *urb;
	void *urb_buf;
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);

	blocks = DIV_ROUND_UP(total_size, TRIGGER6_MAX_TRANSFER_LENGTH);
	spin_lock_init(&trigger6->urb_available_list_lock);
	INIT_LIST_HEAD(&trigger6->urb_available_list);
	sema_init(&trigger6->urb_available_list_sem, 0);
	trigger6->num_urbs = 0;
	for (i = 0; i < blocks; i++) {
		urb_entry = kzalloc(sizeof(struct trigger6_urb), GFP_KERNEL);
		if (!urb_entry)
			break;
		urb_entry->parent = trigger6;

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			kfree(urb_entry);
			break;
		}
		urb_entry->urb = urb;

		urb_buf =
			usb_alloc_coherent(usb_dev, TRIGGER6_MAX_TRANSFER_LENGTH,
					   GFP_KERNEL, &urb->transfer_dma);

		if (!urb_buf) {
			usb_free_urb(urb);
			kfree(urb_entry);
			break;
		}

		usb_fill_bulk_urb(urb, usb_dev, usb_sndbulkpipe(usb_dev, 4),
				  urb_buf, TRIGGER6_MAX_TRANSFER_LENGTH,
				  trigger6_urb_completion, urb_entry);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		list_add_tail(&urb_entry->entry, &trigger6->urb_available_list);
		up(&trigger6->urb_available_list_sem);
		trigger6->num_urbs++;
	}
	return trigger6->num_urbs;
}

struct urb *trigger6_get_urb(struct trigger6_device *trigger6)
{
	int ret;
	struct trigger6_urb *urb_entry;

	ret = down_interruptible(&trigger6->urb_available_list_sem);
	if (ret < 0)
		return ERR_PTR(ret);

	spin_lock_irq(&trigger6->urb_available_list_lock);
	urb_entry = list_first_entry(&trigger6->urb_available_list,
				     struct trigger6_urb, entry);
	list_del_init(&urb_entry->entry);
	spin_unlock_irq(&trigger6->urb_available_list_lock);

	return urb_entry->urb;
}

static const unsigned char trigger6_end_of_buffer[8] = { 0xff, 0xc0, 0x00, 0x00,
						       0x00, 0x00, 0x00, 0x00 };

void trigger6_fb_send_rect(struct drm_framebuffer *fb,
			 const struct iosys_map *map, struct drm_rect *rect)
{
	int ret, i;
	void *vaddr = map->vaddr;
	struct trigger6_device *trigger6 = to_trigger6(fb->dev);
	struct drm_device *drm = &trigger6->drm;
	struct trigger6_frame_update_header header;
	struct urb *urb;
	void *transfer_buffer;
	int total_length = 0;
	int transfer_blocks, transfer_length;
	/* Hardware can only update framebuffer in multiples of 16 horizontally */
	int x = ALIGN_DOWN(rect->x1, 16);
	int width = ALIGN(rect->x2, 16) - x;
	int y1 = rect->y1;
	int y2 = rect->y2;
	int idx;

	drm_dev_enter(drm, &idx);

	rect->x1 = x;
	rect->x2 = x + width;

	header.header = cpu_to_be16(0xff00);
	header.x = x / 16;
	header.y = cpu_to_be16(y1);
	header.width = width / 16;
	header.height = cpu_to_be16(drm_rect_height(rect));

	transfer_buffer =
		vmalloc(drm_rect_width(rect) * drm_rect_height(rect) * 2 + 16);
	if (!transfer_buffer)
		goto dev_exit;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto free_transfer_buffer;

	memcpy(transfer_buffer, &header, 8);
	total_length += 8;

	for (i = y1; i < y2; i++) {
		const int line_offset = fb->pitches[0] * i;
		const int byte_offset = line_offset + (x * 4);
		trigger6_xrgb_to_yuv422_line(transfer_buffer + total_length,
					   vaddr + byte_offset, width);
		total_length += width * 2;
	}

	memcpy(transfer_buffer + total_length, trigger6_end_of_buffer, 8);
	total_length += 8;

	transfer_blocks =
		DIV_ROUND_UP(total_length, TRIGGER6_MAX_TRANSFER_LENGTH);

	for (i = 0; i < transfer_blocks; i++) {
		/* Last block may be shorter */
		urb = trigger6_get_urb(trigger6);
		if (IS_ERR(urb))
			break;
		transfer_length = min((i + 1) * TRIGGER6_MAX_TRANSFER_LENGTH,
				      total_length) -
				  i * TRIGGER6_MAX_TRANSFER_LENGTH;

		memcpy(urb->transfer_buffer,
		       transfer_buffer + i * TRIGGER6_MAX_TRANSFER_LENGTH,
		       transfer_length);
		urb->transfer_buffer_length = transfer_length;
		urb->complete = trigger6_urb_completion;
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret < 0) {
			trigger6_urb_completion(urb);
			break;
		}
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
free_transfer_buffer:
	vfree(transfer_buffer);
dev_exit:
	drm_dev_exit(idx);
}