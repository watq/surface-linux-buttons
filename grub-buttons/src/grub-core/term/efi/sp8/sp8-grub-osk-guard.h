/* Microsoft mu OnScreenKeyboard protocol, scoped to the D1 menu lifetime.
 * ABI/GUID checked against mu_plus 566cf5c8. No firmware variable writes or
 * driver disconnects. Keep automatic icons, self-refresh and all original
 * mode bits intact. Initialize only the verified OSK input controller. */
#ifndef SP8_GRUB_OSK_GUARD_H
#define SP8_GRUB_OSK_GUARD_H
#include <grub/efi/efi.h>
#include <grub/efi/sp8_swm.h>
#include "sp8-grub-console-leaves.h"
#include "sp8-grub-firmware-input-state.h"
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
static int d1_osk_scope_active;
static unsigned d1_osk_entry_suspended;
typedef grub_efi_status_t (__grub_efi_api *D1_OSK_SHOW) (D1_OSK *, grub_efi_boolean_t);

static void
d1_close_osk_display (const char *reason)
{
  D1_OSK *current = NULL, copy;
  grub_efi_status_t locate, keyboard_status = GRUB_EFI_UNSUPPORTED, icon_status = GRUB_EFI_UNSUPPORTED;
  grub_uint64_t begin;
  if (!d1_osk_scope_active || grub_efi_is_finished) return;
  begin = grub_get_time_ms ();
  locate = grub_efi_system_table->boot_services->locate_protocol (&d1_osk_guid, NULL, (void **) &current);
  if (locate == GRUB_EFI_SUCCESS && current && m1_refresh_map ()
      && m1_guarded_read (NULL, (grub_addr_t) current, &copy, sizeof (copy), GP_TABLE))
    {
      /* Match firmware PreExitBootServices: close the visible overlay before
         GRUB's own clear/loader frames, keeping the protocol and modes. */
      if (copy.show_icon && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.show_icon))
        icon_status = ((D1_OSK_SHOW) copy.show_icon) (current, 0);
      if (copy.show_keyboard && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.show_keyboard))
        keyboard_status = ((D1_OSK_SHOW) copy.show_keyboard) (current, 0);
    }
  m1_record ("D1 OSK DISPLAY_EXIT reason=%s keyboard_status=0x%llx icon_status=0x%llx protocol_retained=1 modes_changed=0 before_GRUB_clear=1 elapsed_ms=%llu time_ms=%llu\n",
             reason, (unsigned long long) keyboard_status, (unsigned long long) icon_status,
             (unsigned long long) (grub_get_time_ms () - begin), (unsigned long long) grub_get_time_ms ());
}

static void
d1_prepare_entry_ui (void)
{
  if (!d1_osk_scope_active || d1_osk_entry_suspended || grub_efi_is_finished) return;
  d1_stop_console_leaves (); grub_efi_console_set_direct_poll (0);
  d1_close_osk_display ("entry_selected");
  grub_efi_console_release_swm ();
  d1_osk_entry_suspended = 1;
}

static void d1_guard_osk (void);
static void
d1_resume_entry_ui (void)
{
  if (!d1_osk_entry_suspended || grub_efi_is_finished) return;
  d1_osk_entry_suspended = 0;
  d1_osk_scope_active = 0;
  d1_guard_osk ();
  m1_record ("D1 OSK ENTRY_RETURN input_and_icon_available_again=1 time_ms=%llu\n",
             (unsigned long long) grub_get_time_ms ());
}

static void
d1_restore_osk (void)
{
  d1_close_osk_display ("handoff_or_return");
  d1_touch_routes_end ();
  d1_stop_console_leaves ();
  grub_efi_console_set_direct_poll (0);
  if (!d1_osk_scope_active) return;
  grub_uint32_t actual_mode = 0; D1_OSK copy;
  grub_efi_status_t status = GRUB_EFI_UNSUPPORTED;
  D1_OSK *current = NULL;
  if (!grub_efi_is_finished && grub_efi_system_table->boot_services->locate_protocol)
    status = grub_efi_system_table->boot_services->locate_protocol (&d1_osk_guid, NULL, (void **) &current);
  if (status == GRUB_EFI_SUCCESS && current && m1_refresh_map ()
      && m1_guarded_read (NULL, (grub_addr_t) current, &copy, sizeof (copy), GP_TABLE)
      && copy.get_mode && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.get_mode))
    status = copy.get_mode (current, &actual_mode);
  else if (status == GRUB_EFI_SUCCESS) status = GRUB_EFI_UNSUPPORTED;
  m1_record ("D1 OSK RETURN original_mode=0x%08x mode_never_changed=1 actual_mode=0x%08x get_status=0x%llx mode_match=%u keyboard_preserved=1\n",
             d1_osk_saved_mode, actual_mode, (unsigned long long) status,
             status == GRUB_EFI_SUCCESS && actual_mode == d1_osk_saved_mode);
  d1_osk_scope_active = 0;
  d1_osk_entry_suspended = 0;
  grub_efi_console_release_swm ();
}

static void
d1_record_osk_geometry (const char *phase)
{
  typedef grub_efi_status_t (__grub_efi_api *D1_OSK_BOUNDS) (D1_OSK *, struct grub_efi_swm_rect *);
  struct grub_efi_swm_rect rect;
  D1_OSK copy; grub_efi_status_t status = GRUB_EFI_UNSUPPORTED; grub_uint64_t begin;
  if (!d1_osk_scope_active || grub_efi_is_finished) return;
  begin = grub_get_time_ms ();
  if (m1_guarded_read (NULL, (grub_addr_t) d1_osk, &copy, sizeof (copy), GP_TABLE)
      && copy.get_bounds && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.get_bounds))
    status = ((D1_OSK_BOUNDS) copy.get_bounds) (d1_osk, &rect);
  if (status == GRUB_EFI_SUCCESS)
    m1_record ("D1 OSK GEOMETRY phase=%s left=%u top=%u right=%u bottom=%u mode_retained=0x%x elapsed_ms=%llu time_ms=%llu visibility_not_encoded=1\n",
               phase, rect.left, rect.top, rect.right, rect.bottom, d1_osk_saved_mode,
               (unsigned long long) (grub_get_time_ms () - begin), (unsigned long long) grub_get_time_ms ());
}

static void
d1_guard_osk (void)
{
  D1_OSK copy;
  D1_OSK *protocol = NULL;
  grub_uint32_t mode = 0;
  grub_efi_status_t status;
  grub_uint64_t begin;
  if (d1_osk_scope_active || grub_efi_is_finished
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
      || !copy.get_mode
      || !gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) copy.get_mode)) {
    m1_record ("D1 OSK protocol_read_or_callable_guard_refused keyboard_unchanged=1\n");
    return;
  }
  status = copy.get_mode (protocol, &mode);
  if (status != GRUB_EFI_SUCCESS) {
    m1_record ("D1 OSK get_mode_failed status=0x%llx keyboard_unchanged=1\n", (unsigned long long) status);
    return;
  }
  d1_osk = protocol; d1_osk_get = copy.get_mode; d1_osk_set = copy.set_mode;
  d1_osk_saved_mode = mode; d1_osk_scope_active = 1;
  m1_record ("D1 OSK MODE_RETAINED mode=0x%08x auto_icon=%u self_refresh=%u mode_changed=0 elapsed_ms=%llu\n",
             mode, !!(mode & 1), !!(mode & 4), (unsigned long long) (grub_get_time_ms () - begin));
  d1_input_osk_wait_required = !d1_initialize_osk_input (protocol, &d1_osk_guid);
  d1_initialize_touch_routes ();
  {
    struct grub_efi_swm *swm = NULL, swm_copy;
    status = grub_efi_system_table->boot_services->locate_protocol (&d1_swm_guid, NULL, (void **) &swm);
    if (status == GRUB_EFI_SUCCESS && swm && m1_refresh_map ()
        && m1_guarded_read (NULL, (grub_addr_t) swm, &swm_copy, sizeof (swm_copy), GP_TABLE)
        && swm_copy.register_client && swm_copy.unregister_client && swm_copy.set_window_frame && swm_copy.blt_window
        && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) swm_copy.register_client)
        && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) swm_copy.unregister_client)
        && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) swm_copy.set_window_frame)
        && gp_memory_callable (m1_ranges, m1_range_count, (grub_addr_t) swm_copy.blt_window))
      grub_efi_console_configure_swm (swm, &swm_copy);
    else
      m1_record ("D1 SWM_CONSOLE compositor_unavailable_or_guard_refused=1 status=0x%llx keyboard_retained=1\n", (unsigned long long) status);
  }
  grub_efi_console_set_direct_poll (1);
  d1_start_console_leaves ();
  m1_record ("D1 EFI_INPUT_POLICY verified_leaf_reads OSK_preserved=1 aggregate_compatibility_when_needed=1\n");
}
#endif
