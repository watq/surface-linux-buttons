/* Data-only range policy shared with host tests. Does not map memory. */
#ifndef SP8_GPIO_MEMORY_H
#define SP8_GPIO_MEMORY_H
#include "sp8-gpio-core.h"

typedef struct {
    uint64_t base, length, attributes;
    uint32_t type;
} GP_MEMORY_RANGE;

/* UEFI memory types from efidef.h, kept explicit for a portable test. */
#define GP_EFI_ACPI_RECLAIM 9u
#define GP_EFI_ACPI_NVS 10u
#define GP_EFI_MMIO 11u
#define GP_EFI_UC 1u
#define GP_EFI_RP 0x2000u
#define GP_EFI_XP 0x4000u

static inline int gp_range_contains(uint64_t base, uint64_t length,
                                    uint64_t address, size_t bytes) {
    return bytes > 0 && length > 0 && base <= UINT64_MAX - length &&
        address >= base && address - base < length &&
        bytes <= length - (address - base);
}
static inline int gp_ram_type(uint32_t type) {
    /* Loader/BootServices/RuntimeServices code and data; ACPI tables.
     * Conventional memory is not accepted as a firmware table location. */
    return (type >= 1 && type <= 6) || type == GP_EFI_ACPI_RECLAIM || type == GP_EFI_ACPI_NVS;
}
static inline int gp_memory_allows(const GP_MEMORY_RANGE *ranges, size_t count,
                                   uint64_t address, size_t bytes, enum gp_space kind,
                                   const GP_MEMORY_RANGE *gcd) {
    size_t i;
    int mapped = 0;
    if (kind > GP_MMIO || address < 0x10000 || !bytes || address > UINT64_MAX - bytes) return 0;
    for (i = 0; i < count; i++) {
        const GP_MEMORY_RANGE *m = &ranges[i];
        int contains;
        if (!m->length || m->base > UINT64_MAX - m->length) return 0;
        if (!(address < m->base + m->length && m->base < address + bytes)) continue;
        contains = gp_range_contains(m->base, m->length, address, bytes);
        if (m->attributes & GP_EFI_RP) return 0;
        if (kind == GP_NVS) {
            if (m->type != GP_EFI_ACPI_NVS) return 0;
            if (contains) mapped = 1;
            continue;
        }
        if (kind == GP_TABLE) {
            if (!gp_ram_type(m->type)) return 0;
            if (contains) mapped = 1;
            continue;
        }
        if (m->type != GP_EFI_MMIO && m->type != 0) return 0;
        if (m->type == GP_EFI_MMIO) {
            if (m->attributes & (2u | 4u | 8u | 16u)) return 0; /* conflicting cache policy */
            if (contains && (m->attributes & GP_EFI_UC)) mapped = 1;
        }
    }
    if (mapped) return 1;
    if (kind != GP_MMIO) return 0;
    /* Some firmware publishes device apertures only in the DXE GCD map.
     * Require a current uncached MMIO descriptor and no RAM overlap. */
    return gcd && gcd->type == GP_EFI_MMIO && (gcd->attributes & GP_EFI_UC) &&
        !(gcd->attributes & (2u | 4u | 8u | 16u)) &&
        !(gcd->attributes & GP_EFI_RP) &&
        gp_range_contains(gcd->base, gcd->length, address, bytes);
}
static inline int gp_memory_callable(const GP_MEMORY_RANGE *ranges, size_t count, uint64_t address) {
    size_t i;
    int found = 0;
    if (address < 0x10000) return 0;
    for (i = 0; i < count; i++) {
        const GP_MEMORY_RANGE *m = &ranges[i];
        if (!m->length || m->base > UINT64_MAX - m->length) return 0;
        if (!gp_range_contains(m->base, m->length, address, 1)) continue;
        if ((m->type != 1 && m->type != 3 && m->type != 5) ||
            (m->attributes & (GP_EFI_RP | GP_EFI_XP))) return 0;
        found = 1;
    }
    return found;
}
#endif
