/* Bounded EFI input diagnostics and repair of already-published touch routes.
 * GUIDs/ABI follow Microsoft mu_plus 566cf5c8 and UEFI. Never read pointer
 * events, register a competing HID callback, reset/disconnect a controller,
 * change OSK modes or write firmware variables. Missing routes keep the
 * ordinary aggregate compatibility path, including its measured latency.
 */
#ifndef SP8_GRUB_FIRMWARE_INPUT_STATE_H
#define SP8_GRUB_FIRMWARE_INPUT_STATE_H
#define D1_FW_MAX_HANDLES 8
static grub_guid_t d1_touch_panel_guid = {0x91b1d27b,0xe126,0x48d1,{0x82,0x34,0xd2,0x8b,0x81,0xc8,0x83,0x62}};
static grub_guid_t d1_hid_pointer_guid = {0x80b5ee6e,0xcbd8,0x43ae,{0xb6,0xac,0x10,0x91,0x7b,0x25,0x35,0xb7}};
static grub_guid_t d1_absolute_pointer_guid = GRUB_EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
static grub_guid_t d1_simple_pointer_guid = GRUB_EFI_SIMPLE_POINTER_PROTOCOL_GUID;
static grub_guid_t d1_swm_guid = {0x9d400d20,0x6f35,0x4268,{0x90,0x4f,0xdc,0x04,0xb1,0x87,0x7b,0x62}};
static grub_guid_t d1_sre_guid = {0x7768969c,0x1d94,0x4d19,{0xb0,0xcf,0x9f,0x66,0xcc,0x59,0x34,0xb4}};
static grub_guid_t d1_driver_binding_guid = {0x18a031ab,0xb443,0x4d1a,{0xa5,0xc0,0x0c,0x09,0x26,0x1e,0x9f,0x71}};
static grub_guid_t d1_loaded_image_guid = GRUB_EFI_LOADED_IMAGE_GUID;
static grub_guid_t d1_pci_io_guid = {0x4cf5b200,0x68b8,0x4ca5,{0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};

struct d1_fw_handles { grub_efi_handle_t handle[D1_FW_MAX_HANDLES]; unsigned count, known; grub_efi_status_t status; };
typedef struct { void *reset, *get_state; grub_efi_event_t wait; void *mode; } D1_ABSOLUTE_PREFIX;
typedef struct { grub_uint64_t min_x, min_y, min_z, max_x, max_y, max_z; grub_uint32_t attributes; } D1_ABSOLUTE_MODE;
typedef struct { void *register_callback, *unregister_callback; } D1_HID_POINTER_PREFIX;
typedef struct { void *supported, *start, *stop; grub_uint32_t version; grub_efi_handle_t image, binding; } D1_DRIVER_PREFIX;
_Static_assert (sizeof (D1_ABSOLUTE_PREFIX) == 32, "x64 AbsolutePointer protocol");
_Static_assert (sizeof (D1_ABSOLUTE_MODE) == 56, "x64 AbsolutePointer mode");
_Static_assert (offsetof (D1_DRIVER_PREFIX, image) == 32, "x64 DriverBinding protocol");
static grub_efi_handle_t d1_touch_attempted[D1_FW_MAX_HANDLES];
static unsigned d1_touch_attempted_stage[D1_FW_MAX_HANDLES], d1_touch_connect_count;
static unsigned d1_touch_attempted_count, d1_touch_probe_enabled;
static unsigned d1_touch_last_signature, d1_touch_have_signature, d1_touch_reports;
static unsigned d1_fw_boot_context_recorded;
typedef grub_efi_status_t (__grub_efi_api *D1_PCI_READ)
  (void *, unsigned, grub_uint32_t, grub_efi_uintn_t, void *);
typedef grub_efi_status_t (__grub_efi_api *D1_PCI_LOCATION)
  (void *, grub_efi_uintn_t *, grub_efi_uintn_t *, grub_efi_uintn_t *, grub_efi_uintn_t *);
typedef struct { void *poll[2], *mem[2], *io[2]; D1_PCI_READ pci_read; void *pci_write, *other[6]; D1_PCI_LOCATION location; } D1_PCI_PREFIX;
_Static_assert (offsetof (D1_PCI_PREFIX, pci_read) == 48, "PCI IO config read ABI");
_Static_assert (offsetof (D1_PCI_PREFIX, location) == 112, "PCI IO location ABI");
static unsigned d1_touch_snapshot (const char *phase, unsigned verbose);
static struct d1_fw_handles d1_fw_handles (grub_guid_t *guid);
static int d1_fw_has_handle (const struct d1_fw_handles *, grub_efi_handle_t);
static int d1_touch_can_attempt (grub_efi_handle_t, unsigned);

/* GM15 publishes this exact BEBB input/HID-pointer endpoint only after
   the slow aggregate connection. If its controller path already exists,
   connect that single reviewed endpoint before resorting to the aggregate.
   This is not a scan of arbitrary vendor/USB devices, and the path match is
   mandatory even if LocateDevicePath reports a partial match. */
static void
d1_initialize_reviewed_pointer_controller (void)
{
  static grub_uint8_t path_bytes[24] = {
    1,4,20,0,0xd4,0x78,0xbb,0xbe,0x74,0xa7,0x48,0x41,
    0xae,0x8b,0x16,0x22,0xf2,0x43,0x69,0xf9,127,255,4,0};
  static grub_guid_t path_guid = GRUB_EFI_DEVICE_PATH_GUID;
  grub_efi_device_path_t *remaining = (void *) path_bytes, *actual;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t handle = NULL;
  grub_efi_status_t status;
  grub_uint8_t copy[24];
  grub_efi_device_path_t header;
  grub_uint64_t begin;
  if (grub_efi_is_finished || !bs->locate_device_path || !bs->connect_controller) return;
  begin = grub_get_time_ms ();
  status = bs->locate_device_path (&path_guid, &remaining, &handle);
  if (status != GRUB_EFI_SUCCESS || !handle || remaining != (void *) (path_bytes + 20)
      || handle == grub_efi_system_table->console_in_handler)
    {
      m1_record ("D1 TOUCH_CONTROLLER exact_path=BEBB78D4_GM15 locate_status=0x%llx connect_attempted=0 elapsed_ms=%llu\n",
                 (unsigned long long) status, (unsigned long long) (grub_get_time_ms () - begin));
      return;
    }
  actual = grub_efi_get_device_path (handle);
  if (!actual || !m1_guarded_read (NULL, (grub_addr_t) actual, &header, sizeof (header), GP_TABLE)
      || header.type != GRUB_EFI_HARDWARE_DEVICE_PATH_TYPE
      || header.subtype != GRUB_EFI_VENDOR_DEVICE_PATH_SUBTYPE || header.length != 20
      || !m1_guarded_read (NULL, (grub_addr_t) actual, copy, sizeof (copy), GP_TABLE)
      || grub_memcmp (copy, path_bytes, sizeof (copy)))
    {
      m1_record ("D1 TOUCH_CONTROLLER exact_path=BEBB78D4_GM15 path_guard_refused=1 connect_attempted=0\n");
      return;
    }
  {
    struct d1_fw_handles absolute = d1_fw_handles (&d1_absolute_pointer_guid);
    if (d1_fw_has_handle (&absolute, handle))
      {
        m1_record ("D1 TOUCH_CONTROLLER exact_path=BEBB78D4_GM15 absolute_already_published=1 connect_attempted=0\n");
        return;
      }
  }
  if (!d1_touch_can_attempt (handle, 1)) return;
  status = bs->connect_controller (handle, NULL, NULL, 0);
  m1_record ("D1 TOUCH_CONTROLLER exact_path=BEBB78D4_GM15 handle=0x%llx connect_attempted=1 recursive=0 status=0x%llx total_ms=%llu time_ms=%llu\n",
             (unsigned long long) (grub_addr_t) handle, (unsigned long long) status,
             (unsigned long long) (grub_get_time_ms () - begin), (unsigned long long) grub_get_time_ms ());
  m1_refresh_map ();
}

/* GM15 identifies the actual built-in panel at this exact path. Before its
   driver publishes panel/HID protocols, locate this controller without
   enumerating PCI buses and validate location + Intel THC PCI identity. */
static void
d1_initialize_reviewed_panel_controller (void)
{
  static grub_uint8_t path_bytes[22] = {2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,1,1,6,0,6,0x10,127,255,4,0};
  grub_efi_device_path_t *remaining = (void *) path_bytes;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t handle = NULL; void *protocol; D1_PCI_PREFIX copy;
  grub_efi_uintn_t segment, bus, device, function; grub_uint32_t identity = 0;
  grub_efi_status_t status; grub_uint64_t begin;
  if (grub_efi_is_finished || !bs->locate_device_path || !bs->connect_controller) return;
  begin = grub_get_time_ms ();
  status = bs->locate_device_path (&d1_pci_io_guid, &remaining, &handle);
  if (status != GRUB_EFI_SUCCESS || !handle || remaining != (void *) (path_bytes + 18))
    {
      m1_record ("D1 TOUCH_CONTROLLER exact_path=PCI0:00:10.6 locate_status=0x%llx connect_attempted=0 elapsed_ms=%llu\n",
                 (unsigned long long) status, (unsigned long long) (grub_get_time_ms () - begin));
      return;
    }
  protocol = grub_efi_open_protocol (handle, &d1_pci_io_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (!protocol || !m1_guarded_read (NULL, (grub_addr_t) protocol, &copy, sizeof (copy), GP_TABLE)
      || !copy.location || !copy.pci_read
      || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.location)
      || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.pci_read)
      || copy.location (protocol, &segment, &bus, &device, &function) != GRUB_EFI_SUCCESS
      || segment || bus || device != 0x10 || function != 6
      || copy.pci_read (protocol, 2, 0, 1, &identity) != GRUB_EFI_SUCCESS || identity != 0xa0d08086)
    {
      m1_record ("D1 TOUCH_CONTROLLER exact_path=PCI0:00:10.6 identity_or_callable_guard_refused=1 connect_attempted=0\n");
      return;
    }
  {
    struct d1_fw_handles panel = d1_fw_handles (&d1_touch_panel_guid);
    if (d1_fw_has_handle (&panel, handle))
      { m1_record ("D1 TOUCH_CONTROLLER exact_path=PCI0:00:10.6 panel_already_published=1 connect_attempted=0\n"); return; }
  }
  status = bs->connect_controller (handle, NULL, NULL, 0);
  m1_record ("D1 TOUCH_CONTROLLER exact_path=PCI0:00:10.6 identity=0x%x handle=0x%llx connect_attempted=1 recursive=0 status=0x%llx total_ms=%llu time_ms=%llu\n",
             identity, (unsigned long long) (grub_addr_t) handle, (unsigned long long) status,
             (unsigned long long) (grub_get_time_ms () - begin), (unsigned long long) grub_get_time_ms ());
  m1_refresh_map ();
}

static struct d1_fw_handles
d1_fw_handles (grub_guid_t *guid)
{
  struct d1_fw_handles result;
  grub_efi_handle_t handles[D1_FW_MAX_HANDLES + 1];
  grub_efi_uintn_t bytes = sizeof (handles);
  grub_memset (&result, 0, sizeof (result));
  result.status = GRUB_EFI_UNSUPPORTED;
  if (grub_efi_is_finished || !grub_efi_system_table->boot_services->locate_handle) return result;
  result.status = grub_efi_system_table->boot_services->locate_handle (GRUB_EFI_BY_PROTOCOL, guid, NULL, &bytes, handles);
  if (result.status == GRUB_EFI_NOT_FOUND) { result.known = 1; return result; }
  if (result.status != GRUB_EFI_SUCCESS || bytes % sizeof (handles[0])
      || bytes > sizeof (result.handle)) return result;
  result.count = bytes / sizeof (handles[0]); result.known = 1;
  grub_memcpy (result.handle, handles, bytes);
  return result;
}

static int
d1_fw_has_handle (const struct d1_fw_handles *set, grub_efi_handle_t handle)
{
  unsigned i;
  for (i = 0; i < set->count; i++) if (set->handle[i] == handle) return 1;
  return 0;
}

static void
d1_fw_hex (char *text, const grub_uint8_t *bytes, unsigned size)
{
  static const char digits[] = "0123456789abcdef";
  unsigned i;
  for (i = 0; i < size; i++) { text[2*i] = digits[bytes[i] >> 4]; text[2*i+1] = digits[bytes[i] & 15]; }
  text[2*size] = 0;
}

/* Log a guarded path prefix only; never dereference an unbounded string. */
static void
d1_fw_path_record (const char *kind, grub_efi_handle_t handle, grub_efi_device_path_t *path)
{
  grub_uint8_t bytes[256]; char hex[513]; unsigned offset = 0, nodes, complete = 0;
  for (nodes = 0; path && nodes < 24 && offset <= sizeof (bytes) - 4; nodes++)
    {
      grub_efi_device_path_t node;
      if ((grub_addr_t) path > ~(grub_addr_t) 0 - offset
          || !m1_guarded_read (NULL, (grub_addr_t) path + offset, &node, sizeof (node), GP_TABLE)
          || node.length < 4 || node.length > sizeof (bytes) - offset
          || !m1_guarded_read (NULL, (grub_addr_t) path + offset, bytes + offset, node.length, GP_TABLE)) break;
      offset += node.length;
      if ((node.type & 0x7f) == GRUB_EFI_END_DEVICE_PATH_TYPE
          && node.subtype == GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE) { complete = 1; break; }
    }
  d1_fw_hex (hex, bytes, offset);
  m1_record ("D1 FW_PATH kind=%s handle=0x%llx bytes=%u complete=%u hex=%s\n",
             kind, (unsigned long long) (grub_addr_t) handle, offset, complete, hex);
}

/* Read only public boot/console variables with a fixed-size buffer. */
static grub_efi_status_t
d1_fw_variable_record (const char *name, grub_uint8_t *data, grub_efi_uintn_t *size)
{
  static grub_guid_t global = GRUB_EFI_GLOBAL_VARIABLE_GUID;
  grub_efi_char16_t wide[16]; char hex[257]; grub_efi_uint32_t attributes = 0;
  grub_efi_runtime_services_t *rs = grub_efi_system_table->runtime_services;
  grub_efi_status_t status = GRUB_EFI_UNSUPPORTED; unsigned saved;
  m1_utf16 (wide, name, ARRAY_SIZE (wide));
  if (rs && rs->get_variable) status = rs->get_variable (wide, &global, &attributes, size, data);
  saved = status == GRUB_EFI_SUCCESS ? grub_min (*size, (grub_efi_uintn_t) 128) : 0;
  d1_fw_hex (hex, data, saved);
  m1_record ("D1 FW_VAR name=%s status=0x%llx attributes=0x%x size=%llu prefix_bytes=%u hex=%s\n",
             name, (unsigned long long) status, attributes, (unsigned long long) *size, saved, hex);
  return status;
}

static void
d1_record_firmware_boot_context (void)
{
  grub_uint8_t data[256]; grub_efi_uintn_t size;
  grub_efi_handle_t handle; unsigned depth, current = 0, current_known = 0;
  grub_uint64_t begin;
  static const char *names[] = {"BootCurrent", "BootOrder", "BootNext", "ConIn", "ConInDev"};
  if (d1_fw_boot_context_recorded || grub_efi_is_finished || !grub_env_get ("sp8_daily_active")) return;
  d1_fw_boot_context_recorded = 1; begin = grub_get_time_ms ();
  for (depth = 0; depth < ARRAY_SIZE (names); depth++)
    {
      grub_efi_status_t status;
      size = sizeof (data); status = d1_fw_variable_record (names[depth], data, &size);
      if (!depth && status == GRUB_EFI_SUCCESS && size == 2)
        { current = data[0] | (data[1] << 8); current_known = 1; }
    }
  if (current_known)
    {
      char name[9]; grub_snprintf (name, sizeof (name), "Boot%04X", current);
      size = sizeof (data); d1_fw_variable_record (name, data, &size);
    }
  m1_record ("D1 FW_CONSOLE in_handle=0x%llx basic=0x%llx out_handle=0x%llx boot_current_known=%u cold_or_warm_reset_not_encoded=1\n",
             (unsigned long long) (grub_addr_t) grub_efi_system_table->console_in_handler,
             (unsigned long long) (grub_addr_t) grub_efi_system_table->con_in,
             (unsigned long long) (grub_addr_t) grub_efi_system_table->console_out_handler, current_known);
  if (m1_refresh_map ())
    for (handle = grub_efi_image_handle, depth = 0; handle && depth < 4; depth++)
      {
        grub_efi_loaded_image_t *image, copy;
        image = grub_efi_open_protocol (handle, &d1_loaded_image_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (!image || !m1_guarded_read (NULL, (grub_addr_t) image, &copy, sizeof (copy), GP_TABLE))
          { m1_record ("D1 FW_IMAGE depth=%u handle=0x%llx loaded_image_unavailable_or_guard_refused=1\n", depth, (unsigned long long) (grub_addr_t) handle); break; }
        m1_record ("D1 FW_IMAGE depth=%u handle=0x%llx parent=0x%llx device=0x%llx image_size=%llu\n",
                   depth, (unsigned long long) (grub_addr_t) handle,
                   (unsigned long long) (grub_addr_t) copy.parent_handle,
                   (unsigned long long) (grub_addr_t) copy.device_handle, (unsigned long long) copy.image_size);
        d1_fw_path_record ("loaded_image_file", handle, copy.file_path);
        if (copy.parent_handle == handle) break;
        handle = copy.parent_handle;
      }
  m1_record ("D1 FW_BOOT_CONTEXT elapsed_ms=%llu read_only=1 time_ms=%llu\n",
             (unsigned long long) (grub_get_time_ms () - begin), (unsigned long long) grub_get_time_ms ());
  d1_touch_snapshot ("D1_entry_before_initialization", 1);
}

static int
d1_absolute_valid (grub_efi_handle_t handle, D1_ABSOLUTE_PREFIX *copy, D1_ABSOLUTE_MODE *mode)
{
  void *protocol = grub_efi_open_protocol (handle, &d1_absolute_pointer_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  return protocol && m1_guarded_read (NULL, (grub_addr_t) protocol, copy, sizeof (*copy), GP_TABLE)
         && copy->get_state && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy->get_state)
         && copy->mode && m1_guarded_read (NULL, (grub_addr_t) copy->mode, mode, sizeof (*mode), GP_TABLE)
         && mode->max_x > mode->min_x && mode->max_y > mode->min_y;
}

/* OpenProtocolInformation observes ownership; no GetState/Reset/CheckEvent on
 * physical pointers, so OSK/SWM retains its exclusive event queue. */
static unsigned
d1_absolute_swm_bound (grub_efi_handle_t handle, const struct d1_fw_handles *swm, unsigned *known)
{
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_open_protocol_information_entry_t *entries = NULL;
  grub_efi_uintn_t count = 0, i; unsigned bound = 0;
  grub_efi_status_t status;
  *known = 0;
  if (!bs->open_protocol_information || !bs->free_pool || !swm->known) return 0;
  status = bs->open_protocol_information (handle, &d1_absolute_pointer_guid, &entries, &count);
  if (status == GRUB_EFI_SUCCESS && count <= 32 && (!count || entries) && m1_refresh_map ())
    {
      *known = 1;
      for (i = 0; i < count; i++)
        {
          grub_efi_open_protocol_information_entry_t entry;
          if (!m1_guarded_read (NULL, (grub_addr_t) &entries[i], &entry, sizeof (entry), GP_TABLE))
            { *known = 0; break; }
          if (entry.attributes & GRUB_EFI_OPEN_PROTOCOL_BY_DRIVER)
            bound |= d1_fw_has_handle (swm, entry.agent_handle) ? 1 : 2;
        }
    }
  if (entries) bs->free_pool (entries);
  return bound;
}

static unsigned
d1_touch_snapshot (const char *phase, unsigned verbose)
{
  if (grub_efi_is_finished) return 1;
  grub_uint64_t begin = grub_get_time_ms ();
  struct d1_fw_handles panel = d1_fw_handles (&d1_touch_panel_guid), hid = d1_fw_handles (&d1_hid_pointer_guid);
  struct d1_fw_handles absolute = d1_fw_handles (&d1_absolute_pointer_guid), swm = d1_fw_handles (&d1_swm_guid);
  struct d1_fw_handles sre = d1_fw_handles (&d1_sre_guid), simple = d1_fw_handles (&d1_simple_pointer_guid);
  unsigned i, raw = 0, bound = 0, pending = 0, known, required, signature, changed;
  known = panel.known && hid.known && absolute.known && swm.known && sre.known && simple.known;
  for (i = 0; i < absolute.count; i++)
    {
      D1_ABSOLUTE_PREFIX copy; D1_ABSOLUTE_MODE mode; unsigned owner_known, is_bound;
      grub_efi_handle_t handle = absolute.handle[i];
      if (handle == grub_efi_system_table->console_in_handler || d1_fw_has_handle (&swm, handle)) continue;
      raw++;
      if (!d1_absolute_valid (handle, &copy, &mode))
        {
          known = 0;
          if (verbose) m1_record ("D1 TOUCH_PROVIDER phase=%s handle=0x%llx protocol_or_mode_guard_refused=1 pointer_events_not_consumed=1\n",
                                  phase, (unsigned long long) (grub_addr_t) handle);
          continue;
        }
      is_bound = d1_absolute_swm_bound (handle, &swm, &owner_known);
      if (!owner_known) known = 0;
      bound += !!(is_bound & 1);
      if (verbose)
        {
          m1_record ("D1 TOUCH_PROVIDER phase=%s handle=0x%llx panel_marker=%u HID_pointer=%u SWM_bound=%u other_driver_owner=%u owner_known=%u wait_present=%u min_xy=%llu,%llu max_xy=%llu,%llu attributes=0x%x pointer_events_not_consumed=1\n",
                     phase, (unsigned long long) (grub_addr_t) handle, d1_fw_has_handle (&panel, handle), d1_fw_has_handle (&hid, handle),
                     !!(is_bound & 1), !!(is_bound & 2), owner_known, !!copy.wait, (unsigned long long) mode.min_x, (unsigned long long) mode.min_y,
                     (unsigned long long) mode.max_x, (unsigned long long) mode.max_y, mode.attributes);
          d1_fw_path_record ("absolute_provider", handle, grub_efi_get_device_path (handle));
        }
    }
  for (i = 0; i < panel.count; i++) if (!d1_fw_has_handle (&absolute, panel.handle[i])) pending++;
  for (i = 0; i < hid.count; i++) if (!d1_fw_has_handle (&absolute, hid.handle[i]) && !d1_fw_has_handle (&panel, hid.handle[i])) pending++;
  required = !known || !swm.count || !raw || bound != raw || pending;
  signature = (known << 31) ^ (raw << 24) ^ (bound << 16) ^ (pending << 8) ^ required;
  changed = !d1_touch_have_signature || signature != d1_touch_last_signature;
  if (verbose || changed || d1_touch_reports < 2 || grub_get_time_ms () - begin > 20)
    {
      m1_record ("D1 TOUCH_ROUTE phase=%s panel_handles=%u HID_pointer_handles=%u absolute_handles=%u raw_providers=%u SWM_handles=%u SRE_handles=%u simple_pointer_handles=%u SWM_bound=%u pending_producers=%u inventory_known=%u aggregate_compatibility_required=%u probe_ms=%llu time_ms=%llu actual_touch_events_unobserved=1\n",
                 phase, panel.count, hid.count, absolute.count, raw, swm.count, sre.count, simple.count, bound, pending, known, required,
                 (unsigned long long) (grub_get_time_ms () - begin), (unsigned long long) grub_get_time_ms ());
      d1_touch_reports++;
      if (verbose || !known)
        m1_record ("D1 TOUCH_INVENTORY_STATUS panel=0x%llx HID_pointer=0x%llx absolute=0x%llx SWM=0x%llx SRE=0x%llx simple_pointer=0x%llx\n",
                   (unsigned long long) panel.status, (unsigned long long) hid.status,
                   (unsigned long long) absolute.status, (unsigned long long) swm.status,
                   (unsigned long long) sre.status, (unsigned long long) simple.status);
      if (verbose)
        for (i = 0; i < panel.count; i++) d1_fw_path_record ("touch_panel_producer", panel.handle[i], grub_efi_get_device_path (panel.handle[i]));
      if (verbose)
        for (i = 0; i < hid.count; i++) d1_fw_path_record ("HID_pointer_producer", hid.handle[i], grub_efi_get_device_path (hid.handle[i]));
    }
  d1_touch_have_signature = 1; d1_touch_last_signature = signature;
  return required;
}

static unsigned
d1_touch_path_requires_compatibility (void)
{
  return d1_touch_probe_enabled && !grub_efi_is_finished ? d1_touch_snapshot ("periodic", 0) : 0;
}

static int
d1_touch_can_attempt (grub_efi_handle_t handle, unsigned stage)
{
  unsigned i;
  for (i = 0; i < d1_touch_attempted_count; i++) if (d1_touch_attempted[i] == handle) break;
  if (i == d1_touch_attempted_count)
    {
      if (d1_touch_attempted_count == D1_FW_MAX_HANDLES) return 0;
      d1_touch_attempted[i] = handle; d1_touch_attempted_stage[i] = 0; d1_touch_attempted_count++;
    }
  if (d1_touch_attempted_stage[i] & stage) return 0;
  d1_touch_attempted_stage[i] |= stage; d1_touch_connect_count++;
  return 1;
}

static void
d1_initialize_touch_routes (void)
{
  if (grub_efi_is_finished) return;
  struct d1_fw_handles panel = d1_fw_handles (&d1_touch_panel_guid), hid = d1_fw_handles (&d1_hid_pointer_guid);
  struct d1_fw_handles swm = d1_fw_handles (&d1_swm_guid), absolute;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  unsigned kind, i; grub_uint64_t begin = grub_get_time_ms ();
  d1_touch_attempted_count = 0; d1_touch_connect_count = 0; d1_touch_have_signature = 0; d1_touch_reports = 0;
  d1_touch_probe_enabled = 1;
  d1_touch_snapshot ("before_targeted_init", 1);
  d1_initialize_reviewed_panel_controller ();
  d1_initialize_reviewed_pointer_controller ();
  panel = d1_fw_handles (&d1_touch_panel_guid); hid = d1_fw_handles (&d1_hid_pointer_guid);
  /* Only preexisting published panel/HID producers, never a PCI/USB tree. */
  for (kind = 0; kind < 2; kind++)
    {
      struct d1_fw_handles *producers = kind ? &hid : &panel;
      for (i = 0; producers->known && i < producers->count && swm.count && bs->connect_controller; i++)
        {
          grub_efi_handle_t handle = producers->handle[i]; grub_efi_device_path_t *path;
          grub_efi_status_t status; grub_uint64_t call_begin;
          if (handle == grub_efi_system_table->console_in_handler || d1_fw_has_handle (&swm, handle)) continue;
          path = grub_efi_get_device_path (handle);
          if (!path || !d1_input_path_readable (path) || d1_input_is_osk_controller (path)) continue;
          if (grub_efi_open_protocol (handle, &d1_absolute_pointer_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL)) continue;
          if (kind)
            {
              void *protocol = grub_efi_open_protocol (handle, &d1_hid_pointer_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
              D1_HID_POINTER_PREFIX producer;
              if (!protocol || !m1_guarded_read (NULL, (grub_addr_t) protocol, &producer, sizeof (producer), GP_TABLE)
                  || !producer.register_callback || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) producer.register_callback)
                  || !producer.unregister_callback || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) producer.unregister_callback)) continue;
            }
          if (!d1_touch_can_attempt (handle, 1)) continue;
          call_begin = grub_get_time_ms (); status = bs->connect_controller (handle, NULL, NULL, 0);
          m1_record ("D1 TOUCH_INIT kind=existing_%s handle=0x%llx recursive=0 elapsed_ms=%llu status=0x%llx attempt_once=1 time_ms=%llu\n",
                     kind ? "HID_pointer" : "touch_panel", (unsigned long long) (grub_addr_t) handle,
                     (unsigned long long) (grub_get_time_ms () - call_begin), (unsigned long long) status,
                     (unsigned long long) grub_get_time_ms ());
          if (!m1_refresh_map ()) break;
        }
    }
  absolute = d1_fw_handles (&d1_absolute_pointer_guid);
  /* Bind only a verified existing SWM driver to an unowned raw provider.
   * Existing BY_DRIVER owners are not displaced by SWM's exclusive open;
   * retain aggregate compatibility instead. GRUB never steals raw events. */
  for (i = 0; absolute.known && i < absolute.count && swm.count == 1 && bs->connect_controller; i++)
    {
      grub_efi_handle_t handle = absolute.handle[i], drivers[2];
      D1_ABSOLUTE_PREFIX pointer; D1_ABSOLUTE_MODE mode; D1_DRIVER_PREFIX driver;
      unsigned owner_known; void *protocol; grub_uint64_t call_begin; grub_efi_status_t status;
      if (handle == grub_efi_system_table->console_in_handler || d1_fw_has_handle (&swm, handle)
          || !d1_absolute_valid (handle, &pointer, &mode)) continue;
      if (d1_absolute_swm_bound (handle, &swm, &owner_known) || !owner_known) continue;
      if (!d1_fw_has_handle (&panel, handle) && !d1_fw_has_handle (&hid, handle)) continue;
      protocol = grub_efi_open_protocol (swm.handle[0], &d1_driver_binding_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      if (!protocol || !m1_guarded_read (NULL, (grub_addr_t) protocol, &driver, sizeof (driver), GP_TABLE)
          || driver.image != swm.handle[0] || driver.binding != swm.handle[0]
          || !driver.supported || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) driver.supported)
          || !driver.start || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) driver.start)
          || !d1_touch_can_attempt (handle, 2)) continue;
      drivers[0] = swm.handle[0]; drivers[1] = NULL;
      call_begin = grub_get_time_ms (); status = bs->connect_controller (handle, drivers, NULL, 0);
      m1_record ("D1 TOUCH_INIT kind=existing_SWM_binding handle=0x%llx driver=0x%llx recursive=0 elapsed_ms=%llu status=0x%llx attempt_once=1 time_ms=%llu\n",
                 (unsigned long long) (grub_addr_t) handle, (unsigned long long) (grub_addr_t) drivers[0],
                 (unsigned long long) (grub_get_time_ms () - call_begin), (unsigned long long) status,
                 (unsigned long long) grub_get_time_ms ());
      if (!m1_refresh_map ()) break;
    }
  d1_input_touch_wait_required = d1_touch_snapshot ("after_targeted_init", 1);
  m1_record ("D1 TOUCH_INIT_SUMMARY attempts=%u total_ms=%llu aggregate_compatibility_required=%u pointer_GetState_Reset_or_callback_registration=0 time_ms=%llu\n",
             d1_touch_connect_count, (unsigned long long) (grub_get_time_ms () - begin), d1_input_touch_wait_required,
             (unsigned long long) grub_get_time_ms ());
}

static void
d1_touch_routes_end (void)
{
  if (d1_touch_probe_enabled && !grub_efi_is_finished) d1_touch_snapshot ("handoff_or_return", 1);
  d1_touch_probe_enabled = 0;
}
#endif
