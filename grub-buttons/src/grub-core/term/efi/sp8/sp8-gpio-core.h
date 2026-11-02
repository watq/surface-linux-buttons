/* Core shared by the EFI application and host simulation. No write callback. */
#ifndef SP8_GPIO_CORE_H
#define SP8_GPIO_CORE_H
#include "sp8-grub-gpio-types.h"
#include <stddef.h>
#include "sp8-gpio-contract.h"

enum gp_space { GP_TABLE = 0, GP_NVS = 1, GP_MMIO = 2 };
typedef int (*gp_read_fn)(void *, uint64_t, void *, size_t, enum gp_space);
typedef struct {
    gp_read_fn read;
    void *context;
} GP_READER;
typedef struct {
    uint64_t bar, pad_address;
    uint32_t revision, padbar, baseline, hostsw, lock, locktx;
    uint32_t pin, pad_index, group_index;
} GP_BUTTON;
typedef struct {
    GP_BUTTON buttons[3]; /* power, volume+, volume- */
    uint64_t pnva;
    uint32_t sbrg;
    int ready;
    const char *error;
} GP_PLAN;
typedef struct {
    uint32_t since;
    unsigned initialized, armed, candidate, stable;
} GP_DEBOUNCE;

static inline uint32_t gp_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t gp_u64(const unsigned char *p) {
    return gp_u32(p) | (uint64_t)gp_u32(p + 4) << 32;
}
static inline int gp_equal(const void *av, const void *bv, size_t n) {
    const unsigned char *a = av, *b = bv;
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static inline int gp_checksum(const unsigned char *p, size_t n) {
    unsigned char sum = 0;
    size_t i;
    for (i = 0; i < n; i++) sum = (unsigned char)(sum + p[i]);
    return sum == 0;
}
static inline uint32_t gp_contract_crc(const unsigned char *p, size_t n, int dsdt) {
    uint32_t crc = ~0u;
    size_t i;
    unsigned bit;
    for (i = 0; i < n; i++) {
        unsigned char v = p[i];
        if (i == 9 || (dsdt && i >= SP8_PNVB_VALUE_OFFSET && i < SP8_PNVB_VALUE_OFFSET + 4)) v = 0;
        crc ^= v;
        for (bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static inline int gp_dsdt_contract(const unsigned char *p, size_t n, uint64_t *pnva) {
    static const unsigned char prefix[] = {8,'P','N','V','B',12};
    if (n != SP8_DSDT_LENGTH || p[8] != 2 || !gp_equal(p, "DSDT", 4) || gp_u32(p + 4) != n ||
        !gp_checksum(p, n) || !gp_equal(p + 10, sp8_dsdt_identity, sizeof(sp8_dsdt_identity)) ||
        !gp_equal(p + SP8_PNVB_VALUE_OFFSET - 6, prefix, sizeof(prefix)) ||
        !gp_equal(p + SP8_PNVA_DECLARATIONS_OFFSET, sp8_pnva_declarations, sizeof(sp8_pnva_declarations)) ||
        !gp_equal(p + SP8_PNVA_FIELD_OFFSET, sp8_pnva_field, sizeof(sp8_pnva_field)) ||
        !gp_equal(p + SP8_GPI0_DEVICE_OFFSET, sp8_gpi0_device, sizeof(sp8_gpi0_device))) return 0;
    *pnva = gp_u32(p + SP8_PNVB_VALUE_OFFSET);
    return *pnva >= 0x100000 && (*pnva & 3u) == 0;
}
static inline int gp_ssdt_contract(const unsigned char *p, size_t n) {
    return n == SP8_SSDT_LENGTH && p[8] == 2 && gp_equal(p, "SSDT", 4) && gp_u32(p + 4) == n &&
        gp_checksum(p, n) && gp_equal(p + 10, sp8_ssdt_identity, sizeof(sp8_ssdt_identity)) &&
        gp_equal(p + SP8_MSBT_DEVICE_OFFSET, sp8_msbt_device, sizeof(sp8_msbt_device));
}
static inline int gp_read32(GP_READER *r, uint64_t address, enum gp_space kind, uint32_t *value) {
    unsigned char data[4];
    if ((address & 3) || !r->read(r->context, address, data, sizeof(data), kind)) return 0;
    *value = gp_u32(data);
    return 1;
}
static inline int gp_fail(GP_PLAN *p, const char *error) { p->ready = 0; p->error = error; return 0; }
static inline int gp_pad_input(uint32_t value) {
    /* Pure GPIO, receive enabled, transmit disabled; no NMI/SMI route. */
    return value != ~0u && !(value & (0xfu << 10)) && !(value & (1u << 9)) &&
        (value & (1u << 8)) && !(value & ((1u << 17) | (1u << 18)));
}
static inline int gp_discover(GP_READER *r, uint64_t pnva, GP_PLAN *p) {
    static const uint32_t offsets[3] = {0x6e0000, 0x6a0000, 0x6a0000};
    static const uint32_t pins[3] = {3, 193, 194}, pads[3] = {3, 22, 23};
    unsigned char nvs[SP8_PNVA_SIZE];
    unsigned i;
    p->ready = 0; p->error = NULL; p->pnva = pnva;
    if (!r->read(r->context, pnva, nvs, sizeof(nvs), GP_NVS)) return gp_fail(p, "PNVA_not_readable_ACPI_NVS");
    if (((uint32_t)nvs[0] | (uint32_t)nvs[1] << 8) != 2) return gp_fail(p, "PCHS_not_reviewed_LP");
    p->sbrg = gp_u32(nvs + SP8_SBRG_OFFSET);
    if (p->sbrg < 0x80000000u || (p->sbrg & 0x00ffffffu) || p->sbrg > 0xff000000u)
        return gp_fail(p, "invalid_SBRG_alignment_or_range");
    for (i = 0; i < 3; i++) {
        GP_BUTTON *b = &p->buttons[i];
        uint32_t revision, padbar, ownership, stride, shift, offset;
        b->bar = (uint64_t)p->sbrg + offsets[i];
        b->pin = pins[i]; b->pad_index = pads[i]; b->group_index = 0;
        if (!gp_read32(r, b->bar, GP_MMIO, &revision) ||
            !gp_read32(r, b->bar + 12, GP_MMIO, &padbar)) return gp_fail(p, "GPIO_header_not_mapped_MMIO");
        b->revision = revision >> 16; b->padbar = padbar;
        if (revision == ~0u || b->revision < 0x94 || b->revision > 0x199)
            return gp_fail(p, "GPIO_revision_not_supported");
        stride = 16; /* Intel >=0x94 has four pad DWORDs; reviewed source. */
        if (padbar < 0x400 || padbar > 0xf000 || (padbar & 15))
            return gp_fail(p, "PADBAR_invalid_or_outside_resource");
        offset = padbar + pads[i] * stride;
        if (offset > 0xfffc)
            return gp_fail(p, "PADBAR_invalid_or_outside_resource");
        shift = (pads[i] % 8) * 4;
        if (!gp_read32(r, b->bar + 0x20 + (pads[i] / 8) * 4, GP_MMIO, &ownership) ||
            (ownership & (0xfu << shift))) return gp_fail(p, "pad_not_host_owned_or_unreadable");
        if (!gp_read32(r, b->bar + 0xb0, GP_MMIO, &b->hostsw) ||
            !gp_read32(r, b->bar + 0x80, GP_MMIO, &b->lock) ||
            !gp_read32(r, b->bar + 0x84, GP_MMIO, &b->locktx)) return gp_fail(p, "pad_metadata_unreadable");
        b->pad_address = b->bar + offset;
        if (!gp_read32(r, b->pad_address, GP_MMIO, &b->baseline) || !gp_pad_input(b->baseline))
            return gp_fail(p, "pad_not_GPIO_input_or_has_SMI_NMI_route");
    }
    p->ready = 1;
    return 1;
}
static inline int gp_sample(GP_READER *r, GP_PLAN *p, unsigned index, uint32_t *raw, int *pressed) {
    GP_BUTTON *b;
    uint32_t ownership, hostsw, revision, padbar;
    if (!p->ready || index >= 3) return gp_fail(p, "sample_without_valid_plan");
    b = &p->buttons[index];
    if (!gp_read32(r, b->bar, GP_MMIO, &revision) || revision == ~0u ||
        (revision >> 16) != b->revision ||
        !gp_read32(r, b->bar + 12, GP_MMIO, &padbar) || padbar != b->padbar)
        return gp_fail(p, "GPIO_controller_layout_changed_stop_sampling");
    if (!gp_read32(r, b->bar + 0x20 + (b->pad_index / 8) * 4, GP_MMIO, &ownership) ||
        (ownership & (0xfu << ((b->pad_index % 8) * 4))) ||
        !gp_read32(r, b->bar + 0xb0, GP_MMIO, &hostsw) ||
        ((hostsw ^ b->hostsw) & (1u << b->pad_index)))
        return gp_fail(p, "pad_ownership_changed_stop_sampling");
    if (!gp_read32(r, b->pad_address, GP_MMIO, raw)) return gp_fail(p, "sample_read_refused");
    if ((*raw & ~2u) != (b->baseline & ~2u) || !gp_pad_input(*raw))
        return gp_fail(p, "pad_configuration_changed_stop_sampling");
    *pressed = !(*raw & 2u); /* active-low per reviewed MSBT Linux table. */
    return 1;
}
static inline int gp_debounce(GP_DEBOUNCE *d, uint32_t ticks, unsigned pressed) {
    /* Each tick is AFTER a minimum 10ms Stall. Six equal samples >=50ms.
     * Initial held keys are suppressed until a stable release is observed. */
    if (!d->initialized) {
        d->initialized = 1; d->candidate = d->stable = pressed; d->since = ticks; return 0;
    }
    if (d->candidate != pressed) { d->candidate = pressed; d->since = ticks; }
    if ((uint32_t)(ticks - d->since) < 5) return 0;
    if (!d->armed) { if (!pressed) { d->armed = 1; d->stable = 0; } return 0; }
    if (d->stable == pressed) return 0;
    d->stable = pressed;
    return pressed ? 1 : -1;
}
#endif
