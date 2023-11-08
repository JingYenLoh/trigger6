
#ifndef TRIGGER6_H
#define TRIGGER6_H

#include <linux/mm_types.h>
#include <linux/usb.h>

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_simple_kms_helper.h>

#define DRIVER_NAME "trigger6"
#define DRIVER_DESC "Magic Control Technology Trigger 6"
#define DRIVER_DATE "20231031"

#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 1

#define TRIGGER6_ENDPOINT_BULK_IN 0x1
#define TRIGGER6_ENDPOINT_BULK_OUT 0x2
#define TRIGGER6_ENDPOINT_INTERRUPT_IN 0x3

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
} __attribute__((packed));

struct trigger6_session {
	__le32 session_number;
	__le32 payload_length;
	__le32 dest_addr;
	__le32 packet_length;
	__le32 bytes_written;
	__le32 output_index;
	__le32 unk7;
	__le32 unk8;
} __attribute__((packed));

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
} __attribute__((packed));

struct trigger6_device {
	struct drm_device drm;
	struct usb_interface *intf;
	struct device *dmadev;

	struct drm_connector connector;
	// TODO Handle more pipes
	struct drm_simple_display_pipe display_pipe;

	struct trigger6_mode modes[30];

	// TODO figure if needed
	struct completion transfer_done;
	void *transfer_buffer;
	int num_urbs;
	struct list_head urb_available_list;
	spinlock_t urb_available_list_lock;
	struct semaphore urb_available_list_sem;
};

#define to_trigger6(x) container_of(x, struct trigger6_device, drm)

int trigger6_read_byte(struct trigger6_device *trigger6, u16 address);
int trigger6_connector_init(struct trigger6_device *trigger6);
int trigger6_set_resolution(struct trigger6_device *trigger6,
			    struct trigger6_mode *mode);

int trigger6_read_modes(struct trigger6_device *trigger6, int output_index, int byte_offset, void* data, int length);
int trigger6_read_connector_status(struct trigger6_device *trigger6, int output_index);
void trigger6_free_urb(struct trigger6_device *trigger6);
int trigger6_init_urb(struct trigger6_device *trigger6, size_t total_size);
int trigger6_enable_output(struct trigger6_device *trigger6);
int trigger6_disable_output(struct trigger6_device *trigger6);

void trigger6_fb_send_rect(struct drm_framebuffer *fb,
			   const struct iosys_map *map, struct drm_rect *rect);

void trigger6_free_urb(struct trigger6_device *trigger6);
int trigger6_init_urb(struct trigger6_device *trigger6, size_t total_size);
#endif
