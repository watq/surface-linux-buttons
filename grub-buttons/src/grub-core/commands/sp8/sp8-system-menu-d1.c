/* Import the current generated system menu into a separately signed GRUB.
 * Menu entries keep their original kernel paths, classes, IDs and bodies.
 * Add only diagnostic source markers; original entry list remains unchanged.
 */
#include <grub/dl.h>
#include <grub/command.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/menu.h>
#include <grub/normal.h>
#include <grub/script_sh.h>
#include <grub/term.h>
#include <grub/font.h>
#include <grub/video.h>
#include <stddef.h>
#include "sp8-system-config-filter.h"

GRUB_MOD_LICENSE ("GPLv3+");

static void
d1_phase (const char *phase)
{
  grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", (char *) phase});
}

static grub_err_t
d1_wrap_entries (grub_menu_t menu)
{
  grub_menu_entry_t entry;
  if (!menu) return grub_error (GRUB_ERR_BAD_ARGUMENT, "D1 system menu is missing");
  for (entry = menu->entry_list; entry; entry = entry->next) {
    const char marker[] = "sp8_gpio selected\n";
    const char suffix[] = "\nsp8_system_menu wrap-sub\n";
    char *wrapped;
    grub_size_t length, extra;
    if (!entry->sourcecode ||
        !grub_strncmp (entry->sourcecode, marker, sizeof (marker) - 1)) continue;
    length = grub_strlen (entry->sourcecode);
    extra = sizeof (marker) + (entry->submenu ? sizeof (suffix) : 0);
    if (length > 4 * 1024 * 1024 || length > (~(grub_size_t) 0) - extra)
      return grub_error (GRUB_ERR_OUT_OF_RANGE, "D1 menu entry too large");
    wrapped = grub_malloc (length + extra);
    if (!wrapped) return grub_errno;
    grub_memcpy (wrapped, marker, sizeof (marker) - 1);
    grub_memcpy (wrapped + sizeof (marker) - 1, entry->sourcecode, length + 1);
    if (entry->submenu)
      grub_memcpy (wrapped + sizeof (marker) - 1 + length, suffix, sizeof (suffix));
    /* commands/menuentry.c owns the allocated source body. */
    grub_free ((void *) entry->sourcecode);
    entry->sourcecode = wrapped;
  }
  return GRUB_ERR_NONE;
}

static grub_err_t
d1_wrap_menu (int nested)
{
  grub_menu_t menu = grub_env_get_menu ();
  grub_err_t result = d1_wrap_entries (menu);
  if (result) return result;
  d1_phase (nested ? "SUBMENU_ORIGINAL_ENTRIES_WRAPPED" : "ROOT_ORIGINAL_ENTRIES_WRAPPED");
  return GRUB_ERR_NONE;
}

static grub_err_t
d1_load_system_config (const char *path)
{
  grub_file_t file;
  grub_size_t bytes;
  char *text;
  unsigned skipped;
  grub_menu_t menu = grub_env_get_menu ();
  int count_before = menu ? menu->size : 0;
  grub_err_t error;
  char marker[384];
  grub_uint32_t fingerprint = 2166136261U;
  grub_size_t source_bytes, i;
  int errors_before;
  grub_snprintf (marker, sizeof (marker), "CONFIG_IMPORT_BEGIN path=%s", path);
  d1_phase (marker);
  file = grub_file_open (path, GRUB_FILE_TYPE_CONFIG);
  if (!file) return grub_errno;
  if (!file->size || file->size > 4 * 1024 * 1024) {
    grub_file_close (file);
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "D1 system configuration has unexpected size");
  }
  bytes = (grub_size_t) file->size;
  text = grub_malloc (bytes + 1);
  if (!text) { grub_file_close (file); return grub_errno; }
  if (grub_file_read (file, text, bytes) != (grub_ssize_t) bytes) {
    grub_free (text); grub_file_close (file);
    return grub_error (GRUB_ERR_FILE_READ_ERROR, "D1 could not read complete system configuration");
  }
  grub_file_close (file);
  source_bytes = bytes;
  for (i = 0; i < bytes; i++) fingerprint = (fingerprint ^ (grub_uint8_t) text[i]) * 16777619U;
  text[bytes] = 0;
  if (grub_strlen (text) != bytes || !d1_filter_system_config (text, &bytes, &skipped)) {
    grub_free (text);
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "D1 system configuration filtering refused");
  }
  errors_before = grub_err_printed_errors;
  error = grub_script_execute_sourcecode (text);
  grub_free (text);
  menu = grub_env_get_menu ();
  if (error || grub_err_printed_errors != errors_before || !menu || menu->size <= count_before)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "D1 system menu import failed");
  grub_env_set ("sp8_daily_imported", "1");
  grub_snprintf (marker, sizeof (marker), "CONFIG_IMPORT_OK entries=%d filtered_nonmenu_blocks=%u source_bytes=%llu source_fnv1a32=%08x",
                 menu->size - count_before, skipped, (unsigned long long) source_bytes, fingerprint);
  d1_phase (marker);
  if (!grub_env_get ("sp8_daily_quiet"))
    grub_printf ("D1 SYSTEM MENU IMPORTED entries=%d filtered_nonmenu_blocks=%u\n",
                 menu->size - count_before, skipped);
  return d1_wrap_menu (0);
}

static grub_err_t
d1_help (void)
{
  int key;
  d1_phase ("HELP_OPEN");
  grub_cls ();
  grub_printf ("音量＋向上，音量－向下。\n");
  grub_printf ("短按电源并松开：确认；键盘方向键和 Enter 也可使用。\n");
  grub_printf ("同时按住双音量约半秒或按 Esc：返回。\n");
  grub_printf ("子菜单返回上一级，主菜单返回原 GRUB。\n");
  grub_printf ("原 Ubuntu、Windows、旧内核和 Vimix 主题保留。\n");
  grub_printf ("工具箱可检查、停用、回退；系统更新照常。\n");
  grub_printf ("有问题回到 Linux 后告诉我，我会读取启动日志。\n");
  grub_printf ("短按电源并松开，或按 Enter / Esc 返回。\n");
  grub_printf ("D1_HELP_PAGE\n");
  do { key = grub_getkey (); } while (key != '\r' && key != '\n' && key != GRUB_TERM_ESC);
  d1_phase (key == GRUB_TERM_ESC ? "HELP_RETURN key=ESC" : "HELP_RETURN key=ENTER");
  return GRUB_ERR_NONE;
}

static grub_err_t
d1_command (grub_command_t command __attribute__ ((unused)), int argc, char **argv)
{
  if (argc == 2 && !grub_strcmp (argv[0], "load")) return d1_load_system_config (argv[1]);
  if (argc == 1 && !grub_strcmp (argv[0], "wrap-sub")) return d1_wrap_menu (1);
  if (argc == 1 && !grub_strcmp (argv[0], "layout")) {
    struct grub_video_mode_info mode;
    char theme_path[256], detail[256];
    const char *embedded = grub_env_get ("sp8_daily_embed_prefix");
    int large, medium;
    if (!embedded || grub_video_get_info (&mode)) return grub_errno;
    large = mode.width >= 2560 && mode.height >= 1440;
    medium = !large && mode.width >= 1600 && mode.height >= 900;
    if (large || medium) {
      char path[256];
      grub_snprintf (path, sizeof (path), "%s/themes/Vimix/surface-readable-%u.pf2", embedded, large ? 48 : 36);
      if (grub_command_execute ("loadfont", 1, (char *[]) {path})) return grub_errno;
    }
    grub_snprintf (theme_path, sizeof (theme_path), "%s/themes/Vimix/%s",
                   embedded, large ? "theme.txt" : medium ? "theme-medium.txt" : "theme-compact.txt");
    grub_env_set ("theme", theme_path); grub_env_export ("theme");
    grub_env_set ("sp8_daily_menu_font", large ? "Surface Readable Regular 48" : medium ? "Surface Readable Regular 36" : "Surface Readable Regular 24");
    grub_env_set ("sp8_m1_theme", "Vimix");
    grub_snprintf (detail, sizeof (detail), "LAYOUT_READY width=%u height=%u menu_font=%u footer_font=%u centered=1 entries_unchanged=1",
                   mode.width, mode.height, large ? 48 : medium ? 36 : 24, large ? 28 : medium ? 24 : 20);
    d1_phase (detail);
    return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "help")) return d1_help ();
  if (argc == 1 && !grub_strcmp (argv[0], "original")) {
    d1_phase ("RETURN_REQUEST target=original");
    grub_env_set ("sp8_daily_return_request", "1");
    return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "status")) {
    grub_menu_t menu = grub_env_get_menu ();
    grub_menu_entry_t entry;
    grub_printf ("D1 MENU_COUNT=%d\n", menu ? menu->size : 0);
    if (menu) for (entry = menu->entry_list; entry; entry = entry->next)
      grub_printf ("D1 MENU ITEM id=%s submenu=%d title=%s\n", entry->id ? entry->id : "", entry->submenu, entry->title);
    return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "menu-snapshot")) {
    grub_menu_t menu = grub_env_get_menu ();
    grub_menu_entry_t entry;
    unsigned index = 0;
    if (menu) for (entry = menu->entry_list; entry; entry = entry->next, index++) {
      grub_uint32_t source_hash = 2166136261U;
      const char *body = entry->sourcecode ? entry->sourcecode : "";
      const char marker[] = "sp8_gpio selected\n";
      const char suffix[] = "\nsp8_system_menu wrap-sub\n";
      grub_size_t size;
      struct grub_menu_entry_class *cls;
      if (!grub_strncmp (body, marker, sizeof (marker) - 1)) body += sizeof (marker) - 1;
      size = grub_strlen (body);
      if (entry->submenu && size >= sizeof (suffix) - 1
          && !grub_strcmp (body + size - (sizeof (suffix) - 1), suffix)) size -= sizeof (suffix) - 1;
      for (grub_size_t i = 0; i < size; i++) source_hash = (source_hash ^ (grub_uint8_t) body[i]) * 16777619U;
      grub_printf ("D1 SNAP index=%u id=%s submenu=%d title=%s body_bytes=%llu fnv1a32=%08x classes=",
                   index, entry->id ? entry->id : "", entry->submenu, entry->title,
                   (unsigned long long) size, source_hash);
      for (cls = entry->classes; cls; cls = cls->next) if (cls->name) grub_printf ("%s,", cls->name);
      grub_printf ("\n");
    }
    return GRUB_ERR_NONE;
  }
  if (argc == 1 && !grub_strcmp (argv[0], "font-status")) {
    const char *name = grub_env_get ("sp8_daily_menu_font");
    grub_font_t font = grub_font_get (name ? name : "Surface Readable Regular 24");
    struct grub_font_glyph *glyph;
    if (!font) return grub_error (GRUB_ERR_BAD_FONT, "D1 readable font not loaded");
    glyph = grub_font_get_glyph (font, 0x97F3);
    grub_printf ("D1 FONT name=%s height=%d maxwidth=%d chinese_glyph=%u\n",
                 grub_font_get_name (font), grub_font_get_height (font),
                 grub_font_get_max_char_width (font), glyph ? glyph->width : 0);
    return GRUB_ERR_NONE;
  }
  return grub_error (GRUB_ERR_BAD_ARGUMENT, "sp8_system_menu load FILE|wrap-sub|layout|help|original|status|font-status");
}
static grub_command_t d1_cmd;
GRUB_MOD_INIT (sp8_system_menu)
{
  d1_cmd = grub_register_command ("sp8_system_menu", d1_command, "load FILE|wrap-sub|layout|help|original|status|font-status",
                                  "Load current system entries with signed theme and logging");
}
GRUB_MOD_FINI (sp8_system_menu)
{
  if (d1_cmd) grub_unregister_command (d1_cmd);
}
