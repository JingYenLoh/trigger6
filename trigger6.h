// SPDX-License-Identifier: GPL-2.0-only

#ifndef TRIGGER6_H
#define TRIGGER6_H

#include <linux/completion.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/mm_types.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/usb.h>

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>

#include <sound/asound.h>

struct drm_format_conv_state;
struct drm_rect;
struct iosys_map;
struct snd_card;
struct snd_pcm;
struct snd_pcm_substream;

#define DRIVER_NAME "trigger6"
#define DRIVER_DESC "Magic Control Technology Trigger 6"

#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 1

#define TRIGGER6_ENDPOINT_BULK_IN	0x1
#define TRIGGER6_ENDPOINT_BULK_OUT	0x2
#define TRIGGER6_ENDPOINT_INTERRUPT_IN	0x3

#define TRIGGER6_NUM_OUTPUTS	2

#define TRIGGER6_NUM_TRANSFERS	8
#define TRIGGER6_URB_BUF_SIZE	SZ_128K
#define TRIGGER6_BULK_TIMEOUT_MS	2000

struct trigger6_mode {
	__le32 clock_khz;
	__le16 refresh_hz;
	__le16 htotal;
	__le16 hdisplay;
	/* Sync starts one early and lasts one longer, also on Windows */
	__le16 hsync_start;
	__le16 hsync_width;
	__le16 vtotal;
	__le16 vdisplay;
	__le16 vsync_start;
	__le16 vsync_width;
	__le16 pll_numerator;
	__le16 pll_denominator;
	u8 pll_integer;
	u8 pll_multiplier;	// bit 0: x4, bit 1: x2
	u8 hsync_positive;
	u8 vsync_positive;
	__le16 unk11;
} __packed;

#define TRIGGER6_SESSION_VIDEO	0
#define TRIGGER6_SESSION_AUDIO	3

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

struct trigger6_audio_config {
	__le32 enable;
	__le32 unk1;
	__le32 unk2;	// 0x2580, same as the initial silence in bytes
	__le32 unk3;	// 0x20000
} __packed;

struct trigger6_transfer {
	struct trigger6_device *trigger6;
	struct urb *session_urb;
	struct urb *data_urb;
	/* Completed once by each of the two URBs */
	struct completion done;
};

struct trigger6_pool {
	int next;
	struct trigger6_transfer transfers[TRIGGER6_NUM_TRANSFERS];
};

struct trigger6_output {
	struct trigger6_device *trigger6;
	int index;

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct trigger6_pool pool;
};

struct trigger6_audio {
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;
	bool enabled;

	struct hrtimer timer;
	ktime_t period;
	struct trigger6_pool pool;

	/* Protected by the PCM stream lock */
	bool running;
	ktime_t start;
	snd_pcm_uframes_t sent_ptr;
	u64 frames_sent;
};

struct trigger6_device {
	struct drm_device drm;
	struct usb_interface *intf;
	/* Keeps each session next to its data on the shared bulk endpoint */
	spinlock_t bulk_lock;
	u32 pll_step_khz;
	u32 vram_size;
	struct trigger6_output outputs[TRIGGER6_NUM_OUTPUTS];
	struct trigger6_audio audio;
};

#define to_trigger6(x) container_of(x, struct trigger6_device, drm)

struct trigger6_device *trigger6_drm_create(struct usb_interface *intf);
void trigger6_drm_destroy(struct trigger6_device *trigger6);

int trigger6_read_platform(struct trigger6_device *trigger6);
int trigger6_read_vram_size(struct trigger6_device *trigger6);
int trigger6_read_connector_status(struct trigger6_device *trigger6,
				   int output);
int trigger6_read_edid(struct trigger6_device *trigger6, int output,
		       int byte_offset, void *data, int length);
int trigger6_set_resolution(struct trigger6_device *trigger6, int output,
			    const struct trigger6_mode *mode);
int trigger6_enable_output(struct trigger6_device *trigger6, int output);
int trigger6_disable_output(struct trigger6_device *trigger6, int output);
int trigger6_set_audio(struct trigger6_device *trigger6, bool enable);

int trigger6_alloc_pool(struct trigger6_device *trigger6,
			struct trigger6_pool *pool, size_t size);
void trigger6_free_pool(struct trigger6_pool *pool);
void trigger6_kill_pool(struct trigger6_pool *pool);
struct trigger6_transfer *
trigger6_get_next_transfer(struct trigger6_pool *pool);
struct trigger6_transfer *
trigger6_try_get_next_transfer(struct trigger6_pool *pool);
void trigger6_fill_session(struct trigger6_transfer *transfer, u32 number,
			   u32 dest, size_t payload_len, size_t offset,
			   size_t len);
int trigger6_submit_transfer(struct trigger6_transfer *transfer, size_t len);

bool trigger6_frame_fits(struct trigger6_output *output, int width, int height);
void trigger6_send_frame(struct trigger6_output *output,
			 struct drm_framebuffer *fb, const struct iosys_map *src,
			 const struct drm_rect *rect,
			 struct drm_format_conv_state *fmtcnv_state);

int trigger6_audio_init(struct trigger6_device *trigger6);
void trigger6_audio_disconnect(struct trigger6_device *trigger6);
void trigger6_audio_suspend(struct trigger6_device *trigger6);
void trigger6_audio_resume(struct trigger6_device *trigger6);

#endif
