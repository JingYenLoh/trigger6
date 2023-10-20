// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "trigger6.h"

static int trigger6_usb_suspend(struct usb_interface *interface,
				pm_message_t message)
{
	struct drm_device *dev = usb_get_intfdata(interface);

	return drm_mode_config_helper_suspend(dev);
}

static int trigger6_usb_resume(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);

	return drm_mode_config_helper_resume(dev);
}

/*
 * FIXME: Dma-buf sharing requires DMA support by the importing device.
 *        This function is a workaround to make USB devices work as well.
 *        See todo.rst for how to fix the issue in the dma-buf framework.
 */
static struct drm_gem_object *
trigger6_driver_gem_prime_import(struct drm_device *dev,
				 struct dma_buf *dma_buf)
{
	struct trigger6_device *trigger6 = to_trigger6(dev);

	if (!trigger6->dmadev)
		return ERR_PTR(-ENODEV);

	return drm_gem_prime_import_dev(dev, dma_buf, trigger6->dmadev);
}

DEFINE_DRM_GEM_FOPS(trigger6_driver_fops);

static const struct drm_driver driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,

	/* GEM hooks */
	.fops = &trigger6_driver_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_prime_import = trigger6_driver_gem_prime_import,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs trigger6_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct trigger6_mode *
trigger6_get_mode(const struct drm_display_mode *mode)
{
	return ERR_PTR(-EINVAL);
}

static void trigger6_pipe_enable(struct drm_simple_display_pipe *pipe,
				 struct drm_crtc_state *crtc_state,
				 struct drm_plane_state *plane_state)
{
	// TODO
}

static void trigger6_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	// TODO
}

enum drm_mode_status
trigger6_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			 const struct drm_display_mode *mode)
{
	// TODO
	return MODE_OK;
}

int trigger6_pipe_check(struct drm_simple_display_pipe *pipe,
			struct drm_plane_state *new_plane_state,
			struct drm_crtc_state *new_crtc_state)
{
	return 0;
}

static void trigger6_pipe_update(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *old_state)
{
	// TODO
}

static const struct drm_simple_display_pipe_funcs trigger6_pipe_funcs = {
	.enable = trigger6_pipe_enable,
	.disable = trigger6_pipe_disable,
	.check = trigger6_pipe_check,
	.mode_valid = trigger6_pipe_mode_valid,
	.update = trigger6_pipe_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

static const uint32_t trigger6_pipe_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static int trigger6_usb_probe(struct usb_interface *interface,
			      const struct usb_device_id *id)
{
	int ret;
	struct trigger6_device *trigger6;
	struct drm_device *dev;

	trigger6 = devm_drm_dev_alloc(&interface->dev, &driver,
				      struct trigger6_device, drm);
	if (IS_ERR(trigger6))
		return PTR_ERR(trigger6);

	trigger6->intf = interface;
	dev = &trigger6->drm;

	init_completion(&trigger6->transfer_done);

	trigger6->dmadev = usb_intf_get_dma_device(interface);
	if (!trigger6->dmadev)
		drm_warn(dev,
			 "buffer sharing not supported"); /* not an error */

	ret = drmm_mode_config_init(dev);
	if (ret)
		goto err_put_device;

	/* No idea */
	dev->mode_config.min_width = 0;
	dev->mode_config.max_width = 10000;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_height = 10000;
	dev->mode_config.funcs = &trigger6_mode_config_funcs;

	// Might need
	// trigger6_init_urb(trigger6, 100 * TRIGGER6_MAX_TRANSFER_LENGTH);
	trigger6_read_modes(trigger6, 0, 0, trigger6->modes, 512);
	trigger6_read_modes(trigger6, 0, 512, trigger6->modes + 16, 448);

	for (int i = 0; i < ARRAY_SIZE(trigger6->modes); i++) {
		drm_warn(dev, "mode %d: %dx%d@%d\n", i,
			 trigger6->modes[i].line_active_pixels, trigger6->modes[i].frame_active_lines,
			 trigger6->modes[i].refresh_rate_hz);
	}

	ret = trigger6_connector_init(trigger6);
	if (ret)
		goto err_put_device;

	ret = drm_simple_display_pipe_init(
		&trigger6->drm, &trigger6->display_pipe, &trigger6_pipe_funcs,
		trigger6_pipe_formats, ARRAY_SIZE(trigger6_pipe_formats), NULL,
		&trigger6->connector);
	if (ret)
		goto err_put_device;

	drm_plane_enable_fb_damage_clips(&trigger6->display_pipe.plane);

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, trigger6);

	drm_kms_helper_poll_init(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_put_device;

	drm_fbdev_generic_setup(dev, 0);

	return 0;

err_put_device:
	put_device(trigger6->dmadev);
	return ret;
}

static void trigger6_usb_disconnect(struct usb_interface *interface)
{
	struct trigger6_device *trigger6 = usb_get_intfdata(interface);
	struct drm_device *dev = &trigger6->drm;

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	// trigger6_free_urb(trigger6);
	put_device(trigger6->dmadev);
	trigger6->dmadev = NULL;
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x5601, 0xff, 0x00, 0x00) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver trigger6_driver = {
	.name = "trigger6",
	.probe = trigger6_usb_probe,
	.disconnect = trigger6_usb_disconnect,
	.suspend = trigger6_usb_suspend,
	.resume = trigger6_usb_resume,
	.id_table = id_table,
};
module_usb_driver(trigger6_driver);
MODULE_LICENSE("GPL");
