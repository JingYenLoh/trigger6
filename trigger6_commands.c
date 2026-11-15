// SPDX-License-Identifier: GPL-2.0-only

#include "trigger6.h"

int trigger6_read_platform(struct trigger6_device *trigger6)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);
	__le32 platform;
	int ret;

	ret = usb_control_msg_recv(usb_dev, 0, 0xb0,
				   USB_DIR_IN | USB_TYPE_VENDOR, 0, 0,
				   &platform, sizeof(platform),
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (ret)
		return ret;

	return le32_to_cpu(platform);
}

/* In MiB */
int trigger6_read_vram_size(struct trigger6_device *trigger6)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);
	u8 size;
	int ret;

	ret = usb_control_msg_recv(usb_dev, 0, 0x88,
				   USB_DIR_IN | USB_TYPE_VENDOR, 0, 0,
				   &size, sizeof(size),
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (ret)
		return ret;

	return size;
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

int trigger6_set_audio(struct trigger6_device *trigger6, bool enable)
{
	struct usb_device *usb_dev = interface_to_usbdev(trigger6->intf);
	struct trigger6_audio_config config = {
		.enable = cpu_to_le32(enable),
		.unk2 = cpu_to_le32(0x2580),
		.unk3 = cpu_to_le32(0x20000),
	};

	return usb_control_msg_send(usb_dev, 0, 0x24, USB_TYPE_VENDOR, 0, 0,
				    &config, sizeof(config),
				    USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
}
