// SPDX-License-Identifier: GPL-2.0-only

#ifndef TRIGGER6_H
#define TRIGGER6_H

#include <linux/completion.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/usb.h>

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>

struct drm_format_conv_state;
struct drm_rect;
struct iosys_map;

#define DRIVER_NAME "trigger6"
#define DRIVER_DESC "Magic Control Technology Trigger 6"

#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 1

#define TRIGGER6_ENDPOINT_BULK_IN	0x1
#define TRIGGER6_ENDPOINT_BULK_OUT	0x2
#define TRIGGER6_ENDPOINT_INTERRUPT_IN	0x3

#define TRIGGER6_NUM_OUTPUTS	2
#define TRIGGER6_MAX_MODES	36

#define TRIGGER6_NUM_URBS	8
#define TRIGGER6_URB_BUF_SIZE	SZ_128K
#define TRIGGER6_BULK_TIMEOUT_MS	2000

struct trigger6_mode {
	u32 pixel_clock_khz;
	u16 refresh_rate_hz;
	u16 line_total_pixels;
	u16 line_active_pixels;
	u16 line_active_plus_front_porch_pixels;
	u16 line_sync_width;
	u16 frame_total_lines;
	u16 frame_active_lines;
	u16 frame_active_plus_front_porch_lines;
	u16 frame_sync_width;
	u16 unk8;
	u16 unk9;
	u16 unk10;
	u8 sync_polarity_0;
	u8 sync_polarity_1;
	u16 unk11;
} __packed;

struct trigger6_session {
	__le32 session_number;
	__le32 payload_length;
	__le32 dest_addr;
	__le32 fragment_length;
	__le32 offset;
	__le32 output_index;
	__le32 unk7;
	__le32 unk8;
} __packed;

#define TRIGGER6_JPEG_FORMAT 0xD
#define TRIGGER6_NV12_FORMAT 0x6
#define TRIGGER6_BGR24_FORMAT 0x9

struct trigger6_video_header {
	__le32 type; // 0x3 = full, 0x4 = partial?, 0x7 = partial??
	__le32 data_length;
	__le32 sequence_counter;
	__le32 unk4;	// values seen: 6, 9
	__le16 width;	// or height
	__le16 height;	// or width
	__le32 start_address;
	__le32 end_address;
	__le32 unk9;
	__le32 image_format; // 0xD for JPEG, 0x6 for NV12, 0x9 for BGR24
	__le32 unk11;
	__le32 unk12;
	__le32 unk13;
} __packed;

struct trigger6_transfer {
	struct trigger6_output *output;
	struct urb *urb;
	struct completion done;
};

struct trigger6_output {
	struct trigger6_device *trigger6;
	int index;

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct trigger6_mode modes[TRIGGER6_MAX_MODES];

	int next_transfer;
	struct trigger6_transfer transfers[TRIGGER6_NUM_URBS];
};

struct trigger6_device {
	struct drm_device drm;
	struct usb_interface *intf;
	/* Keeps the fragments of a frame together on the shared bulk endpoint */
	struct mutex bulk_lock;
	struct trigger6_output outputs[TRIGGER6_NUM_OUTPUTS];
};

#define to_trigger6(x) container_of(x, struct trigger6_device, drm)

struct trigger6_device *trigger6_drm_create(struct usb_interface *intf);
void trigger6_drm_destroy(struct trigger6_device *trigger6);

int trigger6_read_modes(struct trigger6_device *trigger6, int output,
			int byte_offset, void *data, int length);
int trigger6_read_connector_status(struct trigger6_device *trigger6,
				   int output);
int trigger6_read_edid(struct trigger6_device *trigger6, int output,
		       int byte_offset, void *data, int length);
int trigger6_set_resolution(struct trigger6_device *trigger6, int output,
			    const struct trigger6_mode *mode);
int trigger6_enable_output(struct trigger6_device *trigger6, int output);
int trigger6_disable_output(struct trigger6_device *trigger6, int output);

int trigger6_alloc_urbs(struct trigger6_output *output);
void trigger6_free_urbs(struct trigger6_output *output);
void trigger6_stop_io(struct trigger6_output *output);

void trigger6_send_frame(struct trigger6_output *output,
			 struct drm_framebuffer *fb, const struct iosys_map *src,
			 const struct drm_rect *rect,
			 struct drm_format_conv_state *fmtcnv_state);

#endif
