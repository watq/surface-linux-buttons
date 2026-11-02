#ifndef SP8_GRUB_GPIO_TYPES_H
#define SP8_GRUB_GPIO_TYPES_H
#include <grub/types.h>
#include <stddef.h>
typedef grub_uint32_t uint32_t;
typedef grub_uint64_t uint64_t;
#ifndef UINT64_MAX
#define UINT64_MAX (~(uint64_t) 0)
#endif
#endif
