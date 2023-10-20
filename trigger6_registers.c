
#include <uapi/linux/hid.h>

#include "trigger6.h"

int trigger6_read_byte(struct trigger6_device *trigger6, u16 address)
{
	// TODO
	return 0;
}

static inline int trigger6_write_6_bytes(struct trigger6_device *trigger6,
					 u16 address, void *data)
{
	// TODO
	return 0;
}

int trigger6_power_on(struct trigger6_device *trigger6)
{
	// TODO
	return 0;
}

int trigger6_power_off(struct trigger6_device *trigger6)
{
	// TODO
	return 0;
}
int trigger6_set_resolution(struct trigger6_device *trigger6,
			    const struct trigger6_mode *mode)
{
	// TODO
	return 0;
}