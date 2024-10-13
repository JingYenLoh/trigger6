// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fbdev_ttm.h>
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

static struct trigger6_mode *
trigger6_get_mode(struct drm_simple_display_pipe *pipe,
		  const struct drm_display_mode *mode)
{
	struct trigger6_device *device = to_trigger6(pipe->crtc.dev);

	int i;
	u16 width = mode->hdisplay;
	u16 height = mode->vdisplay;
	u16 hz = drm_mode_vrefresh(mode);
	for (i = 0; i < ARRAY_SIZE(device->modes); i++) {
		if (device->modes[i].line_active_pixels == width &&
		    device->modes[i].frame_active_lines == height &&
		    device->modes[i].refresh_rate_hz == hz) {
			return &device->modes[i];
		}
	}

	return ERR_PTR(-EINVAL);
}

static void trigger6_pipe_enable(struct drm_simple_display_pipe *pipe,
				 struct drm_crtc_state *crtc_state,
				 struct drm_plane_state *plane_state)
{
	struct trigger6_device *trigger6 = to_trigger6(pipe->crtc.dev);

	trigger6_enable_output(trigger6);

	if (crtc_state->mode_changed) {
		trigger6_set_resolution(
			trigger6,
			trigger6_get_mode(pipe, &crtc_state->adjusted_mode));
	}
}

static void trigger6_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct trigger6_device *device = to_trigger6(pipe->crtc.dev);

	trigger6_disable_output(device);
}

enum drm_mode_status
trigger6_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			 const struct drm_display_mode *mode)
{
	const struct trigger6_mode *trigger6_mode =
		trigger6_get_mode(pipe, mode);
	return IS_ERR(trigger6_mode) ? MODE_BAD : MODE_OK;
}

static void trigger6_pipe_update(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *old_state)
{
	int ret;
	struct trigger6_device *trigger6 = to_trigger6(pipe->crtc.dev);
	struct usb_interface *intf = trigger6->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(state);
	int width, height;
	size_t buf_size;
	struct drm_rect current_rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &current_rect)) {
		// hack to force full screen updates for now
		current_rect.x1 = 0;
		current_rect.y1 = 0;
		current_rect.x2 = state->fb->width;
		current_rect.y2 = state->fb->height;

		width = drm_rect_width(&current_rect);
		height = drm_rect_height(&current_rect);

		buf_size = sizeof(struct trigger6_video_header) +
			   width * height * 3;
		void *buf = vmalloc(buf_size);
		struct trigger6_video_header video_header = { 0 };
		video_header.type = cpu_to_le32(0x3);
		video_header.data_length = cpu_to_le32(buf_size);
		video_header.sequence_counter = cpu_to_le32(1);
		video_header.unk4 = cpu_to_le32(0x9);
		// Guessed from pcap
		video_header.width = cpu_to_le16(width * 3);
		video_header.height = cpu_to_le16(0);
		video_header.start_address = cpu_to_le32(0x60);
		video_header.end_address = cpu_to_le32(0x60);
		video_header.image_format = cpu_to_le32(TRIGGER6_BGR24_FORMAT);
		memcpy(buf, &video_header, sizeof(video_header));

		// Put BGR24 representation of framebuffer into buf
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(
			buf + sizeof(struct trigger6_video_header));
		ret = drm_gem_fb_begin_cpu_access(state->fb, DMA_FROM_DEVICE);
		if (ret < 0) {
			drm_warn(&trigger6->drm, "fb CPU access failed: %d",
				 ret);
		}
		struct drm_format_conv_state fmtcnv_state =
			DRM_FORMAT_CONV_STATE_INIT;
		drm_fb_xrgb8888_to_rgb888(&map, NULL,
					  &shadow_plane_state->data[0],
					  state->fb, &current_rect,
					  &fmtcnv_state);
		drm_gem_fb_end_cpu_access(state->fb, DMA_FROM_DEVICE);

		size_t blocks =
			DIV_ROUND_UP(buf_size, TRIGGER6_MAX_TRANSFER_LENGTH);
		struct trigger6_session *session =
			kzalloc(sizeof(struct trigger6_session), GFP_KERNEL);
		void *transfer_block =
			kmalloc(TRIGGER6_MAX_TRANSFER_LENGTH, GFP_KERNEL);

		for (size_t i = 0; i < blocks; i++) {
			size_t offset = i * TRIGGER6_MAX_TRANSFER_LENGTH;
			size_t length =
				min((i + 1) * TRIGGER6_MAX_TRANSFER_LENGTH,
				    buf_size) -
				offset;
			session->session_number = 0;
			session->payload_length = cpu_to_le32(buf_size);
			session->dest_addr = cpu_to_le32(0x030);
			session->fragment_length = cpu_to_le32(length);
			session->output_index = cpu_to_le32(0x0);
			session->offset = cpu_to_le32(offset);

			ret = usb_bulk_msg(
				usb_dev,
				usb_sndbulkpipe(usb_dev,
						TRIGGER6_ENDPOINT_BULK_OUT),
				session, sizeof(struct trigger6_session), NULL,
				5000);
			if (ret < 0) {
				drm_warn(&trigger6->drm,
					 "Session negotiation failed: %d", ret);
			}

			memcpy(transfer_block, buf + offset, length);
			ret = usb_bulk_msg(
				usb_dev,
				usb_sndbulkpipe(usb_dev,
						TRIGGER6_ENDPOINT_BULK_OUT),
				transfer_block, length, NULL, 5000);

			if (ret < 0) {
				drm_warn(&trigger6->drm,
					 "Transfer block failed");
			}
		}

		kfree(session);
		kfree(transfer_block);
		vfree(buf);
	}
}

static const struct drm_simple_display_pipe_funcs trigger6_pipe_funcs = {
	.enable = trigger6_pipe_enable,
	.disable = trigger6_pipe_disable,
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
			 trigger6->modes[i].line_active_pixels,
			 trigger6->modes[i].frame_active_lines,
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

	drm_fbdev_ttm_setup(dev, 0);

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
