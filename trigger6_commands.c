// SPDX-License-Identifier: GPL-2.0-only

#include "trigger6.h"

int trigger6_read_modes(struct trigger6_device *trigger6, int output,
			int byte_offset, void *data, int length)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);

	return usb_control_msg_recv(usb_dev, 0, 0x89,
				    USB_DIR_IN | USB_TYPE_VENDOR, output,
				    byte_offset, data, length,
				    USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
}

int trigger6_read_connector_status(struct trigger6_device *trigger6,
				   int output)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);
	u8 status;
	int ret;

	ret = usb_control_msg_recv(usb_dev, 0, 0x87,
				   USB_DIR_IN | USB_TYPE_VENDOR, output, 0,
				   &status, sizeof(status),
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (ret)
		return ret;

	return status;
}

int trigger6_read_edid(struct trigger6_device *trigger6, int output,
		       int byte_offset, void *data, int length)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);

	return usb_control_msg_recv(usb_dev, 0, 0x80,
				    USB_DIR_IN | USB_TYPE_VENDOR, byte_offset,
				    output, data, length,
				    USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
}

int trigger6_set_resolution(struct trigger6_device *trigger6, int output,
			    const struct trigger6_mode *mode)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);

	return usb_control_msg_send(usb_dev, 0, 0x12, USB_TYPE_VENDOR, output,
				    0, mode, sizeof(*mode),
				    USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
}

int trigger6_enable_output(struct trigger6_device *trigger6, int output)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);

	return usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), 0x3,
			       USB_TYPE_VENDOR, output, 0x0001, NULL, 0,
			       USB_CTRL_SET_TIMEOUT);
}

int trigger6_disable_output(struct trigger6_device *trigger6, int output)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);

	return usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), 0x3,
			       USB_TYPE_VENDOR, output, 0x0000, NULL, 0,
			       USB_CTRL_SET_TIMEOUT);
}
