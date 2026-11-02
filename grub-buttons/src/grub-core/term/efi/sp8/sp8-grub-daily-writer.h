/* Minimal UEFI file logging for the embedded GRUB input module.
 * Writes only a new GMxxxx.LOG in the uniquely owner-marked G1 directory.
 * No filesystem install, truncate, Delete, SetInfo or firmware variable write.
 */
#ifndef SP8_GRUB_GPIO_LOG_H
#define SP8_GRUB_GPIO_LOG_H

typedef struct m1_file M1_FILE;
struct m1_file {
  grub_efi_uint64_t revision;
  grub_efi_status_t (__grub_efi_api *open) (M1_FILE *, M1_FILE **, grub_efi_char16_t *, grub_efi_uint64_t, grub_efi_uint64_t);
  grub_efi_status_t (__grub_efi_api *close) (M1_FILE *);
  void *delete_unused;
  grub_efi_status_t (__grub_efi_api *read) (M1_FILE *, grub_efi_uintn_t *, void *);
  grub_efi_status_t (__grub_efi_api *write) (M1_FILE *, grub_efi_uintn_t *, void *);
  void *get_position_unused, *set_position_unused;
  grub_efi_status_t (__grub_efi_api *get_info) (M1_FILE *, grub_guid_t *, grub_efi_uintn_t *, void *);
  void *set_info_unused;
  grub_efi_status_t (__grub_efi_api *flush) (M1_FILE *);
};
typedef struct {
  grub_efi_uint64_t revision;
  grub_efi_status_t (__grub_efi_api *open_volume) (void *, M1_FILE **);
} M1_FS;
typedef struct {
  grub_efi_uint64_t size, file_size, physical_size;
  grub_uint8_t times[48];
  grub_efi_uint64_t attribute;
  grub_efi_char16_t filename[32];
} M1_FILE_INFO;
_Static_assert (offsetof (M1_FILE, flush) == 80, "x64 EFI file prefix");
_Static_assert (offsetof (M1_FILE_INFO, attribute) == 72, "EFI file attributes offset");

static M1_FILE *m1_log_file;
static unsigned m1_log_bytes;
static char m1_log_name[16];
static unsigned m1_log_attempted;
#ifndef SP8_LOG_VERSION
#define SP8_LOG_VERSION "M1"
#endif
static const char m1_owner_text[] = "SP8-KEYDIAG-LOG-V1\n";
static grub_guid_t m1_fs_guid = GRUB_EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static grub_guid_t m1_info_guid = {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

static void
m1_utf16 (grub_efi_char16_t *output, const char *input, grub_size_t count)
{
  grub_size_t i;
  for (i = 0; input[i] && i + 1 < count; i++) output[i] = (unsigned char) input[i];
  output[i] = 0;
}

static void
m1_log_abandon (void)
{
  if (m1_log_file) m1_log_file->close (m1_log_file);
  m1_log_file = NULL;
}

static void
m1_log_line (const char *line)
{
  grub_efi_uintn_t size, offset = 0, length = grub_strlen (line);
  if (!m1_log_file) return;
  if (length > 4 * 1024 * 1024u - m1_log_bytes) { m1_log_abandon (); return; }
  while (offset < length) {
    size = length - offset;
    if (m1_log_file->write (m1_log_file, &size, (void *) (line + offset)) != GRUB_EFI_SUCCESS ||
        !size || size > length - offset) { m1_log_abandon (); return; }
    offset += size; m1_log_bytes += size;
  }
  if (m1_log_file->flush (m1_log_file) != GRUB_EFI_SUCCESS) m1_log_abandon ();
}

static void
m1_record (const char *format, ...)
{
  char line[640];
  va_list args;
  va_start (args, format);
  grub_vsnprintf (line, sizeof (line), format, args);
  va_end (args);
  m1_log_line (line);
}

static M1_FILE *
m1_marked_directory (grub_efi_handle_t handle)
{
  M1_FS *fs = NULL;
  M1_FILE *volume = NULL, *directory = NULL, *marker = NULL;
  grub_efi_char16_t dirname[32], markername[16];
  char owner[sizeof (m1_owner_text)];
  grub_efi_uintn_t size = sizeof (owner);
  int matches = 0;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  if (bs->handle_protocol (handle, &m1_fs_guid, (void **) &fs) != GRUB_EFI_SUCCESS || !fs ||
      fs->open_volume (fs, &volume) != GRUB_EFI_SUCCESS || !volume) return NULL;
  m1_utf16 (dirname, "\\SP8-KEYDIAG", ARRAY_SIZE (dirname));
  if (volume->open (volume, &directory, dirname, 1, 0) != GRUB_EFI_SUCCESS || !directory) {
    volume->close (volume); return NULL;
  }
  volume->close (volume);
  m1_utf16 (markername, "OWNER.ID", ARRAY_SIZE (markername));
  if (directory->open (directory, &marker, markername, 1, 0) == GRUB_EFI_SUCCESS && marker) {
    matches = marker->read (marker, &size, owner) == GRUB_EFI_SUCCESS &&
              size == sizeof (m1_owner_text) - 1 && !grub_memcmp (owner, m1_owner_text, size);
    marker->close (marker);
  }
  if (!matches) { directory->close (directory); return NULL; }
  return directory;
}

static int
m1_log_new (M1_FILE *directory)
{
  unsigned number;
  for (number = 1; number <= 9999; number++) {
    M1_FILE *existing = NULL;
    M1_FILE_INFO info;
    grub_efi_uintn_t size = sizeof (info);
    grub_efi_char16_t name[16];
    grub_efi_status_t status;
    grub_snprintf (m1_log_name, sizeof (m1_log_name), "GM%04u.LOG", number);
    m1_utf16 (name, m1_log_name, ARRAY_SIZE (name));
    status = directory->open (directory, &existing, name, 1, 0);
    if (status == GRUB_EFI_SUCCESS && existing) { existing->close (existing); continue; }
    if (status != GRUB_EFI_NOT_FOUND) return 0;
    status = directory->open (directory, &m1_log_file, name, 0x8000000000000003ULL, 0);
    if (status != GRUB_EFI_SUCCESS || !m1_log_file) return 0;
    grub_memset (&info, 0, sizeof (info));
    if (m1_log_file->get_info (m1_log_file, &m1_info_guid, &size, &info) != GRUB_EFI_SUCCESS ||
        size < 80 || info.file_size || (info.attribute & 0x10)) {
      m1_log_abandon (); return 0;
    }
    return 1;
  }
  return 0;
}

static void
m1_log_start (void)
{
  grub_efi_handle_t *handles = NULL;
  grub_efi_uintn_t count = 0, i;
  unsigned matches = 0;
  M1_FILE *chosen = NULL;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  if (m1_log_file || m1_log_attempted || grub_efi_is_finished) return;
  m1_log_attempted = 1;
  m1_log_bytes = 0;
  m1_log_name[0] = 0;
  if (bs->locate_handle_buffer (GRUB_EFI_BY_PROTOCOL, &m1_fs_guid, NULL, &count, &handles) == GRUB_EFI_SUCCESS &&
      handles && count <= 256) {
    for (i = 0; i < count; i++) {
      M1_FILE *directory = m1_marked_directory (handles[i]);
      if (!directory) continue;
      matches++;
      if (!chosen) chosen = directory;
      else directory->close (directory);
    }
    if (matches == 1 && chosen) m1_log_new (chosen);
  }
  if (chosen) chosen->close (chosen);
  if (handles) bs->free_pool (handles);
  if (m1_log_file) {
    m1_record ("LOG ACTIVE path=\\SP8-KEYDIAG\\%s UTF-8 new-file flush-per-line cap=4194304\n", m1_log_name);
    m1_record ("=== SP8 GRUB GPIO %s START ===\n", SP8_LOG_VERSION);
    if (!grub_env_get ("sp8_daily_quiet"))
      grub_printf ("%s LOG ACTIVE %s\n", SP8_LOG_VERSION, m1_log_name);
  } else if (!grub_env_get ("sp8_daily_quiet"))
      grub_printf ("%s LOG UNAVAILABLE; keyboard and guarded GPIO input continue.\n", SP8_LOG_VERSION);
}

static void
m1_log_finish (const char *reason)
{
  grub_efi_status_t status;
  if (!m1_log_file) return;
  m1_record ("=== SP8 GRUB GPIO %s END reason=%s ===\n", SP8_LOG_VERSION, reason);
  m1_record ("LOG END complete=1\n");
  if (!m1_log_file) return;
  status = m1_log_file->close (m1_log_file);
  m1_log_file = NULL;
  if (status == GRUB_EFI_SUCCESS) {
    if (!grub_env_get ("sp8_daily_quiet"))
      grub_printf ("%s LOG SAVED %s bytes=%u\n", SP8_LOG_VERSION, m1_log_name, m1_log_bytes);
  } else grub_printf ("%s LOG CLOSE FAILED; verify saved file after return.\n", SP8_LOG_VERSION);
}
#endif
