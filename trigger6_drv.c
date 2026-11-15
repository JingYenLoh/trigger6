// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>

#include <drm/drm_modeset_helper.h>

#include "trigger6.h"

static int trigger6_usb_suspend(struct usb_interface *interface,
				pm_message_t message)
{
	struct trigger6_device *trigger6 = usb_get_intfdata(interface);
	int i, ret;

	ret = drm_mode_config_helper_suspend(&trigger6->drm);
	if (ret)
		return ret;

	for (i = 0; i < TRIGGER6_NUM_OUTPUTS; i++)
		trigger6_stop_io(&trigger6->outputs[i]);

	return 0;
}

static int trigger6_usb_resume(struct usb_interface *interface)
{
	struct trigger6_device *trigger6 = usb_get_intfdata(interface);

	return drm_mode_config_helper_resume(&trigger6->drm);
}

static int trigger6_usb_probe(struct usb_interface *interface,
			      const struct usb_device_id *id)
{
	struct trigger6_device *trigger6;

	trigger6 = trigger6_drm_create(interface);
	if (IS_ERR(trigger6))
		return PTR_ERR(trigger6);

	usb_set_intfdata(interface, trigger6);

	return 0;
}

static void trigger6_usb_disconnect(struct usb_interface *interface)
{
	struct trigger6_device *trigger6 = usb_get_intfdata(interface);

	trigger6_drm_destroy(trigger6);
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5601, 0xff, 0x00, 0x00) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver trigger6_driver = {
	.name = DRIVER_NAME,
	.probe = trigger6_usb_probe,
	.disconnect = trigger6_usb_disconnect,
	.suspend = trigger6_usb_suspend,
	.resume = trigger6_usb_resume,
	.id_table = id_table,
};

module_usb_driver(trigger6_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_DESC);
