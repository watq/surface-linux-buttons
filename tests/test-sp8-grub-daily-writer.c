/* Exercise the actual D1 writer with injected EFI I/O, not physical media. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#define GRUB_UTIL 1
#define GRUB_FILE "test-sp8-grub-daily-writer.c"
#include <grub/efi/efi.h>
#include <grub/efi/api.h>
#include <grub/misc.h>
#include <grub/env.h>
#include <grub/time.h>
#include "sp8-grub-daily-log.h"

grub_efi_system_table_t *grub_efi_system_table;
int grub_efi_is_finished;
static unsigned clock_ms, writes, flushes, closes, short_write, fail_write, fail_flush;
static unsigned io_delay, callback_once;
static char saved[131072];
static size_t used;
grub_uint64_t grub_get_time_ms(void) { return clock_ms; }
void *grub_memmove(void *d, const void *s, grub_size_t n) { return memmove(d,s,n); }
void *grub_memset(void *d, int v, grub_size_t n) { return memset(d,v,n); }
int grub_strncmp(const char *a, const char *b, grub_size_t n) { return strncmp(a,b,n); }
grub_size_t grub_strlen(const char *s) { return strlen(s); }
int grub_vsnprintf(char *out, grub_size_t n, const char *fmt, va_list args) { return vsnprintf(out,n,fmt,args); }
const char *grub_env_get(const char *name) { (void)name; return "1"; }
int grub_printf(const char *fmt, ...) { (void)fmt; return 0; }
static grub_efi_status_t __grub_efi_api write_file(M1_FILE *file, grub_efi_uintn_t *n, void *text) {
  (void)file; writes++; clock_ms+=io_delay;
  if (fail_write) return GRUB_EFI_DEVICE_ERROR;
  if (short_write && *n>7) *n=7;
  assert(used+*n<sizeof(saved)); memcpy(saved+used,text,*n); used+=*n; saved[used]=0;
  if (callback_once) { callback_once=0; m1_record("D1 ERROR errno=1 message=callback_fault\n"); }
  return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api flush_file(M1_FILE *file) {
  (void)file; flushes++; clock_ms+=io_delay; return fail_flush?GRUB_EFI_DEVICE_ERROR:GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api close_file(M1_FILE *file) { (void)file; closes++; return GRUB_EFI_SUCCESS; }
static M1_FILE file={.write=write_file,.flush=flush_file,.close=close_file};
static void reset(void) {
  clock_ms=writes=flushes=closes=short_write=fail_write=fail_flush=io_delay=callback_once=0;
  used=0; saved[0]=0; grub_efi_is_finished=0;
  m1_log_file=&file; m1_log_bytes=0;
  m1_log_pending_bytes=m1_log_busy=m1_log_metadata_lines=m1_log_force_after_io=m1_log_drain_depth=0;
  m1_log_inflight_bytes=0;
  m1_log_io_ms=m1_log_write_calls=m1_log_flush_calls=m1_log_max_write_ms=m1_log_max_flush_ms=0;
  m1_log_last_io_ms=m1_log_last_io_end_ms=m1_log_reentrant_overflow=0;
}
int main(void) {
  reset(); char expected[4096]={0};
  for(unsigned i=0;i<12;i++) {char line[80]; snprintf(line,sizeof(line),"D1 PHASE META index=%u time_ms=0\n",i);strcat(expected,line);m1_record("%s",line);}
  assert(!writes&&!flushes&&m1_log_pending_bytes==strlen(expected));
  m1_record("D1 PHASE BUILD=writer_fixture\n");strcat(expected,"D1 PHASE BUILD=writer_fixture\n");
  assert(writes==1&&flushes==1&&!strcmp(saved,expected));
  puts("PASS routine startup metadata shares one Write/Flush with exact order and bytes; BUILD drains immediately");
  m1_record("D1 PHASE META pending=1\n");clock_ms=99;m1_log_poll();assert(flushes==1);
  clock_ms=100;m1_log_poll();assert(flushes==2&&strstr(saved,"pending=1"));
  unsigned before=flushes;m1_record("GPIO EVENT button=volume_up state=pressed\n");assert(flushes==before+1);
  m1_record("M1 DELIVER button=volume_up key=UP\n");assert(flushes==before+2);
  m1_record("D1 PHASE EFI_INPUT operation=LeafReadKeyStrokeEx elapsed_ms=999\n");assert(flushes==before+3);
  puts("PASS idle metadata drains by100ms and GPIO/accepted keys/slow faults are immediate durability checkpoints");
  reset();short_write=1;m1_record("D1 ERROR errno=5 message=partial_write_must_retain_exact_text\n");
  assert(writes>1&&flushes==1&&!strcmp(saved,"D1 ERROR errno=5 message=partial_write_must_retain_exact_text\n"));
  reset();callback_once=1;io_delay=10;m1_record("D1 ERROR errno=2 message=outer_fault\n");
  assert(strstr(saved,"outer_fault")<strstr(saved,"callback_fault")&&flushes==2&&!m1_log_pending_bytes);
  assert(m1_log_io_ms==40&&m1_log_max_write_ms==10&&m1_log_max_flush_ms==10);
  puts("PASS partial writes and reentrant EFI fault logging retain exact ordered bytes; callback fault drains after I/O and I/O time is measured");
  reset();fail_write=1;m1_record("D1 ERROR errno=3 message=write_failure\n");assert(!m1_log_file&&closes==1);
  reset();fail_flush=1;m1_record("D1 ERROR errno=4 message=flush_failure\n");assert(!m1_log_file&&closes==1);
  reset();m1_log_bytes=4194300;m1_record("D1 ERROR longer_than_cap\n");assert(!m1_log_file&&closes==1);
  puts("PASS I/O failure and4MiB cap close only the current new logfile without modifying original logs");
  reset();m1_record("D1 PHASE META before_handoff=1\n");m1_log_finish("system_boot");
  assert(!m1_log_file&&closes==1&&strstr(saved,"LOG IO_SUMMARY")&&strstr(saved,"END reason=system_boot")&&strstr(saved,"LOG END complete=1"));
  reset();m1_log_busy=1;
  for(unsigned i=0;i<20;i++)m1_record("D1 ERROR message=%0400u\n",i);
  assert(m1_log_reentrant_overflow>0);
  m1_log_busy=0;m1_log_finish("system_boot");
  assert(strstr(saved,"LOG END complete=0")&&!strstr(saved,"LOG END complete=1"));
  reset();grub_efi_is_finished=1;m1_record("D1 ERROR after_exit=1\n");m1_log_poll();assert(!writes&&!flushes);
  puts("PASS handoff drains metadata and complete END, no EFI writes after ExitBootServices; original key/OSK behavior is outside this writer");
  return 0;
}
