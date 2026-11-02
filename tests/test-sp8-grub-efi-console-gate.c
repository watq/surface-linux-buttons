/* Exercise the actual EFI console readiness/cached-key code on fake firmware.
 * No host keyboard, GPIO, EFI variables or physical boot are accessed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#define GRUB_UTIL 1
#define GRUB_FILE "test-sp8-grub-efi-console-gate.c"
#include "../grub-buttons/src/grub-core/term/efi/console.c"

grub_efi_system_table_t *grub_efi_system_table;
int grub_efi_is_finished;
grub_err_t grub_errno;
char grub_errmsg[GRUB_MAX_ERRMSG];
grub_uint8_t grub_term_normal_color,grub_term_highlight_color;
int grub_efi_set_text_mode(int on) {(void)on;assert(!"text output outside input test");return 0;}
grub_err_t grub_error(grub_err_t code,const char *fmt,...) {(void)fmt;grub_errno=code;return code;}
void *grub_efi_open_protocol(grub_efi_handle_t handle,grub_guid_t *guid,grub_efi_uint32_t attributes) {
  (void)handle;(void)guid;(void)attributes;assert(!"new protocol discovery outside input test");return NULL;
}
struct grub_command *grub_command_list;
static unsigned active=1,ready,checks,reads,fake_ms,slow_check;
static char record[1024];
const char *grub_env_get(const char *name) {(void)name;return active?"1":NULL;}
grub_uint64_t grub_get_time_ms(void) {return fake_ms;}
int grub_snprintf(char *buf,grub_size_t n,const char *fmt,...) {
  int ret;va_list a;va_start(a,fmt);ret=vsnprintf(buf,n,fmt,a);va_end(a);return ret;
}
static grub_err_t log_command(grub_command_t command,int argc,char **argv) {
  (void)command;
  if(argc==2&&strlen(record)+strlen(argv[1])+2<sizeof(record)){strcat(record,argv[1]);strcat(record,"\n");}
  return 0;
}
static struct grub_command cmd={.name="sp8_gpio",.func=log_command};
void *grub_named_list_find(grub_named_list_t head,const char *name) {(void)head;return strcmp(name,"sp8_gpio")?NULL:&cmd;}
static grub_efi_status_t __grub_efi_api check_event(grub_efi_event_t event) {
  assert(event);checks++;fake_ms+=slow_check;return ready?GRUB_EFI_SUCCESS:GRUB_EFI_NOT_READY;
}
static grub_efi_status_t __grub_efi_api read_basic(grub_efi_simple_input_interface_t *input,grub_efi_input_key_t *key) {
  (void)input;reads++;
  if(!ready)return GRUB_EFI_NOT_READY;
  *key=(grub_efi_input_key_t){.unicode_char='e'};ready=0;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api read_ex(grub_efi_simple_text_input_ex_interface_t *input,grub_efi_key_data_t *key) {
  (void)input;reads++;
  if(!ready)return GRUB_EFI_NOT_READY;
  memset(key,0,sizeof(*key));key->key.unicode_char='c';
  key->key_state.key_shift_state=GRUB_EFI_SHIFT_STATE_VALID|GRUB_EFI_LEFT_SHIFT_PRESSED;
  ready=0;return GRUB_EFI_SUCCESS;
}
int main(void) {
  grub_efi_system_table_t table={0};grub_efi_boot_services_t services={0};
  grub_efi_simple_input_interface_t input={.read_key_stroke=read_basic,.wait_for_key=(void*)1};
  grub_efi_simple_text_input_ex_interface_t ex={.read_key_stroke=read_ex,.wait_for_key=(void*)2};
  struct grub_term_input terminal={.name="console",.data=&ex};
  table.con_in=&input;table.boot_services=&services;services.check_event=check_event;grub_efi_system_table=&table;
  assert(grub_console_getkey_con(NULL)==GRUB_TERM_NO_KEY&&reads==0&&checks==1);
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&reads==0&&checks==2);
  ready=1;assert(grub_console_getkey_con(NULL)=='e'&&reads==1);
  ready=1;assert(grub_console_getkeystatus(&terminal)&GRUB_TERM_STATUS_LSHIFT);assert(reads==2);
  unsigned count=checks;
  assert(grub_console_getkey(&terminal)=='c'&&reads==2&&checks==count);
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&reads==2);
  puts("PASS visible D1 console skips empty Basic/Ex reads, preserves keyboard input/modifiers and consumes buffered key exactly once");
  slow_check=2983;assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&reads==2);
  assert(strstr(record,"operation=CheckEvent elapsed_ms=2983"));
  puts("PASS a slow firmware CheckEvent is logged at the real call boundary for future hardware attribution");
  active=0;assert(grub_console_getkey_con(NULL)==GRUB_TERM_NO_KEY&&reads==3);
  active=1;input.wait_for_key=NULL;assert(grub_console_getkey_con(NULL)==GRUB_TERM_NO_KEY&&reads==4);
  puts("PASS original non-D1 and missing-event fallback paths remain available");
  return 0;
}
