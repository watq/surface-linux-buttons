/* Explicit SP8 GPIO input for a separately signed GRUB menu.
 * Uses the same ACPI, readable-map/GCD and pad checks as hardware-tested G1.
 * Never acknowledges interrupts, changes ownership or writes GPIO registers.
 */
#include <grub/dl.h>
#include <grub/kernel.h>
#include <grub/term.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/env.h>
#include <grub/time.h>
#include <grub/command.h>
#include <grub/loader.h>
#include <grub/efi/efi.h>
#include <grub/efi/api.h>
#include <stdarg.h>
#include "sp8-gpio-memory.h"
#include "sp8-grub-daily-log.h"
#include "sp8-grub-gpio-actions.h"

GRUB_MOD_LICENSE ("GPLv3+");

#define M1_MAX_RANGES 1024u
#define M1_MAX_TABLES 256u
#define M1_DXE_SIGNATURE 0x565245535f455844ULL
typedef struct {
  grub_efi_physical_address_t base;
  grub_efi_uint64_t length, capabilities, attributes;
  grub_efi_uint32_t type;
  grub_efi_handle_t image, device;
} M1_GCD_DESCRIPTOR;
typedef grub_efi_status_t (__grub_efi_api *M1_GCD_QUERY) (grub_efi_physical_address_t, M1_GCD_DESCRIPTOR *);
typedef struct {
  grub_efi_table_header_t hdr;
  void *add_space, *allocate_space, *free_space, *remove_space;
  M1_GCD_QUERY query;
} M1_DXE_PREFIX;
_Static_assert (sizeof (M1_GCD_DESCRIPTOR) == 56, "x64 PI GCD descriptor");
_Static_assert (offsetof (M1_DXE_PREFIX, query) == 56, "PI DXE service order");

static GP_MEMORY_RANGE m1_ranges[M1_MAX_RANGES];
static grub_size_t m1_range_count;
static M1_GCD_QUERY m1_gcd;
static GP_PLAN m1_plan;
static M1_ACTIONS m1_actions;
static unsigned m1_enabled, m1_ram_reads, m1_mmio_reads, m1_refused;
static unsigned d1_gcd_queries;
static grub_uint64_t m1_start_ms, m1_last_sample_ms, m1_last_map_ms;
static grub_uint64_t d1_max_sample_gap_ms;
static unsigned d1_sample_gap_count;
static unsigned m1_sampled;
static unsigned d1_first_poll_recorded;
static unsigned d1_first_EFI_return_seen, d1_first_post_EFI_sample_recorded;
static const char * const m1_names[3] = {"power", "volume_up", "volume_down"};
static unsigned d1_error_busy;
static unsigned d1_dispatch_slow_count[4], d1_dispatch_reports[4];
static grub_uint64_t d1_dispatch_max_ms[4];
static const char * const d1_dispatch_phases[4] = {"sample", "usb", "net", "terminal"};
static grub_guid_t m1_acpi_guid = GRUB_EFI_ACPI_20_TABLE_GUID;
static grub_guid_t m1_dxe_guid = {0x05ad34ba,0x6f02,0x4214,{0x95,0x2e,0x4d,0xa0,0x39,0x8e,0x2b,0xb9}};

static void
d1_trace_input_component (const char *phase, const char *name,
                          grub_uint64_t begin, grub_uint64_t end, int key)
{
  unsigned i;
  grub_uint64_t elapsed;
  if (!m1_log_file || grub_efi_is_finished || end < begin) return;
  elapsed = end - begin;
  if (elapsed <= 20) return;
  for (i = 0; i < ARRAY_SIZE (d1_dispatch_phases); i++)
    if (!grub_strcmp (phase, d1_dispatch_phases[i])) break;
  if (i == ARRAY_SIZE (d1_dispatch_phases)) return;
  d1_dispatch_slow_count[i]++;
  if (d1_dispatch_reports[i] < 8 || elapsed > d1_dispatch_max_ms[i] + 20)
    {
      m1_record ("D1 INPUT_COMPONENT phase=%s name=%.47s begin_ms=%llu end_ms=%llu total_ms=%llu key_class=%s diagnostic_io_after_interval=1\n",
                 phase, name, (unsigned long long) begin, (unsigned long long) end,
                 (unsigned long long) elapsed, key == GRUB_TERM_NO_KEY ? "none" : "accepted");
      d1_dispatch_reports[i]++;
    }
  d1_dispatch_max_ms[i] = grub_max (d1_dispatch_max_ms[i], elapsed);
}

static void
d1_start_input_trace (void)
{
  grub_term_input_t term;
  unsigned count = 0;
  if (!m1_log_file || grub_efi_is_finished) return;
  if (grub_term_input_trace_hook != d1_trace_input_component)
    {
      grub_memset (d1_dispatch_slow_count, 0, sizeof (d1_dispatch_slow_count));
      grub_memset (d1_dispatch_reports, 0, sizeof (d1_dispatch_reports));
      grub_memset (d1_dispatch_max_ms, 0, sizeof (d1_dispatch_max_ms));
      grub_term_input_trace_hook = d1_trace_input_component;
    }
  m1_record ("D1 INPUT_HOOKS sample=%u native_usb=%u net_idle=%u observer_after_operations=1 input_order_unchanged=1\n",
             !!grub_term_poll_input_hook, !!grub_term_poll_usb, !!grub_net_poll_cards_idle);
  FOR_ACTIVE_TERM_INPUTS (term)
    {
      if (count < 16)
        m1_record ("D1 INPUT_TERMINAL index=%u name=%.47s\n", count, term->name ? term->name : "unnamed");
      count++;
    }
  m1_record ("D1 INPUT_TERMINAL_COUNT count=%u names_truncated=%u threshold_ms=20 per_phase_initial_reports=8\n",
             count, count > 16);
}

static void
d1_stop_input_trace (void)
{
  unsigned i;
  if (grub_term_input_trace_hook != d1_trace_input_component) return;
  grub_term_input_trace_hook = NULL;
  for (i = 0; i < ARRAY_SIZE (d1_dispatch_phases); i++)
    m1_record ("D1 INPUT_COMPONENT_SUMMARY phase=%s slow_calls=%u max_ms=%llu reports=%u includes_original_operation_callbacks=1\n",
               d1_dispatch_phases[i], d1_dispatch_slow_count[i],
               (unsigned long long) d1_dispatch_max_ms[i], d1_dispatch_reports[i]);
}

#ifndef M1_LOAD32
#define M1_LOAD32(address) (*(volatile uint32_t *) (grub_addr_t) (address))
#define M1_COPY_ADDRESS(data, address, bytes) grub_memcpy ((data), (void *) (grub_addr_t) (address), (bytes))
#endif

static int
m1_refresh_map (void)
{
  grub_efi_memory_descriptor_t *map = NULL;
  grub_efi_uintn_t bytes = 0, key, stride = 0, allocated, i;
  grub_efi_uint32_t version;
  grub_efi_status_t status;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  unsigned attempt;
  if (grub_efi_is_finished) return 0;
  status = bs->get_memory_map (&bytes, map, &key, &stride, &version);
  if (status != GRUB_EFI_BUFFER_TOO_SMALL || stride < sizeof (*map) || stride > 256 || bytes > 1024 * 1024) return 0;
  for (attempt = 0; attempt < 3; attempt++) {
    allocated = bytes + 16 * stride;
    map = grub_malloc (allocated);
    if (!map) return 0;
    bytes = allocated;
    status = bs->get_memory_map (&bytes, map, &key, &stride, &version);
    if (status == GRUB_EFI_SUCCESS) break;
    grub_free (map); map = NULL;
    if (status != GRUB_EFI_BUFFER_TOO_SMALL || stride < sizeof (*map) || stride > 256 || bytes > 1024 * 1024) return 0;
  }
  if (!map || status != GRUB_EFI_SUCCESS || version != 1 || bytes > allocated ||
      stride < sizeof (*map) || stride > 256 || bytes % stride || bytes / stride > M1_MAX_RANGES) {
    grub_free (map); return 0;
  }
  for (i = 0; i < bytes / stride; i++) {
    grub_efi_memory_descriptor_t *m = (void *) ((grub_uint8_t *) map + i * stride);
    if (!m->num_pages || m->num_pages > UINT64_MAX / 4096 || (m->physical_start & 4095) ||
        m->physical_start > UINT64_MAX - m->num_pages * 4096) { grub_free (map); return 0; }
    m1_ranges[i] = (GP_MEMORY_RANGE) {m->physical_start, m->num_pages * 4096, m->attribute, m->type};
  }
  m1_range_count = bytes / stride;
  grub_free (map);
  return 1;
}

static int
m1_guarded_read (void *context, uint64_t address, void *data, size_t bytes, enum gp_space kind)
{
  GP_MEMORY_RANGE gcd, *gcdp = NULL;
  M1_GCD_DESCRIPTOR descriptor;
  (void) context;
  if (grub_efi_is_finished) { m1_refused++; return 0; }
  if (kind == GP_MMIO && (bytes != 4 || (address & 3))) { m1_refused++; return 0; }
  /* A current uncached EFI MMIO descriptor already satisfies the same
   * range policy. Query DXE GCD only when that map actually needs fallback.
   * Reserved apertures still get their current GCD descriptor for each read. */
  if (kind == GP_MMIO && m1_gcd &&
      !gp_memory_allows (m1_ranges, m1_range_count, address, bytes, kind, NULL)) {
    d1_gcd_queries++;
    grub_memset (&descriptor, 0, sizeof (descriptor));
    if (m1_gcd (address, &descriptor) == GRUB_EFI_SUCCESS && descriptor.type == 3) {
      gcd = (GP_MEMORY_RANGE) {descriptor.base, descriptor.length, descriptor.attributes, GP_EFI_MMIO};
      gcdp = &gcd;
    }
  }
  if (!gp_memory_allows (m1_ranges, m1_range_count, address, bytes, kind, gcdp)) { m1_refused++; return 0; }
  if (kind == GP_MMIO) {
    uint32_t value = M1_LOAD32 (address);
    grub_memcpy (data, &value, sizeof (value)); m1_mmio_reads++;
  } else {
    M1_COPY_ADDRESS (data, address, bytes); m1_ram_reads++;
  }
  return 1;
}

static uint64_t
m1_config_address (grub_guid_t *guid)
{
  grub_efi_system_table_t *st = grub_efi_system_table;
  grub_efi_uintn_t i;
  uint64_t result = 0;
  if (st->num_table_entries > M1_MAX_TABLES ||
      !gp_memory_allows (m1_ranges, m1_range_count, (grub_addr_t) st->configuration_table,
                        st->num_table_entries * sizeof (*st->configuration_table), GP_TABLE, NULL)) return 0;
  for (i = 0; i < st->num_table_entries; i++) {
    grub_efi_configuration_table_t t;
    grub_memcpy (&t, st->configuration_table + i, sizeof (t));
    if (!grub_memcmp (&t.vendor_guid, guid, sizeof (*guid))) {
      if (result) return 0;
      result = (grub_addr_t) t.vendor_table;
    }
  }
  return result;
}

static void
m1_find_gcd (void)
{
  uint64_t address = m1_config_address (&m1_dxe_guid);
  M1_DXE_PREFIX prefix;
  grub_efi_table_header_t *copy;
  grub_efi_uint32_t expected, actual;
  if (!address || !m1_guarded_read (NULL, address, &prefix, sizeof (prefix), GP_TABLE)) return;
  if (prefix.hdr.signature != M1_DXE_SIGNATURE || prefix.hdr.header_size < sizeof (prefix) ||
      prefix.hdr.header_size > 4096 || !prefix.query) return;
  copy = grub_malloc (prefix.hdr.header_size);
  if (!copy) return;
  if (!m1_guarded_read (NULL, address, copy, prefix.hdr.header_size, GP_TABLE)) { grub_free (copy); return; }
  expected = copy->crc32; copy->crc32 = 0;
  if (grub_efi_system_table->boot_services->calculate_crc32 (copy, prefix.hdr.header_size, &actual) == GRUB_EFI_SUCCESS &&
      actual == expected && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) prefix.query))
    m1_gcd = prefix.query;
  grub_free (copy);
}

static unsigned char *
m1_acpi_table (uint64_t address, const char *signature, grub_size_t limit, grub_size_t *length)
{
  unsigned char header[36], *data;
  uint32_t bytes;
  if (!m1_guarded_read (NULL, address, header, sizeof (header), GP_TABLE)) return NULL;
  bytes = gp_u32 (header + 4);
  if (!gp_equal (header, signature, 4) || bytes < sizeof (header) || bytes > limit) return NULL;
  data = grub_malloc (bytes);
  if (!data) return NULL;
  if (!m1_guarded_read (NULL, address, data, bytes, GP_TABLE) || !gp_checksum (data, bytes)) {
    grub_free (data); return NULL;
  }
  *length = bytes;
  return data;
}

static int
m1_find_pnva (uint64_t *pnva)
{
  unsigned char rsdp[36], header[36], *xsdt, *dsdt, *ssdt, *fadt;
  uint64_t address = m1_config_address (&m1_acpi_guid), dsdt_address = 0;
  grub_size_t xlength, length, i;
  int dsdt_ok = 0, ssdt_ok = 0, fadt_seen = 0;
  if (!address || !m1_guarded_read (NULL, address, rsdp, sizeof (rsdp), GP_TABLE) ||
      !gp_equal (rsdp, "RSD PTR ", 8) || rsdp[15] < 2 || gp_u32 (rsdp + 20) != 36 ||
      !gp_checksum (rsdp, 20) || !gp_checksum (rsdp, sizeof (rsdp))) return 0;
  xsdt = m1_acpi_table (gp_u64 (rsdp + 24), "XSDT", 36 + 8 * M1_MAX_TABLES, &xlength);
  if (!xsdt || (xlength - 36) % 8) { grub_free (xsdt); return 0; }
  for (i = 36; i < xlength; i += 8) {
    address = gp_u64 (xsdt + i);
    if (!m1_guarded_read (NULL, address, header, sizeof (header), GP_TABLE)) continue;
    if (gp_equal (header, "FACP", 4)) {
      if (fadt_seen++) { grub_free (xsdt); return 0; }
      fadt = m1_acpi_table (address, "FACP", 4096, &length);
      if (!fadt || length < 44) { grub_free (fadt); grub_free (xsdt); return 0; }
      if (length >= 148) dsdt_address = gp_u64 (fadt + 140);
      if (!dsdt_address) dsdt_address = gp_u32 (fadt + 40);
      grub_free (fadt);
    } else if (gp_equal (header, "SSDT", 4) && gp_u32 (header + 4) == SP8_SSDT_LENGTH) {
      ssdt = m1_acpi_table (address, "SSDT", SP8_SSDT_LENGTH, &length);
      if (ssdt) { if (gp_ssdt_contract (ssdt, length)) ssdt_ok++; grub_free (ssdt); }
    }
  }
  grub_free (xsdt);
  if (dsdt_address) {
    dsdt = m1_acpi_table (dsdt_address, "DSDT", SP8_DSDT_LENGTH, &length);
    if (dsdt) { dsdt_ok = gp_dsdt_contract (dsdt, length, pnva); grub_free (dsdt); }
  }
  return dsdt_ok && ssdt_ok == 1;
}

static void
m1_summary (const char *reason)
{
  unsigned i;
  m1_record ("M1 STOP reason=%s RAM=%u MMIO=%u refused=%u\n", reason, m1_ram_reads, m1_mmio_reads, m1_refused);
  m1_record ("D1 INPUT SAMPLE_GAPS count=%u max_gap_ms=%llu\n", d1_sample_gap_count,
             (unsigned long long) d1_max_sample_gap_ms);
  for (i = 0; i < 3; i++)
    m1_record ("M1 SUMMARY button=%s pressed=%u released=%u delivered=%u\n", m1_names[i],
               m1_actions.pressed_count[i], m1_actions.released_count[i], m1_actions.delivered_count[i]);
}

static void m1_sample_nonblocking (void);
#include "sp8-grub-osk-guard.h"

static void
m1_stop (const char *reason)
{
  d1_stop_input_trace ();
  d1_restore_osk ();
  if (grub_term_poll_input_hook == m1_sample_nonblocking)
    grub_term_poll_input_hook = NULL;
  m1_enabled = 0; m1_plan.ready = 0;
  m1_actions.count = 0; m1_actions.power_pending = 0;
  m1_actions.return_chord_active = m1_actions.return_chord_sent = 0;
  m1_summary (reason);
  grub_env_set ("sp8_gpio_ready", "0");
}

/* Error text is saved at the actual GRUB print boundary, including loader
 * errors. The writer uses only fixed buffers and EFI calls, not grub_error. */
static void
d1_log_error (grub_err_t error, const char *message)
{
  if (d1_error_busy || grub_efi_is_finished) return;
  d1_error_busy = 1;
  m1_record ("D1 ERROR errno=%d message=%s chosen=%s\n", error, message,
             grub_env_get ("chosen") ? grub_env_get ("chosen") : "none");
  d1_error_busy = 0;
}

static void
m1_sample_nonblocking (void)
{
  GP_READER reader = {m1_guarded_read, NULL};
  uint32_t raw[3], tick;
  unsigned pressed[3], i;
  int event[3];
  grub_uint64_t now_ms;
  grub_uint64_t maintenance_ms = 0, map_ms = 0, route_ms = 0, gap_ms = 0;
  grub_uint64_t previous_log_io_ms = 0, previous_log_io_end_ms = 0;
  if (!m1_enabled || grub_efi_is_finished) return;
  now_ms = grub_get_time_ms ();
  if (!d1_first_poll_recorded) {
    m1_record ("D1 PERF FIRST_GPIO_POLL enabled_to_poll_ms=%llu time_ms=%llu\n",
               (unsigned long long) (now_ms - m1_start_ms), (unsigned long long) now_ms);
    d1_first_poll_recorded = 1;
  }
  if (now_ms < m1_last_sample_ms || now_ms - m1_start_ms > 0xffffffffULL * 10) {
    m1_stop ("clock_out_of_range"); return;
  }
  if (!m1_sampled || now_ms - m1_last_sample_ms >= 10) {
    if (now_ms - m1_last_map_ms >= 1000) {
      grub_uint64_t maintenance_begin = grub_get_time_ms (), map_done, input_done;
      if (!m1_refresh_map ()) { m1_stop ("memory_map_unavailable"); return; }
      map_done = grub_get_time_ms ();
      d1_refresh_console_leaves ();
      input_done = grub_get_time_ms ();
      m1_last_map_ms = input_done;
      map_ms = map_done - maintenance_begin;
      route_ms = input_done - map_done;
      maintenance_ms = input_done - maintenance_begin;
      /* Refresh can dispatch firmware callbacks. Timestamp actual sampling
         afterwards, rather than report the time taken before that call. */
      now_ms = grub_get_time_ms ();
    }
    if (!m1_enabled || grub_efi_is_finished) return;
    if (now_ms < m1_last_sample_ms || now_ms - m1_start_ms > 0xffffffffULL * 10)
      { m1_stop ("clock_out_of_range"); return; }
    /* Check the real sample gap after maintenance. Its EFI callbacks may
       have blocked despite an apparently short gap at function entry. */
    if (m1_sampled && now_ms - m1_last_sample_ms > 100) {
      gap_ms = now_ms - m1_last_sample_ms;
      previous_log_io_ms = m1_log_last_io_ms;
      previous_log_io_end_ms = m1_log_last_io_end_ms;
      d1_sample_gap_count++;
      d1_max_sample_gap_ms = grub_max (d1_max_sample_gap_ms, gap_ms);
      grub_memset (m1_actions.debounce, 0, sizeof (m1_actions.debounce));
      m1_actions.power_pending = 0;
      m1_actions.return_chord_active = 0;
      m1_actions.return_chord_sent = 1;
    }
    tick = (uint32_t) ((now_ms - m1_start_ms) / 10);
    m1_sampled = 1; m1_last_sample_ms = now_ms;
    for (i = 0; i < 3; i++) {
      int value;
      if (!gp_sample (&reader, &m1_plan, i, &raw[i], &value)) {
        m1_stop (m1_plan.error ? m1_plan.error : "sample_failed"); return;
      }
      pressed[i] = value;
    }
    if (!m1_actions_update (&m1_actions, tick, pressed, event)) {
      m1_stop ("input_queue_full"); return;
    }
    /* Log after capturing the pins so file I/O is not included in their
       timestamp and cannot create a false newly debounced transition. */
    if (maintenance_ms > 20)
      m1_record ("D1 INPUT MAINTENANCE memory_map_ms=%llu route_ms=%llu total_ms=%llu time_ms=%llu\n",
                 (unsigned long long) map_ms, (unsigned long long) route_ms,
                 (unsigned long long) maintenance_ms, (unsigned long long) now_ms);
    if (gap_ms)
      m1_record ("D1 INPUT SAMPLE_GAP gap_ms=%llu confirmed_keys_retained=%u time_ms=%llu last_log_io_ms=%llu last_log_io_end_ms=%llu\n",
                 (unsigned long long) gap_ms, m1_actions.count, (unsigned long long) now_ms,
                 (unsigned long long) previous_log_io_ms, (unsigned long long) previous_log_io_end_ms);
    for (i = 0; i < 3; i++) if (event[i])
      m1_record ("GPIO EVENT button=%s state=%s sample_tick=%u raw=0x%08x\n", m1_names[i],
                 event[i] > 0 ? "pressed" : "released", tick, raw[i]);
    if (d1_first_EFI_return_seen && !d1_first_post_EFI_sample_recorded) {
      d1_first_post_EFI_sample_recorded = 1;
      m1_record ("D1 PERF FIRST_SAMPLE_AFTER_EFI_RETURN sample_tick=%u time_ms=%llu\n",
                 tick, (unsigned long long) now_ms);
    }
  }
  m1_log_poll ();
}

static int
m1_terminal_getkey (struct grub_term_input *term __attribute__ ((unused)))
{
  unsigned button = 0;
  int key;
  m1_sample_nonblocking ();
  if (!m1_enabled || grub_efi_is_finished) return GRUB_TERM_NO_KEY;
  key = m1_actions_pop (&m1_actions, &button);
  if (key != GRUB_TERM_NO_KEY)
    m1_record ("M1 DELIVER button=%s key=%s sample_tick=%u\n", m1_names[button],
               key == '\r' ? "ENTER" : key == GRUB_TERM_ESC ? "BACK" : key == GRUB_TERM_KEY_UP ? "UP" : "DOWN",
               (uint32_t) ((grub_get_time_ms () - m1_start_ms) / 10));
  return key;
}

static grub_err_t
m1_enable (void)
{
  GP_READER reader = {m1_guarded_read, NULL};
  uint64_t pnva = 0;
  grub_uint64_t begin, map_done, gcd_done, acpi_done, discover_done, log_done;
  if (m1_enabled) return GRUB_ERR_NONE;
  if (grub_efi_is_finished) return grub_error (GRUB_ERR_BAD_DEVICE, "EFI services already finished");
  grub_memset (&m1_plan, 0, sizeof (m1_plan));
  grub_memset (&m1_actions, 0, sizeof (m1_actions));
  m1_gcd = NULL; m1_sampled = 0;
  d1_first_poll_recorded = 0;
  d1_first_EFI_return_seen = d1_first_post_EFI_sample_recorded = 0;
  d1_sample_gap_count = 0; d1_max_sample_gap_ms = 0;
  m1_ram_reads = m1_mmio_reads = m1_refused = d1_gcd_queries = 0;
  m1_log_start ();
  begin = grub_get_time_ms ();
  if (!m1_refresh_map ()) {
    m1_stop ("memory_map_unavailable"); return grub_error (GRUB_ERR_BAD_DEVICE, "M1 GPIO memory map unavailable");
  }
  map_done = grub_get_time_ms ();
  m1_find_gcd ();
  gcd_done = grub_get_time_ms ();
  if (!m1_find_pnva (&pnva)) {
    m1_stop ("reviewed_ACPI_contract_not_matched");
    return grub_error (GRUB_ERR_BAD_DEVICE, "M1 GPIO ACPI preflight refused; keyboard remains available");
  }
  acpi_done = grub_get_time_ms ();
  if (!m1_refresh_map () || !gp_discover (&reader, pnva, &m1_plan)) {
    m1_stop (m1_plan.error ? m1_plan.error : "memory_map_refresh_failed");
    return grub_error (GRUB_ERR_BAD_DEVICE, "M1 GPIO resource preflight refused; keyboard remains available");
  }
  discover_done = grub_get_time_ms ();
  /* Keep the same three newline-delimited records, with one EFI flush for
   * this known startup group instead of three slow synchronous flushes. */
  m1_record ("GPIO SOURCE button=power pin=%u pad=%u address=0x%016llx baseline=0x%08x\n"
             "GPIO SOURCE button=volume_up pin=%u pad=%u address=0x%016llx baseline=0x%08x\n"
             "GPIO SOURCE button=volume_down pin=%u pad=%u address=0x%016llx baseline=0x%08x\n",
             m1_plan.buttons[0].pin, m1_plan.buttons[0].pad_index, (unsigned long long) m1_plan.buttons[0].pad_address, m1_plan.buttons[0].baseline,
             m1_plan.buttons[1].pin, m1_plan.buttons[1].pad_index, (unsigned long long) m1_plan.buttons[1].pad_address, m1_plan.buttons[1].baseline,
             m1_plan.buttons[2].pin, m1_plan.buttons[2].pad_index, (unsigned long long) m1_plan.buttons[2].pad_address, m1_plan.buttons[2].baseline);
  log_done = grub_get_time_ms ();
  m1_start_ms = m1_last_sample_ms = m1_last_map_ms = grub_get_time_ms ();
  m1_enabled = 1;
  grub_term_poll_input_hook = m1_sample_nonblocking;
  grub_term_poll_input ();
  grub_env_set ("sp8_gpio_ready", "1");
  m1_record ("M1 GPIO READY volume_up=UP volume_down=DOWN power=ENTER_on_short_release keyboard=unchanged\n");
  m1_record ("D1 PERF GPIO_INIT map_ms=%llu gcd_ms=%llu acpi_ms=%llu discover_ms=%llu source_log_ms=%llu total_ms=%llu\n",
             (unsigned long long) (map_done - begin), (unsigned long long) (gcd_done - map_done),
             (unsigned long long) (acpi_done - gcd_done), (unsigned long long) (discover_done - acpi_done),
             (unsigned long long) (log_done - discover_done), (unsigned long long) (log_done - begin));
  m1_record ("D1 PERF GCD_QUERY_COUNT discovery=%u fallback_only=1\n", d1_gcd_queries);
  if (!grub_env_get ("sp8_daily_quiet"))
    grub_printf ("M1 GPIO READY: volume+ UP, volume- DOWN, short power release ENTER.\n");
  d1_guard_osk ();
  grub_term_poll_input ();
  return GRUB_ERR_NONE;
}

static grub_err_t
m1_command (grub_command_t command __attribute__ ((unused)), int argc, char **argv)
{
  if (argc == 1 && !grub_strcmp (argv[0], "prepare-entry")) {
    m1_record ("D1 ENTRY_UI_PREPARE before_GRUB_clear=1 GPIO_ready=%u OSK_scope=%u time_ms=%llu\n",
               m1_enabled, d1_osk_scope_active, (unsigned long long) grub_get_time_ms ());
    d1_prepare_entry_ui (); return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "resume-entry")) { d1_resume_entry_ui (); return GRUB_ERR_NONE; }
  if (argc == 1 && !grub_strcmp (argv[0], "on")) return m1_enable ();
  if (argc == 1 && !grub_strcmp (argv[0], "off")) {
    m1_stop ("user_off"); m1_log_finish ("user_off"); return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "log-start")) {
    if (!m1_log_file) m1_log_attempted = 0;
    m1_log_start ();
    m1_record ("D1 BOOT entry=daily signed_embedded_version=D1 time_ms=%llu\n",
               (unsigned long long) grub_get_time_ms ());
    d1_record_firmware_boot_context ();
    return GRUB_ERR_NONE;
  }
  if (argc == 2 && !grub_strcmp (argv[0], "log")) {
    if (!d1_first_EFI_return_seen && grub_strlen (argv[1]) >= 20
        && !grub_memcmp (argv[1], "EFI_INPUT operation=", 20)) d1_first_EFI_return_seen = 1;
    m1_record ("D1 PHASE %s time_ms=%llu\n", argv[1],
               (unsigned long long) grub_get_time_ms ());
    if (grub_strlen (argv[1]) >= 16 && !grub_memcmp (argv[1], "MENU_INPUT_READY", 16))
      {
        d1_record_osk_geometry ("menu_input_ready");
        d1_start_input_trace ();
      }
    return GRUB_ERR_NONE;
  }
  if (argc == 2 && !grub_strcmp (argv[0], "log-end")) {
    m1_stop (argv[1]); m1_log_finish (argv[1]); return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "back")) {
    m1_record ("D1 RETURN target=parent chosen=%s\n", grub_env_get ("chosen") ? grub_env_get ("chosen") : "none");
    grub_env_set ("sp8_daily_back_request", "1");
    return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "selected")) {
    const char *chosen = grub_env_get ("chosen");
    m1_record ("D1 SYSTEM SELECT chosen=%s GPIO_ready=%u delivered_up=%u delivered_down=%u delivered_enter=%u\n",
               chosen ? chosen : "unknown", m1_enabled,
               m1_actions.delivered_count[1], m1_actions.delivered_count[2], m1_actions.delivered_count[0]);
    return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "status")) {
    unsigned i;
    grub_printf ("M1 GPIO ready=%u log=%s\n", m1_enabled, m1_log_name);
    grub_printf ("M1 THEME rendered=%s size=%sx%s\n",
                 grub_env_get ("sp8_m1_theme_rendered") ? grub_env_get ("sp8_m1_theme_rendered") : "0",
                 grub_env_get ("sp8_m1_theme_width") ? grub_env_get ("sp8_m1_theme_width") : "0",
                 grub_env_get ("sp8_m1_theme_height") ? grub_env_get ("sp8_m1_theme_height") : "0");
    for (i = 0; i < 3; i++)
      grub_printf ("%s pressed=%u released=%u delivered=%u\n", m1_names[i], m1_actions.pressed_count[i],
                    m1_actions.released_count[i], m1_actions.delivered_count[i]);
    return GRUB_ERR_NONE;
  }
  if (argc == 2 && !grub_strcmp (argv[0], "mark") &&
      (!grub_strcmp (argv[1], "A") || !grub_strcmp (argv[1], "B") ||
       !grub_strcmp (argv[1], "C") || !grub_strcmp (argv[1], "RETURN"))) {
    m1_record ("M1 MENU SELECT item=%s GPIO_ready=%u delivered_up=%u delivered_down=%u delivered_enter=%u\n",
               argv[1], m1_enabled, m1_actions.delivered_count[1], m1_actions.delivered_count[2], m1_actions.delivered_count[0]);
    return GRUB_ERR_NONE;
  }
  return grub_error (GRUB_ERR_BAD_ARGUMENT, "sp8_gpio on|off|status|selected|back|log-start|log TEXT|log-end REASON");
}

static unsigned d1_resume_gpio, d1_resume_log;
static char d1_previous_log[16];
static grub_err_t
m1_preboot (int flags)
{
  d1_resume_gpio = m1_enabled;
  d1_resume_log = !!m1_log_file;
  grub_strcpy (d1_previous_log, m1_log_name);
  m1_record ("D1 BOOT HANDOFF chosen=%s loader_flags=%d GPIO_ready=%u\n",
             grub_env_get ("chosen") ? grub_env_get ("chosen") : "command-line", flags, m1_enabled);
  m1_stop ("system_boot");
  m1_log_finish ("system_boot");
  return GRUB_ERR_NONE;
}
static grub_err_t
m1_restore (void)
{
  if (grub_efi_is_finished) return GRUB_ERR_NONE;
  if (d1_resume_log) {
    d1_resume_log = 0; m1_log_attempted = 0;
    m1_log_start ();
    m1_record ("D1 BOOT RETURN previous_log=%s chosen=%s errno=%d message=%s\n", d1_previous_log,
               grub_env_get ("chosen") ? grub_env_get ("chosen") : "command-line", grub_errno, grub_errmsg);
  }
  if (d1_resume_gpio) {
    grub_err_t result;
    grub_error_push ();
    d1_resume_gpio = 0;
    result = m1_enable ();
    if (result) m1_record ("D1 GPIO RESTORE refused_errno=%d keyboard_available=1\n", result);
    grub_error_pop ();
  }
  /* Diagnostics never turn a successful EFI return into a boot error. */
  return GRUB_ERR_NONE;
}
static struct grub_preboot *d1_preboot_hook;

static struct grub_term_input m1_term = {.name = "sp8_gpio", .getkey = m1_terminal_getkey};
static grub_command_t m1_cmd;
GRUB_MOD_INIT (sp8_gpio)
{
  m1_cmd = grub_register_command ("sp8_gpio", m1_command, "on|off|status|mark A|B|C|RETURN", "Guarded SP8 GPIO menu input");
  grub_term_register_input_inactive ("sp8_gpio", &m1_term);
  d1_preboot_hook = grub_loader_register_preboot_hook (m1_preboot, m1_restore, GRUB_LOADER_PREBOOT_HOOK_PRIO_NORMAL);
  grub_sp8_error_hook = d1_log_error;
}
GRUB_MOD_FINI (sp8_gpio)
{
  d1_stop_input_trace ();
  if (grub_term_poll_input_hook == m1_sample_nonblocking)
    grub_term_poll_input_hook = NULL;
  if (grub_sp8_error_hook == d1_log_error) grub_sp8_error_hook = NULL;
  if (!grub_efi_is_finished) { m1_stop ("module_unload"); m1_log_finish ("module_unload"); }
  grub_term_unregister_input (&m1_term);
  if (d1_preboot_hook) grub_loader_unregister_preboot_hook (d1_preboot_hook);
  if (m1_cmd) grub_unregister_command (m1_cmd);
}
