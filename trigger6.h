
#ifndef TRIGGER6_H
#define TRIGGER6_H

#include <linux/mm_types.h>
#include <linux/usb.h>

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_simple_kms_helper.h>

#define DRIVER_NAME "trigger6"
#define DRIVER_DESC "MacroSilicon USB to VGA/HDMI"
#define DRIVER_DATE "20220101"

#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 1

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

#define TRIGGER6_MAX_TRANSFER_LENGTH 65536

#define to_trigger6(x) container_of(x, struct trigger6_device, drm)

int trigger6_read_byte(struct trigger6_device *trigger6, u16 address);
int trigger6_connector_init(struct trigger6_device *trigger6);
int trigger6_set_resolution(struct trigger6_device *trigger6,
			    const struct trigger6_mode *mode);

int trigger6_read_modes(struct trigger6_device *trigger6, int output_index, int byte_offset, void* data, int length);
void trigger6_free_urb(struct trigger6_device *trigger6);
int trigger6_init_urb(struct trigger6_device *trigger6, size_t total_size);
int trigger6_power_on(struct trigger6_device *trigger6);
int trigger6_power_off(struct trigger6_device *trigger6);

void trigger6_fb_send_rect(struct drm_framebuffer *fb,
			   const struct iosys_map *map, struct drm_rect *rect);

void trigger6_free_urb(struct trigger6_device *trigger6);
int trigger6_init_urb(struct trigger6_device *trigger6, size_t total_size);
#endif
