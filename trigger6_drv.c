// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>

#include "trigger6.h"

static int trigger6_usb_suspend(struct usb_interface *interface,
				pm_message_t message)
{
	struct trigger6_device *trigger6 = usb_get_intfdata(interface);
	int ret;

	ret = drm_mode_config_helper_suspend(&trigger6->drm);
	if (ret)
		return ret;

	trigger6_stop_io(trigger6);

	return 0;
}

static int trigger6_usb_resume(struct usb_interface *interface)
{
	struct trigger6_device *trigger6 = usb_get_intfdata(interface);

	return drm_mode_config_helper_resume(&trigger6->drm);
}

DEFINE_DRM_GEM_FOPS(trigger6_driver_fops);

static const struct drm_driver driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,

	/* GEM hooks */
	.fops = &trigger6_driver_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs trigger6_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs
trigger6_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static struct trigger6_mode *
trigger6_get_mode(struct trigger6_device *trigger6,
		  const struct drm_display_mode *mode)
{
	u16 width = mode->hdisplay;
	u16 height = mode->vdisplay;
	u16 hz = drm_mode_vrefresh(mode);
	int i;

	for (i = 0; i < ARRAY_SIZE(trigger6->modes); i++) {
		if (trigger6->modes[i].line_active_pixels == width &&
		    trigger6->modes[i].frame_active_lines == height &&
		    trigger6->modes[i].refresh_rate_hz == hz)
			return &trigger6->modes[i];
	}

	return ERR_PTR(-EINVAL);
}

static int trigger6_plane_atomic_check(struct drm_plane *plane,
				       struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(new_plane_state);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state = NULL;
	size_t len;
	int ret;

	if (crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state,
						  new_crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !new_plane_state->visible)
		return ret;

	/* For drm_fb_xrgb8888_to_rgb888 temp buffer */
	len = new_plane_state->fb->width * sizeof(u32);
	if (!drm_format_conv_state_reserve(&shadow_plane_state->fmtcnv_state,
					   len, GFP_KERNEL))
		return -ENOMEM;

	return 0;
}

static void trigger6_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_commit *atomic_state)
{
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(atomic_state, plane);
	struct drm_plane_state *state =
		drm_atomic_get_new_plane_state(atomic_state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(state);
	struct trigger6_device *trigger6 = to_trigger6(plane->dev);
	struct drm_rect current_rect;

	if (!drm_atomic_helper_damage_merged(old_state, state, &current_rect))
		return;

	/* hack to force full screen updates for now */
	current_rect.x1 = 0;
	current_rect.y1 = 0;
	current_rect.x2 = state->fb->width;
	current_rect.y2 = state->fb->height;

	if (drm_gem_fb_begin_cpu_access(state->fb, DMA_FROM_DEVICE) < 0)
		return;

	trigger6_send_frame(trigger6, state->fb, &shadow_plane_state->data[0],
			    &current_rect, &shadow_plane_state->fmtcnv_state);

	drm_gem_fb_end_cpu_access(state->fb, DMA_FROM_DEVICE);
}

static const struct drm_plane_helper_funcs trigger6_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = trigger6_plane_atomic_check,
	.atomic_update = trigger6_plane_atomic_update,
};

static const struct drm_plane_funcs trigger6_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const u32 trigger6_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static void trigger6_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct trigger6_device *trigger6 = to_trigger6(crtc->dev);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct trigger6_mode *mode;
	int idx;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger6_enable_output(trigger6);

	if (crtc_state->mode_changed) {
		mode = trigger6_get_mode(trigger6, &crtc_state->adjusted_mode);
		if (!IS_ERR(mode))
			trigger6_set_resolution(trigger6, mode);
	}

	drm_dev_exit(idx);
}

static void trigger6_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct trigger6_device *trigger6 = to_trigger6(crtc->dev);
	int idx;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger6_disable_output(trigger6);

	drm_dev_exit(idx);
}

static enum drm_mode_status
trigger6_crtc_mode_valid(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode)
{
	struct trigger6_device *trigger6 = to_trigger6(crtc->dev);

	return IS_ERR(trigger6_get_mode(trigger6, mode)) ? MODE_BAD : MODE_OK;
}

static const struct drm_crtc_helper_funcs trigger6_crtc_helper_funcs = {
	.mode_valid = trigger6_crtc_mode_valid,
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_enable = trigger6_crtc_atomic_enable,
	.atomic_disable = trigger6_crtc_atomic_disable,
};

static const struct drm_crtc_funcs trigger6_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs trigger6_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int trigger6_usb_probe(struct usb_interface *interface,
			      const struct usb_device_id *id)
{
	int ret;
	struct trigger6_device *trigger6;
	struct drm_device *dev;
	struct device *dma_dev;

	trigger6 = devm_drm_dev_alloc(&interface->dev, &driver,
				      struct trigger6_device, drm);
	if (IS_ERR(trigger6))
		return PTR_ERR(trigger6);

	trigger6->intf = interface;
	dev = &trigger6->drm;

	dma_dev = usb_intf_get_dma_device(interface);
	if (dma_dev) {
		drm_dev_set_dma_dev(dev, dma_dev);
		put_device(dma_dev);
	} else {
		drm_warn(dev,
			 "buffer sharing not supported"); /* not an error */
	}

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	trigger6_read_modes(trigger6, 0, 0, trigger6->modes, 512);
	trigger6_read_modes(trigger6, 0, 512, trigger6->modes + 16, 448);

	ret = trigger6_alloc_urbs(trigger6);
	if (ret)
		goto err_free_urbs;

	dev->mode_config.min_width = 0;
	dev->mode_config.max_width = 10000;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_height = 10000;
	dev->mode_config.funcs = &trigger6_mode_config_funcs;
	dev->mode_config.helper_private = &trigger6_mode_config_helper_funcs;

	ret = drm_universal_plane_init(dev, &trigger6->plane, 0,
				       &trigger6_plane_funcs,
				       trigger6_plane_formats,
				       ARRAY_SIZE(trigger6_plane_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		goto err_free_urbs;

	drm_plane_helper_add(&trigger6->plane, &trigger6_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&trigger6->plane);

	ret = drm_crtc_init_with_planes(dev, &trigger6->crtc, &trigger6->plane,
					NULL, &trigger6_crtc_funcs, NULL);
	if (ret)
		goto err_free_urbs;

	drm_crtc_helper_add(&trigger6->crtc, &trigger6_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &trigger6->encoder, &trigger6_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		goto err_free_urbs;
	trigger6->encoder.possible_crtcs = drm_crtc_mask(&trigger6->crtc);

	ret = trigger6_connector_init(trigger6);
	if (ret)
		goto err_free_urbs;

	ret = drm_connector_attach_encoder(&trigger6->connector,
					   &trigger6->encoder);
	if (ret)
		goto err_free_urbs;

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, trigger6);

	drm_kms_helper_poll_init(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_poll_fini;

	drm_client_setup(dev, NULL);

	return 0;

err_poll_fini:
	drm_kms_helper_poll_fini(dev);
	usb_set_intfdata(interface, NULL);
err_free_urbs:
	trigger6_free_urbs(trigger6);
	return ret;
}

static void trigger6_usb_disconnect(struct usb_interface *interface)
{
	struct trigger6_device *trigger6 = usb_get_intfdata(interface);
	struct drm_device *dev = &trigger6->drm;

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	trigger6_stop_io(trigger6);
	trigger6_free_urbs(trigger6);
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
