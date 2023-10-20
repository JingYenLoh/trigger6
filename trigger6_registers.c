
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

int trigger6_read_byte(struct trigger6_device *trigger6, u16 address)
{
	// TODO
	return 0;
}

static inline int trigger6_write_6_bytes(struct trigger6_device *trigger6,
					 u16 address, void *data)
{
	// TODO
	return 0;
}

int trigger6_power_on(struct trigger6_device *trigger6)
{
	// TODO
	return 0;
}

int trigger6_power_off(struct trigger6_device *trigger6)
{
	// TODO
	return 0;
}
int trigger6_set_resolution(struct trigger6_device *trigger6,
			    const struct trigger6_mode *mode)
{
	// TODO
	return 0;
}