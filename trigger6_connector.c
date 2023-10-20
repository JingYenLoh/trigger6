
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "trigger6.h"

static int trigger6_read_edid(void *data, u8 *buf, unsigned int block, size_t length)
{
	int ret;
	struct trigger6_device *trigger6 = data;
	struct usb_interface *intf = trigger6->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	int offset = block * EDID_LENGTH;

	if (block > 0)
	{
		memset(buf, 0, length);
		return 0;
	}

	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0), 0x80,
			      USB_DIR_IN | USB_TYPE_VENDOR, offset,
			      0, buf, length, USB_CTRL_GET_TIMEOUT);

	
	drm_warn(trigger6->connector.dev, "read edid: %d %zu\n", buf[4], length);

	return 0;
}

static int trigger6_connector_get_modes(struct drm_connector *connector)
{
	int ret;
	struct trigger6_device *trigger6 = to_trigger6(connector->dev);
	struct edid *edid;
	edid = drm_do_get_edid(connector, trigger6_read_edid, trigger6);
	drm_connector_update_edid_property(connector, edid);
	ret = drm_add_edid_modes(connector, edid);
	kfree(edid);
	return ret;
}

static enum drm_connector_status trigger6_detect(struct drm_connector *connector,
					       bool force)
{
	struct trigger6_device *trigger6 = to_trigger6(connector->dev);
	int status = trigger6_read_connector_status(trigger6, 0);

	drm_warn(connector->dev, "connector status: %d\n", status);

	if (status < 0)
		return connector_status_unknown;

	return status == 1 ? connector_status_connected : connector_status_disconnected;
}

static const struct drm_connector_helper_funcs trigger6_connector_helper_funcs = {
	.get_modes = trigger6_connector_get_modes,
};

static const struct drm_connector_funcs trigger6_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.detect = trigger6_detect,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int trigger6_connector_init(struct trigger6_device *trigger6)
{
	int ret;
	drm_connector_helper_add(&trigger6->connector,
				 &trigger6_connector_helper_funcs);
	ret = drm_connector_init(&trigger6->drm, &trigger6->connector,
				 &trigger6_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	trigger6->connector.polled = DRM_CONNECTOR_POLL_HPD |
				   DRM_CONNECTOR_POLL_CONNECT |
				   DRM_CONNECTOR_POLL_DISCONNECT;
	return ret;
}