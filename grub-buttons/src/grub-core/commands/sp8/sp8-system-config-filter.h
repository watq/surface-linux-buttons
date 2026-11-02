/* Keep system configuration intact except the known obsolete text-key block.
 * Operates on a bounded, already loaded buffer; does not touch source files.
 */
#ifndef SP8_SYSTEM_CONFIG_FILTER_H
#define SP8_SYSTEM_CONFIG_FILTER_H
static int
d1_filter_system_config (char *text, size_t *bytes, unsigned *skipped)
{
  const char *begins[2] = {"### BEGIN /etc/grub.d/01_keybindings ###",
                           "### BEGIN /etc/grub.d/36_bli ###"};
  const char *ends[2] = {"### END /etc/grub.d/01_keybindings ###",
                         "### END /etc/grub.d/36_bli ###"};
  unsigned index;
  *skipped = 0;
  for (index = 0; index < 2; index++) {
  char *first = grub_strstr (text, begins[index]), *last, *entry;
  if (!first) continue;
  last = grub_strstr (first, ends[index]);
  if (!last) return 0;
  entry = grub_strstr (first, "menuentry ");
  if (entry && entry < last) return 0;
  entry = grub_strstr (first, "submenu ");
  if (entry && entry < last) return 0;
  last += grub_strlen (ends[index]);
  if (*last == '\r') last++;
  if (*last == '\n') last++;
  *bytes -= (size_t) (last - first);
  grub_memmove (first, last, grub_strlen (last) + 1);
  (*skipped)++;
  }
  /* Daily D1 prepares its signed fonts and graphic terminal once, after the
   * current state/functions/menu definitions are loaded. Skip only the known
   * generated visual slice, never a menu body or the grubenv/default logic.
   */
  {
    char *header = grub_strstr (text, "### BEGIN /etc/grub.d/00_header ###");
    char *header_end = header ? grub_strstr (header, "### END /etc/grub.d/00_header ###") : 0;
    char *first = header ? grub_strstr (header, "if [ x$feature_default_font_path = xy ] ; then") : 0;
    char *last, *entry;
    if (header && !header_end) return 0;
    if (first && first < header_end) {
      last = grub_strstr (first, "export theme\n");
      if (!last || last >= header_end) return 0;
      last += grub_strlen ("export theme\n");
      entry = grub_strstr (first, "menuentry ");
      if (entry && entry < last) return 0;
      entry = grub_strstr (first, "submenu ");
      if (entry && entry < last) return 0;
      *bytes -= (size_t) (last - first);
      grub_memmove (first, last, grub_strlen (last) + 1);
    }
  }
  /* The same preserved Vimix fonts are in the signed memdisk. Do not reopen
   * external unsigned fonts while executing the generated visual header.
   * Replacements only shorten the bounded buffer and never touch menu bodies.
   */
  {
    const char *from[3] = {"($root)/usr/share/grub/themes/Vimix/",
                           "font=\"/usr/share/grub/unicode.pf2\"", "font=unicode"};
    const char *to[3] = {"(memdisk)/boot/grub/themes/Vimix/", "font=$sp8f", "font=$sp8f"};
    char *header = grub_strstr (text, "### BEGIN /etc/grub.d/00_header ###");
    char *header_end = header ? grub_strstr (header, "### END /etc/grub.d/00_header ###") : 0;
    if (header && !header_end) return 0;
    if (header) for (index = 0; index < 3; index++) {
      char *found;
      size_t old_length = grub_strlen (from[index]), new_length = grub_strlen (to[index]);
      while ((found = grub_strstr (header, from[index])) && found < header_end) {
        size_t difference = old_length - new_length;
        size_t j;
        if (found + old_length > header_end) return 0;
        for (j = 0; j < new_length; j++) found[j] = to[index][j];
        grub_memmove (found + new_length, found + old_length, grub_strlen (found + old_length) + 1);
        *bytes -= difference; header_end -= difference;
      }
    }
  }
  return 1;
}
#endif
