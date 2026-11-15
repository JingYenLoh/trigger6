// SPDX-License-Identifier: GPL-2.0-only

#include <linux/rational.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#include "trigger6.h"

/* clock = ref / 32 * (integer + numerator / denominator) * multiplier */
static const u32 trigger6_pll_ref_khz[] = {
	48000,	/* Lite, unverified */
	40000,	/* Super Lite */
};

/* Verified on the Super Lite */
#define TRIGGER6_MIN_CLOCK_KHZ	25000
#define TRIGGER6_MAX_CLOCK_KHZ	297000

#define plane_to_output(x) container_of(x, struct trigger6_output, plane)
#define crtc_to_output(x) container_of(x, struct trigger6_output, crtc)
#define connector_to_output(x) \
	container_of(x, struct trigger6_output, connector)

DEFINE_DRM_GEM_FOPS(trigger6_driver_fops);

static const struct drm_driver trigger6_drm_driver = {
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

static void trigger6_compute_mode(struct trigger6_device *trigger6,
				  const struct drm_display_mode *mode,
				  struct trigger6_mode *dst)
{
	/* x1, x2, x4 and x8, keeping the integer within 20..39 like Windows */
	static const u8 multipliers[] = { 0x0, 0x2, 0x1, 0x3 };
	int step = trigger6->pll_step_khz, shift = 0;
	unsigned long num, den;

	while (shift < 3 && mode->clock >= step * 40) {
		step <<= 1;
		shift++;
	}
	rational_best_approximation(mode->clock % step, step, U16_MAX, U16_MAX,
				    &num, &den);

	*dst = (struct trigger6_mode) {
		.clock_khz = cpu_to_le32(mode->clock),
		.refresh_hz = cpu_to_le16(drm_mode_vrefresh(mode)),
		.htotal = cpu_to_le16(mode->htotal),
		.hdisplay = cpu_to_le16(mode->hdisplay),
		.hsync_start = cpu_to_le16(mode->hsync_start),
		.hsync_width = cpu_to_le16(mode->hsync_end - mode->hsync_start),
		.vtotal = cpu_to_le16(mode->vtotal),
		.vdisplay = cpu_to_le16(mode->vdisplay),
		.vsync_start = cpu_to_le16(mode->vsync_start),
		.vsync_width = cpu_to_le16(mode->vsync_end - mode->vsync_start),
		.pll_numerator = cpu_to_le16(num),
		.pll_denominator = cpu_to_le16(den),
		.pll_integer = mode->clock / step,
		.pll_multiplier = multipliers[shift],
		.hsync_positive = !!(mode->flags & DRM_MODE_FLAG_PHSYNC),
		.vsync_positive = !!(mode->flags & DRM_MODE_FLAG_PVSYNC),
	};
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
	struct trigger6_output *output = plane_to_output(plane);
	struct drm_rect current_rect;

	if (!drm_atomic_helper_damage_merged(old_state, state, &current_rect))
		return;

	/* hack to force full screen updates for now */
	drm_rect_fp_to_int(&current_rect, &state->src);

	if (drm_gem_fb_begin_cpu_access(state->fb, DMA_FROM_DEVICE) < 0)
		return;

	trigger6_send_frame(output, state->fb, &shadow_plane_state->data[0],
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
	struct trigger6_output *output = crtc_to_output(crtc);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct trigger6_mode mode;
	int idx;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger6_enable_output(output->trigger6, output->index);

	if (crtc_state->mode_changed) {
		trigger6_compute_mode(output->trigger6,
				      &crtc_state->adjusted_mode, &mode);
		trigger6_set_resolution(output->trigger6, output->index, &mode);
	}

	drm_dev_exit(idx);
}

static void trigger6_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct trigger6_output *output = crtc_to_output(crtc);
	int idx;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger6_disable_output(output->trigger6, output->index);

	drm_dev_exit(idx);
}

static enum drm_mode_status
trigger6_crtc_mode_valid(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode)
{
	struct trigger6_output *output = crtc_to_output(crtc);

	if (!trigger6_frame_fits(output, mode->hdisplay, mode->vdisplay))
		return MODE_MEM;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;
	if (mode->clock < TRIGGER6_MIN_CLOCK_KHZ)
		return MODE_CLOCK_LOW;
	if (mode->clock > TRIGGER6_MAX_CLOCK_KHZ)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
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

static int trigger6_read_edid_block(void *data, u8 *buf, unsigned int block,
				    size_t len)
{
	struct trigger6_output *output = data;

	return trigger6_read_edid(output->trigger6, output->index,
				  block * EDID_LENGTH, buf, len);
}

static int trigger6_connector_get_modes(struct drm_connector *connector)
{
	struct trigger6_output *output = connector_to_output(connector);
	const struct drm_edid *edid;
	int ret, idx;

	if (!drm_dev_enter(connector->dev, &idx))
		return 0;

	edid = drm_edid_read_custom(connector, trigger6_read_edid_block,
				    output);

	drm_dev_exit(idx);

	if (!edid)
		return 0;

	ret = drm_edid_connector_update(connector, edid);
	if (ret < 0) {
		ret = 0;
		goto err_free_edid;
	}
	ret = drm_edid_connector_add_modes(connector);

err_free_edid:
	drm_edid_free(edid);
	return ret;
}

static enum drm_connector_status
trigger6_connector_detect(struct drm_connector *connector, bool force)
{
	struct trigger6_output *output = connector_to_output(connector);
	int status, idx;

	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;

	status = trigger6_read_connector_status(output->trigger6,
						output->index);

	drm_dev_exit(idx);

	if (status < 0)
		return connector_status_unknown;

	return status == 1 ? connector_status_connected :
			     connector_status_disconnected;
}

static const struct drm_connector_helper_funcs trigger6_connector_helper_funcs = {
	.get_modes = trigger6_connector_get_modes,
};

static const struct drm_connector_funcs trigger6_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.detect = trigger6_connector_detect,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int trigger6_output_init(struct trigger6_device *trigger6, int index)
{
	struct trigger6_output *output = &trigger6->outputs[index];
	struct drm_device *dev = &trigger6->drm;
	int ret;

	output->trigger6 = trigger6;
	output->index = index;

	ret = trigger6_alloc_pool(trigger6, &output->pool,
				  TRIGGER6_URB_BUF_SIZE);
	if (ret)
		return ret;

	ret = drm_universal_plane_init(dev, &output->plane, BIT(index),
				       &trigger6_plane_funcs,
				       trigger6_plane_formats,
				       ARRAY_SIZE(trigger6_plane_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_helper_add(&output->plane, &trigger6_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&output->plane);

	ret = drm_crtc_init_with_planes(dev, &output->crtc, &output->plane,
					NULL, &trigger6_crtc_funcs, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(&output->crtc, &trigger6_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &output->encoder, &trigger6_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	output->encoder.possible_crtcs = drm_crtc_mask(&output->crtc);

	drm_connector_helper_add(&output->connector,
				 &trigger6_connector_helper_funcs);
	ret = drm_connector_init(dev, &output->connector,
				 &trigger6_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	output->connector.polled = DRM_CONNECTOR_POLL_HPD |
				   DRM_CONNECTOR_POLL_CONNECT |
				   DRM_CONNECTOR_POLL_DISCONNECT;

	return drm_connector_attach_encoder(&output->connector,
					    &output->encoder);
}

static void trigger6_free_all_urbs(struct trigger6_device *trigger6)
{
	int i;

	for (i = 0; i < TRIGGER6_NUM_OUTPUTS; i++)
		trigger6_free_pool(&trigger6->outputs[i].pool);
}

struct trigger6_device *trigger6_drm_create(struct usb_interface *interface)
{
	struct trigger6_device *trigger6;
	struct drm_device *dev;
	struct device *dma_dev;
	int i, ret;

	trigger6 = devm_drm_dev_alloc(&interface->dev, &trigger6_drm_driver,
				      struct trigger6_device, drm);
	if (IS_ERR(trigger6))
		return trigger6;

	trigger6->intf = interface;
	spin_lock_init(&trigger6->bulk_lock);
	dev = &trigger6->drm;

	ret = trigger6_read_platform(trigger6);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret >= ARRAY_SIZE(trigger6_pll_ref_khz)) {
		drm_err(dev, "Unknown hardware platform %d\n", ret);
		return ERR_PTR(-ENODEV);
	}
	trigger6->pll_step_khz = trigger6_pll_ref_khz[ret] / 32;

	ret = trigger6_read_vram_size(trigger6);
	if (ret < 0)
		return ERR_PTR(ret);
	trigger6->vram_size = ret * SZ_1M;

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
		return ERR_PTR(ret);

	dev->mode_config.min_width = 0;
	dev->mode_config.max_width = 10000;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_height = 10000;
	dev->mode_config.funcs = &trigger6_mode_config_funcs;
	dev->mode_config.helper_private = &trigger6_mode_config_helper_funcs;

	for (i = 0; i < TRIGGER6_NUM_OUTPUTS; i++) {
		ret = trigger6_output_init(trigger6, i);
		if (ret)
			goto err_free_urbs;
	}

	drm_mode_config_reset(dev);

	drm_kms_helper_poll_init(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_poll_fini;

	drm_client_setup(dev, NULL);

	return trigger6;

err_poll_fini:
	drm_kms_helper_poll_fini(dev);
err_free_urbs:
	trigger6_free_all_urbs(trigger6);
	return ERR_PTR(ret);
}

void trigger6_drm_destroy(struct trigger6_device *trigger6)
{
	struct drm_device *dev = &trigger6->drm;
	int i;

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	for (i = 0; i < TRIGGER6_NUM_OUTPUTS; i++)
		trigger6_kill_pool(&trigger6->outputs[i].pool);
	trigger6_free_all_urbs(trigger6);
}
