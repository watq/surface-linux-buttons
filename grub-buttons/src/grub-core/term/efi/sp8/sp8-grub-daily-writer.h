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
/* Routine metadata can share one EFI Write/Flush. Critical input/errors and
   handoff checkpoints drain immediately. No accepted key or fault text is
   dropped to gain speed; a sudden loss of power can lose only metadata since
   the last checkpoint (idle servicing tries every100ms). */
#define D1_LOG_BUFFER_SIZE 4096u
static char m1_log_pending[D1_LOG_BUFFER_SIZE], m1_log_writing[D1_LOG_BUFFER_SIZE];
static unsigned m1_log_pending_bytes, m1_log_busy, m1_log_metadata_lines;
static grub_uint64_t m1_log_pending_since_ms;
static grub_uint64_t m1_log_io_ms, m1_log_write_calls, m1_log_flush_calls;
static grub_uint64_t m1_log_max_write_ms, m1_log_max_flush_ms;
static grub_uint64_t m1_log_last_io_ms, m1_log_last_io_end_ms;
static unsigned m1_log_reentrant_overflow;
static unsigned m1_log_force_after_io;
static unsigned m1_log_drain_depth;
static unsigned m1_log_inflight_bytes;
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
  M1_FILE *file = m1_log_file;
  m1_log_file = NULL;
  m1_log_pending_bytes = m1_log_inflight_bytes = 0;
  if (file) file->close (file);
}

static void
m1_log_flush_pending (void)
{
  grub_efi_uintn_t size, offset = 0, length;
  grub_uint64_t begin, duration, io_begin;
  if (!m1_log_file || !m1_log_pending_bytes || m1_log_busy || grub_efi_is_finished) return;
  m1_log_busy = 1;
  m1_log_force_after_io = 0;
  length = m1_log_pending_bytes;
  m1_log_inflight_bytes = length;
  grub_memcpy (m1_log_writing, m1_log_pending, length);
  m1_log_pending_bytes = 0;
  io_begin = grub_get_time_ms ();
  while (offset < length) {
    size = length - offset;
    begin = grub_get_time_ms ();
    grub_efi_status_t status = m1_log_file->write (m1_log_file, &size, m1_log_writing + offset);
    duration = grub_get_time_ms () - begin;
    m1_log_io_ms += duration; m1_log_write_calls++;
    m1_log_max_write_ms = grub_max (m1_log_max_write_ms, duration);
    if (status != GRUB_EFI_SUCCESS || !size || size > length - offset)
      { m1_log_abandon (); m1_log_busy = m1_log_inflight_bytes = 0; return; }
    offset += size; m1_log_bytes += size;
    m1_log_inflight_bytes = length - offset;
  }
  begin = grub_get_time_ms ();
  grub_efi_status_t status = m1_log_file->flush (m1_log_file);
  duration = grub_get_time_ms () - begin;
  m1_log_io_ms += duration; m1_log_flush_calls++;
  m1_log_max_flush_ms = grub_max (m1_log_max_flush_ms, duration);
  m1_log_last_io_end_ms = grub_get_time_ms ();
  m1_log_last_io_ms = m1_log_last_io_end_ms - io_begin;
  if (status != GRUB_EFI_SUCCESS) m1_log_abandon ();
  m1_log_busy = 0;
  if (m1_log_force_after_io && m1_log_file && m1_log_pending_bytes && !m1_log_drain_depth)
    {
      m1_log_force_after_io = 0;
      /* A critical line queued by an EFI callback during Write/Flush must
         be persisted as soon as that I/O returns. No GRUB callback reads
         or consumes the raw input event queue here. */
      m1_log_drain_depth = 1;
      m1_log_flush_pending ();
      m1_log_drain_depth = 0;
    }
}

static int
m1_log_is_checkpoint (const char *line)
{
  static const char * const prefixes[] = {
    "LOG ACTIVE", "=== SP8", "LOG END", "LOG IO_SUMMARY", "GPIO EVENT", "M1 DELIVER", "M1 STOP",
    "D1 ERROR", "D1 BOOT HANDOFF", "D1 BOOT RETURN", "D1 RETURN", "D1 SYSTEM SELECT",
    "D1 ENTRY_UI_PREPARE", "D1 INPUT SAMPLE_GAP", "D1 TOUCH_CONTROLLER", "D1 TOUCH_INIT_SUMMARY",
    "D1 OSK DISPLAY_EXIT", "D1 OSK ENTRY_RETURN", "D1 EFI_INPUT_ROUTE ", "D1 PHASE BUILD=", "D1 PHASE CONFIG_IMPORT_BEGIN",
    "D1 PHASE FRAME_DRAWN", "D1 PHASE MENU_INPUT_READY", "D1 PHASE ENTRY_CLEAR",
    "D1 PHASE ENTRY_SCRIPT_RETURN", "D1 PHASE EFI_NAV_DELIVER"
  };
  unsigned i;
  for (i = 0; i < ARRAY_SIZE (prefixes); i++)
    if (!grub_strncmp (line, prefixes[i], grub_strlen (prefixes[i]))) return 1;
  /* Slow-operation evidence is a checkpoint, including calls whose timing
     includes callback work. Zero-duration query inventory remains metadata. */
  for (i = 0; line[i]; i++)
    if (!grub_strncmp (line + i, "elapsed_ms=", 11)
        || !grub_strncmp (line + i, "total_ms=", 9))
      {
        unsigned j = i + (!grub_strncmp (line + i, "elapsed_ms=", 11) ? 11 : 9);
        unsigned value = 0;
        while (line[j] >= '0' && line[j] <= '9')
          { value = value * 10 + line[j++] - '0'; if (value > 20) return 1; }
      }
  return 0;
}

static void
m1_log_line (const char *line)
{
  unsigned length = grub_strlen (line);
  grub_uint64_t now;
  if (!m1_log_file || grub_efi_is_finished) return;
  if (length > 4 * 1024 * 1024u - m1_log_bytes - m1_log_pending_bytes - m1_log_inflight_bytes)
    {
      if (m1_log_busy) { m1_log_reentrant_overflow++; return; }
      m1_log_flush_pending (); m1_log_abandon (); return;
    }
  if (length > sizeof (m1_log_pending) - m1_log_pending_bytes) m1_log_flush_pending ();
  if (!m1_log_file) return;
  if (length > sizeof (m1_log_pending) - m1_log_pending_bytes)
    { m1_log_reentrant_overflow++; return; }
  now = grub_get_time_ms ();
  if (!m1_log_pending_bytes) m1_log_pending_since_ms = now;
  grub_memcpy (m1_log_pending + m1_log_pending_bytes, line, length);
  m1_log_pending_bytes += length;
  if (m1_log_busy && m1_log_is_checkpoint (line)) m1_log_force_after_io = 1;
  if (m1_log_is_checkpoint (line) || m1_log_pending_bytes >= sizeof (m1_log_pending) / 2
      || now < m1_log_pending_since_ms || now - m1_log_pending_since_ms >= 100)
    m1_log_flush_pending ();
  else m1_log_metadata_lines++;
}

static void
m1_log_poll (void)
{
  grub_uint64_t now;
  if (!m1_log_file || !m1_log_pending_bytes || m1_log_busy || grub_efi_is_finished) return;
  now = grub_get_time_ms ();
  if (m1_log_force_after_io || now < m1_log_pending_since_ms || now - m1_log_pending_since_ms >= 100)
    m1_log_flush_pending ();
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
  m1_log_pending_bytes = m1_log_busy = m1_log_metadata_lines = 0;
  m1_log_io_ms = m1_log_write_calls = m1_log_flush_calls = 0;
  m1_log_max_write_ms = m1_log_max_flush_ms = m1_log_last_io_ms = m1_log_last_io_end_ms = 0;
  m1_log_reentrant_overflow = 0;
  m1_log_force_after_io = m1_log_drain_depth = m1_log_inflight_bytes = 0;
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
    m1_record ("LOG ACTIVE path=\\SP8-KEYDIAG\\%s UTF-8 new-file checkpoints-flush metadata-batch=4096 idle-drain-ms=100 cap=4194304\n", m1_log_name);
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
  m1_log_flush_pending ();
  m1_record ("LOG IO_SUMMARY write_calls=%llu flush_calls=%llu total_io_ms=%llu max_write_ms=%llu max_flush_ms=%llu metadata_lines_batched=%u pending_bytes=%u reentrant_overflow=%u excludes_own_final_checkpoint_io=1\n",
             (unsigned long long) m1_log_write_calls, (unsigned long long) m1_log_flush_calls,
             (unsigned long long) m1_log_io_ms, (unsigned long long) m1_log_max_write_ms,
             (unsigned long long) m1_log_max_flush_ms, m1_log_metadata_lines,
             m1_log_pending_bytes, m1_log_reentrant_overflow);
  m1_record ("=== SP8 GRUB GPIO %s END reason=%s ===\n", SP8_LOG_VERSION, reason);
  m1_record ("LOG END complete=%u\n", !m1_log_reentrant_overflow);
  if (!m1_log_file) return;
  status = m1_log_file->close (m1_log_file);
  m1_log_file = NULL;
  if (status == GRUB_EFI_SUCCESS) {
    if (!grub_env_get ("sp8_daily_quiet"))
      grub_printf ("%s LOG SAVED %s bytes=%u\n", SP8_LOG_VERSION, m1_log_name, m1_log_bytes);
  } else grub_printf ("%s LOG CLOSE FAILED; verify saved file after return.\n", SP8_LOG_VERSION);
}
#endif
