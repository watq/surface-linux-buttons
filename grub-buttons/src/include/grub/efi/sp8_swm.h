/* Microsoft mu public SWM ABI, validated by the D1 input module before use. */
#ifndef GRUB_EFI_SP8_SWM_HEADER
#define GRUB_EFI_SP8_SWM_HEADER 1
#include <grub/efi/api.h>
#include <grub/efi/graphics_output.h>
#include <grub/symbol.h>
struct grub_efi_swm;
struct grub_efi_swm_rect { grub_uint32_t left, top, right, bottom; };
typedef grub_efi_status_t (__grub_efi_api *grub_efi_swm_register_t)
  (struct grub_efi_swm *, grub_efi_handle_t, grub_uint32_t, struct grub_efi_swm_rect *,
   void *, void *, void **, grub_efi_event_t *);
typedef grub_efi_status_t (__grub_efi_api *grub_efi_swm_unregister_t)
  (struct grub_efi_swm *, grub_efi_handle_t);
typedef grub_efi_status_t (__grub_efi_api *grub_efi_swm_frame_t)
  (struct grub_efi_swm *, grub_efi_handle_t, struct grub_efi_swm_rect *);
typedef grub_efi_status_t (__grub_efi_api *grub_efi_swm_blt_t)
  (struct grub_efi_swm *, grub_efi_handle_t, struct grub_efi_gop_blt_pixel *,
   grub_efi_gop_blt_operation_t, grub_efi_uintn_t, grub_efi_uintn_t,
   grub_efi_uintn_t, grub_efi_uintn_t, grub_efi_uintn_t, grub_efi_uintn_t, grub_efi_uintn_t);
struct grub_efi_swm {
  grub_efi_swm_register_t register_client;
  grub_efi_swm_unregister_t unregister_client;
  void *activate_window;
  grub_efi_swm_frame_t set_window_frame;
  grub_efi_swm_blt_t blt_window;
  void *string_to_window, *enable_mouse_pointer, *wait_for_event;
};
/* The GRUB client stays inactive below the firmware OSK. It does not consume
   raw pointer events or replace/activate/deactivate the firmware OSK client. */
void EXPORT_FUNC(grub_efi_console_configure_swm) (struct grub_efi_swm *, const struct grub_efi_swm *);
int EXPORT_FUNC(grub_efi_console_swm_surface) (unsigned, unsigned);
grub_efi_status_t EXPORT_FUNC(grub_efi_console_swm_blt)
  (struct grub_efi_gop_blt_pixel *, unsigned, unsigned, unsigned, unsigned, unsigned);
void EXPORT_FUNC(grub_efi_console_release_swm) (void);
#endif
