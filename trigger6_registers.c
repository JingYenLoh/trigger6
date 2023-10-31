
#include <uapi/linux/hid.h>

#include "trigger6.h"

int trigger6_read_modes(struct trigger6_device *trigger6, int output_index,
			int byte_offset, void *data, int length)
{
	int ret;
	struct usb_interface *intf = trigger6->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0), 0x89,
			      USB_DIR_IN | USB_TYPE_VENDOR, output_index,
			      byte_offset, data, length, USB_CTRL_GET_TIMEOUT);
	return ret;
}

int trigger6_read_connector_status(struct trigger6_device *trigger6,
				   int output_index)
{
	int ret;
	struct usb_interface *intf = trigger6->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	u8 *status;
	status = kmalloc(1, GFP_KERNEL);

	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0), 0x87,
			      USB_DIR_IN | USB_TYPE_VENDOR, output_index, 0,
			      status, 1, USB_CTRL_GET_TIMEOUT);

	if (ret < 0)
		return ret;

	ret = *status;
	kfree(status);

	return ret;
}

int trigger6_enable_output(struct trigger6_device *trigger6)
{
	int ret;
	struct usb_interface *intf = trigger6->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	ret = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), 0x3,
			      USB_TYPE_VENDOR, 0, 0x0001, NULL, 0,
			      USB_CTRL_SET_TIMEOUT);

	return ret;
}

int trigger6_disable_output(struct trigger6_device *trigger6)
{
	int ret;
	struct usb_interface *intf = trigger6->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	ret = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), 0x3,
			      USB_TYPE_VENDOR, 0, 0x0000, NULL, 0,
			      USB_CTRL_SET_TIMEOUT);

	return ret;
}

int trigger6_set_resolution(struct trigger6_device *trigger6,
			    struct trigger6_mode *mode)
{
	int ret;
	struct usb_interface *intf = trigger6->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	ret = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), 0x12,
			      USB_TYPE_VENDOR, 0, 0, mode, 32,
			      USB_CTRL_SET_TIMEOUT);

	return ret;
}