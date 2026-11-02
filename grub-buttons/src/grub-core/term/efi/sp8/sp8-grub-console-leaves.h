/* Discovery of already-installed EFI keyboard endpoints.
 * ConSplitter is an aggregate, not a keyboard. Reading the installed child
 * protocols avoids its lazy connection path, but any slow child is logged
 * by the console and is not assumed fast. Initialization may connect only
 * the verified existing OSK controller once; no general console/USB scan,
 * reset, disconnect or uninstall. All original OSK modes remain intact.
 */
#ifndef SP8_GRUB_CONSOLE_LEAVES_H
#define SP8_GRUB_CONSOLE_LEAVES_H
static struct grub_efi_console_leaf d1_input_snapshot[GRUB_EFI_CONSOLE_MAX_LEAVES];
static unsigned d1_input_snapshot_count;
static int d1_input_leaves_enabled;
static unsigned d1_input_discovery_reports;
static grub_uint64_t d1_input_discovery_longest;
static unsigned d1_input_snapshot_empty_idle;
static unsigned d1_input_osk_wait_required;
static unsigned d1_input_touch_wait_required;
static unsigned d1_touch_path_requires_compatibility (void);
static unsigned d1_input_scan_count, d1_input_publish_count, d1_input_fallback_entries;
static unsigned d1_input_previous_fallback;
static grub_efi_handle_t d1_input_aggregate_handle;
static grub_efi_simple_text_input_ex_interface_t *d1_input_aggregate_ex;
static grub_efi_handle_t d1_input_previous_inventory[2][GRUB_EFI_CONSOLE_MAX_LEAVES + 1];
static unsigned d1_input_previous_inventory_count[2], d1_input_inventory_cached;
static unsigned d1_input_full_scans, d1_input_stable_checks;

/* Read the USB driver's cached interface descriptor. This API copies the
   active descriptor; it is not UsbControlTransfer or a port reset. The ABI
   and the mu HID GUID are taken from the saved official headers/source. */
typedef struct
{
  grub_uint8_t length, type, number, alternate, endpoints;
  grub_uint8_t class_code, subclass, protocol, interface_string;
} D1_USB_INTERFACE;
typedef struct d1_usb_io D1_USB_IO;
typedef grub_efi_status_t (__grub_efi_api *D1_USB_GET_INTERFACE)
  (D1_USB_IO *, D1_USB_INTERFACE *);
struct d1_usb_io
{
  void *control, *bulk, *async_interrupt, *sync_interrupt;
  void *isochronous, *async_isochronous, *device_descriptor, *config_descriptor;
  D1_USB_GET_INTERFACE get_interface;
};
_Static_assert (sizeof (D1_USB_INTERFACE) == 9, "USB interface descriptor ABI");
_Static_assert (offsetof (D1_USB_IO, get_interface) == 64, "USB IO cached interface method ABI");

/* This exception is restricted to the reviewed Surface console inventory:
   a wildcard USB boot-keyboard path and the observed vendor input/OSK paths.
   An exact keyboard path or any unfamiliar route keeps normal ConIn reads. */
static int
d1_configured_console_supports_empty_idle (void)
{
  static grub_guid_t global_guid = GRUB_EFI_GLOBAL_VARIABLE_GUID;
  static grub_guid_t vendor_guids[2] = {
    {0xbebb78d4,0xa774,0x4148,{0xae,0x8b,0x16,0x22,0xf2,0x43,0x69,0xf9}},
    {0xad603516,0xd94a,0x425e,{0x9b,0x42,0x0b,0x55,0xfd,0xdb,0xd8,0xa1}}
  };
  grub_efi_runtime_services_t *rs = grub_efi_system_table->runtime_services;
  grub_uint8_t paths[256];
  grub_efi_uintn_t size = sizeof (paths);
  grub_efi_uint32_t attributes;
  unsigned offset = 0, saw_vendor = 0, node_in_instance = 0;
  grub_efi_status_t status;
  if (!rs || !rs->get_variable) return 0;
  status = rs->get_variable ((grub_efi_char16_t *) L"ConIn", &global_guid,
                             &attributes, &size, paths);
  if (status != GRUB_EFI_SUCCESS || size < 4 || size > sizeof (paths)) return 0;
  while (offset + 4 <= size)
    {
      unsigned type = paths[offset] & 0x7f, subtype = paths[offset + 1];
      unsigned length = paths[offset + 2] | (paths[offset + 3] << 8);
      if (length < 4 || length > size - offset) return 0;
      if (type == GRUB_EFI_END_DEVICE_PATH_TYPE)
        {
          if (length != 4 || !node_in_instance) return 0;
          if (subtype == GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE)
            return offset + length == size && saw_vendor;
          if (subtype != GRUB_EFI_END_THIS_DEVICE_PATH_SUBTYPE) return 0;
          node_in_instance = 0;
        }
      else
        {
          if (node_in_instance++) return 0;
          if (type == GRUB_EFI_HARDWARE_DEVICE_PATH_TYPE
              && subtype == GRUB_EFI_VENDOR_DEVICE_PATH_SUBTYPE && length == 20)
            {
              if (!grub_memcmp (paths + offset + 4, &vendor_guids[0], sizeof (vendor_guids[0]))) saw_vendor = 1;
              else if (grub_memcmp (paths + offset + 4, &vendor_guids[1], sizeof (vendor_guids[1]))) return 0;
            }
          else if (!(type == GRUB_EFI_MESSAGING_DEVICE_PATH_TYPE
                     && subtype == GRUB_EFI_USB_CLASS_DEVICE_PATH_SUBTYPE && length == 11
                     && paths[offset + 4] == 0xff && paths[offset + 5] == 0xff
                     && paths[offset + 6] == 0xff && paths[offset + 7] == 0xff
                     && paths[offset + 8] == 3 && paths[offset + 9] == 1
                     && paths[offset + 10] == 1)) return 0;
        }
      offset += length;
    }
  return 0;
}

/* Only defer the aggregate while its published input inventory is empty
   and no known physical keyboard transport is available. An attached HID
   interface (including an unknown HID), a mu keyboard producer or any
   discovery/ABI failure retains the compatibility aggregate path. */
static int
d1_keyboard_transport_present (unsigned *known, unsigned *needs_aggregate,
                               const struct grub_efi_console_leaf *inputs, unsigned input_count)
{
  static grub_guid_t hid_keyboard_guid = {0x540937ca,0xcac9,0x4d5c,
                                          {0x83,0xa9,0xa8,0x08,0x45,0x79,0x40,0xaf}};
  static grub_guid_t usb_guid = GRUB_EFI_USB_IO_PROTOCOL_GUID;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t handles[GRUB_EFI_CONSOLE_MAX_LEAVES + 1];
  grub_efi_uintn_t bytes = sizeof (handles);
  grub_efi_status_t status;
  unsigned i, j, present = 0;
  *known = 0;
  *needs_aggregate = 1;
  status = bs->locate_handle (GRUB_EFI_BY_PROTOCOL, &hid_keyboard_guid, NULL, &bytes, handles);
  if (status != GRUB_EFI_SUCCESS && status != GRUB_EFI_NOT_FOUND) return 1;
  if (status == GRUB_EFI_SUCCESS)
    {
      if (bytes > sizeof (handles) || bytes % sizeof (handles[0])) return 1;
      for (i = 0; i < bytes / sizeof (handles[0]); i++)
        {
          present = 1;
          for (j = 0; j < input_count; j++) if (inputs[j].handle == handles[i]) break;
          if (j == input_count) { *known = 1; return 1; }
        }
    }
  bytes = sizeof (handles);
  status = bs->locate_handle (GRUB_EFI_BY_PROTOCOL, &usb_guid, NULL, &bytes, handles);
  if (status == GRUB_EFI_NOT_FOUND) { *known = 1; *needs_aggregate = 0; return present; }
  if (status != GRUB_EFI_SUCCESS || bytes > sizeof (handles)
      || bytes % sizeof (handles[0])) return 1;
  for (i = 0; i < bytes / sizeof (handles[0]); i++)
    {
      D1_USB_IO *usb = grub_efi_open_protocol (handles[i], &usb_guid,
                                              GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      D1_USB_IO copy;
      D1_USB_INTERFACE descriptor;
      grub_uint64_t begin;
      if (!usb || !m1_guarded_read (NULL, (grub_addr_t) usb, &copy, sizeof (copy), GP_TABLE)
          || !copy.get_interface
          || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.get_interface)) return 1;
      grub_memset (&descriptor, 0, sizeof (descriptor));
      begin = grub_get_time_ms ();
      status = copy.get_interface (usb, &descriptor);
      if (grub_get_time_ms () - begin > 20)
        m1_record ("D1 EFI_INPUT_PRESENCE operation=USB_cached_interface elapsed_ms=%llu status=0x%llx\n",
                   (unsigned long long) (grub_get_time_ms () - begin), (unsigned long long) status);
      if (status != GRUB_EFI_SUCCESS || descriptor.length != sizeof (descriptor) || descriptor.type != 4) return 1;
      if (descriptor.class_code == 3
          && !(descriptor.subclass == 1 && descriptor.protocol == 2))
        {
          present = 1;
          for (j = 0; j < input_count; j++) if (inputs[j].handle == handles[i]) break;
          if (j == input_count) { *known = 1; return 1; }
        }
    }
  *known = 1;
  *needs_aggregate = 0;
  return present;
}

static int
d1_input_path_readable (grub_efi_device_path_t *path)
{
  grub_addr_t address = (grub_addr_t) path;
  unsigned offset = 0, nodes;
  int has_node = 0;
  for (nodes = 0; nodes < 64 && offset <= 4096 - 4; nodes++)
    {
      grub_efi_device_path_t header;
      unsigned length, type;
      if (address > ~(grub_addr_t) 0 - offset
          || !m1_guarded_read (NULL, address + offset, &header, sizeof (header), GP_TABLE))
        return 0;
      length = header.length;
      type = header.type & 0x7f;
      if (length < 4 || length > 4096 - offset) return 0;
      if (type == GRUB_EFI_END_DEVICE_PATH_TYPE)
        {
          if (length != 4) return 0;
          if (header.subtype == GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE) return has_node;
          if (header.subtype != GRUB_EFI_END_THIS_DEVICE_PATH_SUBTYPE) return 0;
        }
      else
        has_node = 1;
      offset += length;
    }
  return 0;
}

static int
d1_input_validate (void *protocol, int extended)
{
  grub_addr_t function;
  if (extended)
    {
      grub_efi_simple_text_input_ex_interface_t copy;
      if (!m1_guarded_read (NULL, (grub_addr_t) protocol, &copy, sizeof (copy), GP_TABLE)) return 0;
      function = (grub_addr_t) copy.read_key_stroke;
    }
  else
    {
      grub_efi_simple_input_interface_t copy;
      if (!m1_guarded_read (NULL, (grub_addr_t) protocol, &copy, sizeof (copy), GP_TABLE)) return 0;
      function = (grub_addr_t) copy.read_key_stroke;
    }
  return function && gp_memory_callable (m1_ranges, m1_range_count, function);
}

/* Identification is used only to target OSK initialization and preserve its
   route. The OSK remains in the normal input snapshot and is never excluded. */
static int
d1_input_is_osk_controller (grub_efi_device_path_t *path)
{
  static grub_guid_t guid = {0xad603516,0xd94a,0x425e,
                             {0x9b,0x42,0x0b,0x55,0xfd,0xdb,0xd8,0xa1}};
  grub_uint8_t bytes[24];
  return path && m1_guarded_read (NULL, (grub_addr_t) path, bytes, 4, GP_TABLE)
         && bytes[0] == GRUB_EFI_HARDWARE_DEVICE_PATH_TYPE
         && bytes[1] == GRUB_EFI_VENDOR_DEVICE_PATH_SUBTYPE
         && bytes[2] == 20 && bytes[3] == 0
         && m1_guarded_read (NULL, (grub_addr_t) path, bytes, sizeof (bytes), GP_TABLE)
         && !grub_memcmp (bytes + 4, &guid, sizeof (guid))
         && (bytes[20] & 0x7f) == GRUB_EFI_END_DEVICE_PATH_TYPE
         && bytes[21] == GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE
         && bytes[22] == 4 && bytes[23] == 0;
}

/* Connect exactly the existing firmware OSK controller, never the wildcard
   ConIn list or a USB tree. Its already-published control protocol and exact
   device path identify the controller. Log this initialization separately so
   moving a slow call earlier cannot be mistaken for making it faster. */
static int
d1_initialize_osk_input (void *control, grub_guid_t *control_guid)
{
  static grub_guid_t ex_guid = GRUB_EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
  static grub_guid_t basic_guid = GRUB_EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_handle_t handles[GRUB_EFI_CONSOLE_MAX_LEAVES + 1];
  grub_efi_uintn_t bytes = sizeof (handles);
  grub_uint64_t begin = grub_get_time_ms (), connect_ms = 0;
  grub_efi_status_t status;
  unsigned i, connected = 0, ready = 0;
  if (!bs->locate_handle) return 0;
  status = bs->locate_handle (GRUB_EFI_BY_PROTOCOL, control_guid, NULL, &bytes, handles);
  if (status != GRUB_EFI_SUCCESS || bytes > sizeof (handles)
      || bytes % sizeof (handles[0])) return 0;
  for (i = 0; i < bytes / sizeof (handles[0]); i++)
    {
      void *ex, *basic;
      grub_efi_device_path_t *path;
      if (grub_efi_open_protocol (handles[i], control_guid,
                                  GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL) != control) continue;
      path = grub_efi_get_device_path (handles[i]);
      if (!d1_input_is_osk_controller (path)) continue;
      ex = grub_efi_open_protocol (handles[i], &ex_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      basic = grub_efi_open_protocol (handles[i], &basic_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      ready = (ex && d1_input_validate (ex, 1)) || (basic && d1_input_validate (basic, 0));
      if (!ready && bs->connect_controller)
        {
          grub_uint64_t connect_begin = grub_get_time_ms ();
          status = bs->connect_controller (handles[i], NULL, NULL, 0);
          connect_ms = grub_get_time_ms () - connect_begin;
          connected = 1;
          if (m1_refresh_map ())
            {
              ex = grub_efi_open_protocol (handles[i], &ex_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
              basic = grub_efi_open_protocol (handles[i], &basic_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
              ready = (ex && d1_input_validate (ex, 1)) || (basic && d1_input_validate (basic, 0));
            }
        }
      m1_record ("D1 OSK INPUT_INIT handle=0x%llx targeted_connect=%u recursive=0 connect_ms=%llu total_ms=%llu status=0x%llx input_ready=%u\n",
                 (unsigned long long) (grub_addr_t) handles[i], connected,
                 (unsigned long long) connect_ms,
                 (unsigned long long) (grub_get_time_ms () - begin),
                 (unsigned long long) status, ready);
      return ready;
    }
  m1_record ("D1 OSK INPUT_INIT no_verified_controller=1 input_ready=0 aggregate_compatibility_retained=1\n");
  return 0;
}

static void
d1_input_log_path (unsigned index, grub_efi_handle_t handle)
{
  static const char hex[] = "0123456789abcdef";
  grub_efi_device_path_t *path = grub_efi_get_device_path (handle);
  grub_uint8_t bytes[128];
  char encoded[257];
  unsigned used = 0, i, nodes;
  if (!path) return;
  for (nodes = 0; nodes < 32; nodes++)
    {
      grub_efi_device_path_t header;
      if (!m1_guarded_read (NULL, (grub_addr_t) path + used,
                            &header, sizeof (header), GP_TABLE)
          || header.length < 4 || header.length > sizeof (bytes) - used
          || !m1_guarded_read (NULL, (grub_addr_t) path + used,
                               bytes + used, header.length, GP_TABLE)) break;
      used += header.length;
      if ((header.type & 0x7f) == GRUB_EFI_END_DEVICE_PATH_TYPE
          && header.subtype == GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE) break;
    }
  for (i = 0; i < used; i++)
    {
      encoded[i * 2] = hex[bytes[i] >> 4];
      encoded[i * 2 + 1] = hex[bytes[i] & 15];
    }
  encoded[used * 2] = 0;
  m1_record ("D1 EFI_INPUT_DEVICE_PATH source=%u prefix_bytes=%u hex=%s\n", index, used, encoded);
}

static void
d1_refresh_console_leaves (void)
{
  static grub_guid_t ex_guid = GRUB_EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
  static grub_guid_t basic_guid = GRUB_EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
  struct grub_efi_console_leaf inputs[GRUB_EFI_CONSOLE_MAX_LEAVES];
  grub_efi_handle_t inventories[2][GRUB_EFI_CONSOLE_MAX_LEAVES + 1];
  unsigned inventory_count[2] = {0, 0};
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_simple_text_input_ex_interface_t *aggregate_ex;
  grub_efi_status_t status = GRUB_EFI_SUCCESS;
  grub_uint64_t begin;
  unsigned kind, i, j, count = 0, refused = 0, changed = 0;
  unsigned empty_idle = 0, presence_known = 0, keyboard_present = 0, console_policy_known = 0;
  unsigned osk_present = 0, fallback, keyboard_needs_aggregate = 0;
  const char *reason = "verified-leaves";
  if (!d1_input_leaves_enabled || grub_efi_is_finished) return;
  d1_input_scan_count++;
  begin = grub_get_time_ms ();
  d1_input_touch_wait_required = d1_touch_path_requires_compatibility ();
  if (!bs->locate_handle)
    {
      grub_efi_console_set_leaf_inputs (0, NULL);
      grub_efi_console_set_empty_input_policy (0);
      return;
    }
  for (kind = 0; kind < 2; kind++)
    {
      grub_efi_uintn_t bytes = sizeof (inventories[kind]);
      status = bs->locate_handle (GRUB_EFI_BY_PROTOCOL, kind ? &basic_guid : &ex_guid,
                                 NULL, &bytes, inventories[kind]);
      if (status == GRUB_EFI_NOT_FOUND) continue;
      if (status != GRUB_EFI_SUCCESS || bytes > sizeof (inventories[kind])
          || bytes % sizeof (inventories[kind][0]))
        { refused = 1; break; }
      inventory_count[kind] = bytes / sizeof (inventories[kind][0]);
    }
  if (!refused && d1_input_inventory_cached && d1_input_snapshot_count
      && !d1_input_previous_fallback && !d1_input_osk_wait_required && !d1_input_touch_wait_required
      && inventory_count[0] == d1_input_previous_inventory_count[0]
      && inventory_count[1] == d1_input_previous_inventory_count[1]
      && !grub_memcmp (inventories[0], d1_input_previous_inventory[0],
                        inventory_count[0] * sizeof (inventories[0][0]))
      && !grub_memcmp (inventories[1], d1_input_previous_inventory[1],
                        inventory_count[1] * sizeof (inventories[1][0])))
    {
      unsigned valid = 1;
      for (i = 0; i < d1_input_snapshot_count; i++)
        {
          struct grub_efi_console_leaf *leaf = &d1_input_snapshot[i];
          void *protocol = grub_efi_console_query_input_protocol (leaf->handle, !!leaf->ex);
          if (protocol != (leaf->ex ? (void *) leaf->ex : (void *) leaf->basic)
              || !d1_input_validate (protocol, !!leaf->ex))
            { valid = 0; break; }
        }
      if (valid)
        {
          unsigned transport_known, needs_aggregate;
          d1_keyboard_transport_present (&transport_known, &needs_aggregate,
                                          d1_input_snapshot, d1_input_snapshot_count);
          if (!transport_known || needs_aggregate) valid = 0;
        }
      if (valid)
        {
          d1_input_stable_checks++;
          if (d1_input_stable_checks == 1 || grub_get_time_ms () - begin > 20)
            m1_record ("D1 EFI_INPUT_STABLE inventory_unchanged=1 checked_ms=%llu scans=%u full_scans=%u stable_checks=%u publishes=%u\n",
                       (unsigned long long) (grub_get_time_ms () - begin), d1_input_scan_count,
                       d1_input_full_scans, d1_input_stable_checks, d1_input_publish_count);
          return;
        }
    }
  d1_input_full_scans++;
  if (d1_input_aggregate_handle != grub_efi_system_table->console_in_handler)
    {
      d1_input_aggregate_handle = grub_efi_system_table->console_in_handler;
      d1_input_aggregate_ex = grub_efi_open_protocol (d1_input_aggregate_handle,
                                                    &ex_guid, GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    }
  aggregate_ex = d1_input_aggregate_ex;
  grub_memset (inputs, 0, sizeof (inputs));
  for (kind = 0; kind < 2; kind++)
    {
      if (refused) break;
      for (i = 0; i < inventory_count[kind]; i++)
        {
          void *protocol;
          grub_efi_device_path_t *path;
          grub_efi_handle_t handle = inventories[kind][i];
          if (handle == grub_efi_system_table->console_in_handler) continue;
          protocol = grub_efi_console_query_input_protocol (handle, !kind);
          if (!protocol)
            {
              refused = 1;
              break;
            }
          if (protocol == (kind ? (void *) grub_efi_system_table->con_in
                                : (void *) aggregate_ex)) continue;
          for (j = 0; j < count; j++)
            if (inputs[j].handle == handle
                || protocol == (kind ? (void *) inputs[j].basic : (void *) inputs[j].ex)) break;
          if (j < count) continue; /* Prefer Ex once for each actual device. */
          path = grub_efi_get_device_path (handle);
          if (!path || !d1_input_path_readable (path)
              || !d1_input_validate (protocol, !kind) || count == GRUB_EFI_CONSOLE_MAX_LEAVES)
            {
              refused = 1;
              break;
            }
          if (d1_input_is_osk_controller (path)) osk_present = 1;
          inputs[count].handle = handle;
          if (kind) inputs[count].basic = protocol;
          else inputs[count].ex = protocol;
          count++;
        }
      if (refused) break;
    }
  d1_input_osk_wait_required = !osk_present;
  if (refused) { count = 0; reason = "inventory-or-input-guard-refused"; }
  if (d1_input_osk_wait_required)
    { count = 0; reason = "OSK-input-not-ready"; }
  if (d1_input_touch_wait_required)
    { count = 0; reason = "touch-route-needs-compatibility"; }
  if (count && !refused)
    {
      keyboard_present = d1_keyboard_transport_present (&presence_known, &keyboard_needs_aggregate,
                                                        inputs, count);
      if (!presence_known || keyboard_needs_aggregate)
        { count = 0; reason = "physical-keyboard-needs-compatibility"; }
    }
  if (!count && !refused)
    {
      if (!keyboard_needs_aggregate)
        keyboard_present = d1_keyboard_transport_present (&presence_known, &keyboard_needs_aggregate,
                                                          inputs, count);
      if (presence_known && !keyboard_present)
        console_policy_known = d1_configured_console_supports_empty_idle ();
      empty_idle = presence_known && !keyboard_present && console_policy_known
                   && !d1_input_osk_wait_required && !d1_input_touch_wait_required;
      if (!d1_input_osk_wait_required && !d1_input_touch_wait_required)
        reason = empty_idle ? "verified-empty-no-keyboard" : keyboard_present
                   ? "keyboard-transport-needs-compatibility" : "presence-or-config-unknown";
    }
  if (empty_idle != d1_input_snapshot_empty_idle) changed = 1;
  if (count != d1_input_snapshot_count) changed = 1;
  else
    for (i = 0; i < count; i++)
      if (inputs[i].handle != d1_input_snapshot[i].handle
          || inputs[i].ex != d1_input_snapshot[i].ex
          || inputs[i].basic != d1_input_snapshot[i].basic) changed = 1;
  if (changed)
    {
      grub_efi_console_set_leaf_inputs (count, inputs);
      grub_efi_console_set_empty_input_policy (empty_idle);
      d1_input_publish_count++;
    }
  grub_memcpy (d1_input_snapshot, inputs, count * sizeof (inputs[0]));
  d1_input_snapshot_count = count;
  d1_input_snapshot_empty_idle = empty_idle;
  d1_input_inventory_cached = !refused;
  if (!refused)
    for (kind = 0; kind < 2; kind++)
      {
        d1_input_previous_inventory_count[kind] = inventory_count[kind];
        grub_memcpy (d1_input_previous_inventory[kind], inventories[kind],
                      inventory_count[kind] * sizeof (inventories[kind][0]));
      }
  fallback = !count && !empty_idle;
  if (fallback && !d1_input_previous_fallback) d1_input_fallback_entries++;
  d1_input_previous_fallback = fallback;
  if (changed || d1_input_discovery_reports < 2
      || grub_get_time_ms () - begin > d1_input_discovery_longest + 20)
    {
      grub_uint64_t elapsed = grub_get_time_ms () - begin;
      m1_record ("D1 EFI_INPUT_ROUTE leaves=%u aggregate_fallback=%u discovery_ms=%llu refused=%u status=0x%llx empty_inventory_idle=%u keyboard_transport_present=%u presence_known=%u reviewed_ConIn_policy=%u OSK_input_present=%u keyboard_needs_aggregate=%u touch_compatibility_required=%u reason=%s scans=%u publishes=%u fallback_entries=%u\n",
                 count, !count && !empty_idle, (unsigned long long) elapsed, refused,
                 (unsigned long long) status, empty_idle, keyboard_present, presence_known, console_policy_known,
                 osk_present, keyboard_needs_aggregate, d1_input_touch_wait_required, reason,
                 d1_input_scan_count, d1_input_publish_count, d1_input_fallback_entries);
      if (changed || d1_input_discovery_reports == 0)
        for (i = 0; i < count; i++)
          {
            m1_record ("D1 EFI_INPUT_SOURCE index=%u kind=%s handle=0x%llx protocol=0x%llx\n",
                       i, inputs[i].ex ? "Ex" : "Basic",
                       (unsigned long long) (grub_addr_t) inputs[i].handle,
                       (unsigned long long) (grub_addr_t) (inputs[i].ex ? (void *) inputs[i].ex
                                                                                 : (void *) inputs[i].basic));
            d1_input_log_path (i, inputs[i].handle);
          }
      d1_input_discovery_reports++;
      d1_input_discovery_longest = grub_max (d1_input_discovery_longest, elapsed);
    }
}

static void
d1_start_console_leaves (void)
{
  d1_input_scan_count = d1_input_publish_count = d1_input_fallback_entries = 0;
  d1_input_previous_fallback = 0;
  d1_input_inventory_cached = 0;
  d1_input_full_scans = d1_input_stable_checks = 0;
  d1_input_aggregate_handle = NULL;
  d1_input_aggregate_ex = NULL;
  d1_input_leaves_enabled = 1;
  d1_refresh_console_leaves ();
}

static void
d1_stop_console_leaves (void)
{
  if (d1_input_leaves_enabled)
    {
      m1_record ("D1 EFI_INPUT_ROUTE_SUMMARY scans=%u full_scans=%u stable_checks=%u publishes=%u fallback_entries=%u OSK_route_preserved=1\n",
                 d1_input_scan_count, d1_input_full_scans, d1_input_stable_checks,
                 d1_input_publish_count, d1_input_fallback_entries);
      grub_efi_console_log_input_summary ();
    }
  d1_input_leaves_enabled = 0;
  d1_input_snapshot_count = 0;
  d1_input_snapshot_empty_idle = 0;
  d1_input_osk_wait_required = 0;
  d1_input_touch_wait_required = 0;
  d1_input_inventory_cached = 0;
  grub_efi_console_set_empty_input_policy (0);
  grub_efi_console_set_leaf_inputs (0, NULL);
}
#endif
