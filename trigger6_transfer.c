// SPDX-License-Identifier: GPL-2.0-only

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

	if (urb->status)
		drm_warn(&trigger6->drm, "URB completed with status %d",
			 urb->status);

	spin_lock_irqsave(&trigger6->urb_available_list_lock, flags);
	list_add_tail(&urb_entry->entry, &trigger6->urb_available_list);
	spin_unlock_irqrestore(&trigger6->urb_available_list_lock, flags);
	up(&trigger6->urb_available_list_sem);

	if (atomic_dec_and_test(&trigger6->urbs_in_flight))
		complete(&trigger6->transfer_done);
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

		urb_buf = usb_alloc_coherent(usb_dev,
					     TRIGGER6_MAX_TRANSFER_LENGTH,
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

void trigger6_send_frame(struct trigger6_device *trigger6, void *buf,
			 size_t buf_size)
{
	int idx;
	size_t chunks = DIV_ROUND_UP(buf_size, TRIGGER6_MAX_TRANSFER_LENGTH);

	if (!drm_dev_enter(&trigger6->drm, &idx))
		return;

	for (size_t i = 0; i < chunks; i++) {
		size_t offset = i * TRIGGER6_MAX_TRANSFER_LENGTH;
		size_t length = min(buf_size - offset,
				    (size_t)TRIGGER6_MAX_TRANSFER_LENGTH);
		struct urb *urb;
		struct trigger6_session session = { 0 };
		int ret;

		session.session_number = 0;
		session.payload_length = cpu_to_le32(buf_size);
		session.dest_addr = cpu_to_le32(0x030);
		session.fragment_length = cpu_to_le32(length);
		session.output_index = cpu_to_le32(0x0);
		session.offset = cpu_to_le32(offset);

		urb = trigger6_get_urb(trigger6);
		if (IS_ERR(urb))
			goto out;
		memcpy(urb->transfer_buffer, &session, sizeof(session));
		urb->transfer_buffer_length = sizeof(session);
		atomic_inc(&trigger6->urbs_in_flight);
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret) {
			drm_warn(&trigger6->drm,
				 "Session URB submit failed: %d", ret);
			trigger6_urb_completion(urb);
			goto out;
		}

		urb = trigger6_get_urb(trigger6);
		if (IS_ERR(urb))
			goto out;
		memcpy(urb->transfer_buffer, buf + offset, length);
		urb->transfer_buffer_length = length;
		atomic_inc(&trigger6->urbs_in_flight);
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret) {
			drm_warn(&trigger6->drm,
				 "Payload URB submit failed: %d", ret);
			trigger6_urb_completion(urb);
			goto out;
		}
	}

out:
	if (atomic_read(&trigger6->urbs_in_flight) == 0)
		complete(&trigger6->transfer_done);
	drm_dev_exit(idx);
}