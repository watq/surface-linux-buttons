/* Microsoft mu OnScreenKeyboard protocol, scoped to the D1 menu lifetime.
 * ABI/GUID checked against mu_plus 566cf5c8. No firmware variable writes or
 * driver disconnects. Preserve every mode bit except icon auto-activation,
 * and restore the exact old mode before a boot/return. */
#ifndef SP8_GRUB_OSK_GUARD_H
#define SP8_GRUB_OSK_GUARD_H
typedef struct d1_osk_interface D1_OSK;
typedef grub_efi_status_t (__grub_efi_api *D1_OSK_GET) (D1_OSK *, grub_uint32_t *);
typedef grub_efi_status_t (__grub_efi_api *D1_OSK_SET) (D1_OSK *, grub_uint32_t);
struct d1_osk_interface {
  void *show_icon, *show_keyboard, *show_dock, *icon_position;
  void *keyboard_position, *angle, *size;
  D1_OSK_GET get_mode;
  D1_OSK_SET set_mode;
  void *get_bounds;
};
_Static_assert (offsetof (D1_OSK, get_mode) == 56, "mu OSK GetKeyboardMode offset");
_Static_assert (offsetof (D1_OSK, set_mode) == 64, "mu OSK SetKeyboardMode offset");
static grub_guid_t d1_osk_guid = {0x3c4ca20d,0xc95a,0x4b8b,{0x81,0xaf,0x94,0xa9,0x83,0x09,0x23,0xe2}};
static D1_OSK *d1_osk;
static D1_OSK_GET d1_osk_get;
static D1_OSK_SET d1_osk_set;
static grub_uint32_t d1_osk_saved_mode;
static int d1_osk_changed;

static void
d1_restore_osk (void)
{
  grub_efi_status_t status;
  if (!d1_osk_changed || grub_efi_is_finished) return;
  status = d1_osk_set (d1_osk, d1_osk_saved_mode);
  m1_record ("D1 OSK RESTORE mode=0x%08x status=0x%llx\n", d1_osk_saved_mode,
             (unsigned long long) status);
  if (status == GRUB_EFI_SUCCESS) d1_osk_changed = 0;
}

static void
d1_guard_osk (void)
{
  D1_OSK copy;
  D1_OSK *protocol = NULL;
  grub_uint32_t mode = 0;
  grub_efi_status_t status;
  grub_uint64_t begin;
  if (d1_osk_changed || grub_efi_is_finished
      || !grub_env_get ("sp8_daily_active")
      || !grub_efi_system_table->boot_services->locate_protocol) return;
  begin = grub_get_time_ms ();
  status = grub_efi_system_table->boot_services->locate_protocol (&d1_osk_guid, NULL, (void **) &protocol);
  if (status != GRUB_EFI_SUCCESS || !protocol) {
    m1_record ("D1 OSK unavailable status=0x%llx elapsed_ms=%llu keyboard_unchanged=1\n",
               (unsigned long long) status, (unsigned long long) (grub_get_time_ms () - begin));
    return;
  }
  if (!m1_refresh_map () || !m1_guarded_read (NULL, (grub_addr_t) protocol, &copy, sizeof (copy), GP_TABLE)
      || !copy.get_mode || !copy.set_mode
      || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.get_mode)
      || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.set_mode)) {
    m1_record ("D1 OSK protocol_read_or_callable_guard_refused keyboard_unchanged=1\n");
    return;
  }
  status = copy.get_mode (protocol, &mode);
  if (status != GRUB_EFI_SUCCESS) {
    m1_record ("D1 OSK get_mode_failed status=0x%llx keyboard_unchanged=1\n", (unsigned long long) status);
    return;
  }
  if (!(mode & 1)) {
    m1_record ("D1 OSK auto_icon_already_disabled mode=0x%08x\n", mode);
    return;
  }
  status = copy.set_mode (protocol, mode & ~1U);
  if (status == GRUB_EFI_SUCCESS) {
    d1_osk = protocol; d1_osk_get = copy.get_mode; d1_osk_set = copy.set_mode;
    d1_osk_saved_mode = mode; d1_osk_changed = 1;
  }
  m1_record ("D1 OSK MENU_GUARD old_mode=0x%08x menu_mode=0x%08x status=0x%llx elapsed_ms=%llu restore_before_boot=1\n",
             mode, mode & ~1U, (unsigned long long) status,
             (unsigned long long) (grub_get_time_ms () - begin));
}
#endif
