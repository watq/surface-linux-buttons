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
grub_efi_handle_t grub_efi_image_handle;
int grub_efi_is_finished;
grub_err_t grub_errno;
char grub_errmsg[GRUB_MAX_ERRMSG];
grub_uint8_t grub_term_normal_color,grub_term_highlight_color;
static struct grub_efi_console_leaf fixture_inputs[3];
static unsigned fixture_count,fixture_present[3],leaf_pending[3],leaf_reads[3],leaf_delay[3];
static grub_efi_key_data_t fixture_keys[3];
int grub_efi_set_text_mode(int on) {(void)on;assert(!"text output outside input test");return 0;}
grub_err_t grub_error(grub_err_t code,const char *fmt,...) {(void)fmt;grub_errno=code;return code;}
void *grub_efi_open_protocol(grub_efi_handle_t handle,grub_guid_t *guid,grub_efi_uint32_t attributes) {
  (void)guid;assert(attributes==GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  for(unsigned i=0;i<fixture_count;i++)if(fixture_inputs[i].handle==handle)
    return fixture_present[i]?(fixture_inputs[i].ex?(void*)fixture_inputs[i].ex:(void*)fixture_inputs[i].basic):NULL;
  assert(!"new protocol discovery outside input test");return NULL;
}
void *grub_memmove(void *out,const void *in,grub_size_t n) {return memmove(out,in,n);}
void *grub_memset(void *out,int c,grub_size_t n) {return memset(out,c,n);}
struct grub_command *grub_command_list;
static unsigned active=1,ready,checks,reads,fake_ms,slow_check,slow_read;
static unsigned nonowning_queries,query_delay,service_delay,service_calls;
static unsigned withdraw_on_service;
static grub_efi_tpl_t fixture_tpl=GRUB_EFI_TPL_APPLICATION;
static void *replacement_protocol[3];
static unsigned swm_registers,swm_unregisters,swm_updates,swm_draws,swm_fail_registration,swm_closed_paint;
static char record[16384];
static grub_efi_status_t __grub_efi_api query_protocol(grub_efi_handle_t handle,grub_guid_t *guid,
 void **protocol,grub_efi_handle_t agent,grub_efi_handle_t controller,grub_efi_uint32_t attributes) {
  grub_guid_t ex_guid=GRUB_EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID,basic_guid=GRUB_EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
  assert(!agent&&!controller&&attributes==GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  assert(fixture_tpl==GRUB_EFI_TPL_APPLICATION);
  nonowning_queries++;fake_ms+=query_delay;
  for(unsigned i=0;i<fixture_count;i++)if(fixture_inputs[i].handle==handle){
    assert(!memcmp(guid,fixture_inputs[i].ex?&ex_guid:&basic_guid,sizeof(*guid)));
    *protocol=fixture_present[i]?(replacement_protocol[i]?replacement_protocol[i]:(fixture_inputs[i].ex?(void*)fixture_inputs[i].ex:(void*)fixture_inputs[i].basic)):NULL;
    return *protocol?GRUB_EFI_SUCCESS:GRUB_EFI_UNSUPPORTED;
  }
  *protocol=NULL;return GRUB_EFI_UNSUPPORTED;
}
static grub_efi_tpl_t __grub_efi_api raise_tpl(grub_efi_tpl_t target){
  grub_efi_tpl_t old=fixture_tpl;assert(target==GRUB_EFI_TPL_NOTIFY&&old==GRUB_EFI_TPL_APPLICATION);fixture_tpl=target;return old;
}
static void __grub_efi_api restore_tpl(grub_efi_tpl_t old){
  assert(old==GRUB_EFI_TPL_APPLICATION&&fixture_tpl==GRUB_EFI_TPL_NOTIFY);fixture_tpl=old;service_calls++;fake_ms+=service_delay;
  if(withdraw_on_service){withdraw_on_service=0;grub_efi_console_set_leaf_inputs(0,NULL);}
}
const char *grub_env_get(const char *name) {(void)name;return active?"1":NULL;}
int grub_strcmp(const char *left,const char *right) {return strcmp(left,right);}
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
  (void)input;reads++;fake_ms+=slow_read;
  if(!ready)return GRUB_EFI_NOT_READY;
  *key=(grub_efi_input_key_t){.unicode_char='e'};ready=0;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api read_ex(grub_efi_simple_text_input_ex_interface_t *input,grub_efi_key_data_t *key) {
  (void)input;reads++;fake_ms+=slow_read;
  if(!ready)return GRUB_EFI_NOT_READY;
  memset(key,0,sizeof(*key));key->key.unicode_char='c';
  key->key_state.key_shift_state=GRUB_EFI_SHIFT_STATE_VALID|GRUB_EFI_LEFT_SHIFT_PRESSED;
  ready=0;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api read_leaf_ex(grub_efi_simple_text_input_ex_interface_t *input,grub_efi_key_data_t *key) {
  assert(fixture_tpl==GRUB_EFI_TPL_APPLICATION);
  unsigned i;for(i=0;i<fixture_count;i++)if(fixture_inputs[i].ex==input)break;assert(i<fixture_count);
  leaf_reads[i]++;fake_ms+=leaf_delay[i];*key=fixture_keys[i];
  if(!leaf_pending[i]){key->key=(grub_efi_input_key_t){0};return GRUB_EFI_NOT_READY;}
  leaf_pending[i]=0;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api read_leaf_basic(grub_efi_simple_input_interface_t *input,grub_efi_input_key_t *key) {
  assert(fixture_tpl==GRUB_EFI_TPL_APPLICATION);
  unsigned i;for(i=0;i<fixture_count;i++)if(fixture_inputs[i].basic==input)break;assert(i<fixture_count);
  leaf_reads[i]++;fake_ms+=leaf_delay[i];*key=fixture_keys[i].key;
  if(!leaf_pending[i])return GRUB_EFI_NOT_READY;leaf_pending[i]=0;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api swm_register(struct grub_efi_swm *p,grub_efi_handle_t handle,grub_uint32_t z,
 struct grub_efi_swm_rect *rect,void *notify,void *context,void **pointer,grub_efi_event_t *paint) {
 (void)p;assert(handle==grub_efi_image_handle&&z==0x10&&!notify&&!context&&rect->right==639&&rect->bottom==479);
 swm_registers++;*pointer=(void*)99;*paint=(void*)98;return swm_fail_registration?GRUB_EFI_DEVICE_ERROR:GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api swm_unregister(struct grub_efi_swm *p,grub_efi_handle_t handle) {(void)p;assert(handle==grub_efi_image_handle);swm_unregisters++;return GRUB_EFI_SUCCESS;}
static grub_efi_status_t __grub_efi_api swm_close_paint(grub_efi_event_t event) {assert(event==(void*)98);swm_closed_paint++;return GRUB_EFI_SUCCESS;}
static grub_efi_status_t __grub_efi_api swm_frame(struct grub_efi_swm *p,grub_efi_handle_t handle,struct grub_efi_swm_rect *rect) {(void)p;(void)rect;assert(handle==grub_efi_image_handle);swm_updates++;return GRUB_EFI_SUCCESS;}
static grub_efi_status_t __grub_efi_api swm_blt(struct grub_efi_swm *p,grub_efi_handle_t handle,struct grub_efi_gop_blt_pixel *pixels,
 grub_efi_gop_blt_operation_t op,grub_efi_uintn_t sx,grub_efi_uintn_t sy,grub_efi_uintn_t dx,grub_efi_uintn_t dy,
 grub_efi_uintn_t width,grub_efi_uintn_t height,grub_efi_uintn_t pitch) {
 (void)p;(void)pixels;assert(handle==grub_efi_image_handle&&op==GRUB_EFI_BLT_BUFFER_TO_VIDEO&&sx==dx&&sy==dy&&width==640&&height==480&&pitch==2560);swm_draws++;return GRUB_EFI_SUCCESS;
}
int main(void) {
  grub_efi_system_table_t table={0};grub_efi_boot_services_t services={0};
  grub_efi_simple_input_interface_t input={.read_key_stroke=read_basic,.wait_for_key=(void*)1};
  grub_efi_simple_text_input_ex_interface_t ex={.read_key_stroke=read_ex,.wait_for_key=(void*)2};
  struct grub_term_input terminal={.name="console",.data=&ex};
  table.con_in=&input;table.boot_services=&services;services.check_event=check_event;grub_efi_system_table=&table;
  services.open_protocol=query_protocol;services.raise_tpl=raise_tpl;services.restore_tpl=restore_tpl;
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
  unsigned before_checks=checks,before_reads=reads,before_ms=fake_ms;
  grub_efi_console_set_direct_poll(1);
  slow_check=2550;
  assert(grub_console_getkey_con(NULL)==GRUB_TERM_NO_KEY&&reads==before_reads+1);
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&reads==before_reads+2);
  assert(checks==before_checks&&fake_ms==before_ms);
  ready=1;assert(grub_console_getkeystatus(&terminal)&GRUB_TERM_STATUS_LSHIFT);
  before_reads=reads;assert(grub_console_getkey(&terminal)=='c'&&reads==before_reads);
  assert(checks==before_checks);
  puts("PASS the verified endpoint policy skips a 2550ms aggregate event callback; direct reads and cached keyboard/modifier behavior remain intact");
  slow_read=3000;
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY);
  assert(strstr(record,"operation=ReadKeyStrokeEx elapsed_ms=3000"));
  puts("PASS slow direct firmware reads are independently logged, so delay transfer is not hidden");
  slow_read=0;grub_efi_console_set_direct_poll(0);
  before_checks=checks;before_reads=reads;
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&checks==before_checks+1&&reads==before_reads);
  active=0;assert(grub_console_getkey_con(NULL)==GRUB_TERM_NO_KEY&&reads==before_reads+1);
  active=1;input.wait_for_key=NULL;assert(grub_console_getkey_con(NULL)==GRUB_TERM_NO_KEY&&reads==before_reads+2);
  puts("PASS original non-D1 and missing-event fallback paths remain available");
  /* Advance the simulated firmware clock for every console poll below. */
  fake_ms+=5;
  grub_efi_simple_text_input_ex_interface_t leaf0={.read_key_stroke=read_leaf_ex},leaf1={.read_key_stroke=read_leaf_ex};
  grub_efi_simple_input_interface_t leaf2={.read_key_stroke=read_leaf_basic};
  fixture_inputs[0]=(struct grub_efi_console_leaf){.handle=(void*)10,.ex=&leaf0};
  fixture_inputs[1]=(struct grub_efi_console_leaf){.handle=(void*)11,.ex=&leaf1};
  fixture_inputs[2]=(struct grub_efi_console_leaf){.handle=(void*)12,.basic=&leaf2};
  fixture_count=3;fixture_present[0]=fixture_present[1]=fixture_present[2]=1;
  grub_efi_console_set_direct_poll(1);grub_efi_console_set_leaf_inputs(fixture_count,fixture_inputs);
  slow_check=slow_read=2552;before_checks=checks;before_reads=reads;before_ms=fake_ms;
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&fake_ms==before_ms&&checks==before_checks&&reads==before_reads);
  puts("PASS installed leaf protocols skip the simulated 2552ms aggregate read/check without moving the wait before the frame");
  fake_ms+=5;
  fixture_keys[0]=(grub_efi_key_data_t){.key={.unicode_char='c'},.key_state={.key_shift_state=GRUB_EFI_SHIFT_STATE_VALID|GRUB_EFI_LEFT_CONTROL_PRESSED}};
  fixture_keys[1]=(grub_efi_key_data_t){.key={.unicode_char='e'}};fixture_keys[2]=(grub_efi_key_data_t){.key={.unicode_char='x'}};
  leaf_pending[0]=leaf_pending[1]=leaf_pending[2]=1;
  assert(grub_console_getkeystatus(&terminal)&GRUB_TERM_STATUS_LCTRL);
  unsigned prior_leaf_reads=leaf_reads[0]+leaf_reads[1]+leaf_reads[2];
  assert(grub_console_getkey(&terminal)==('c'|GRUB_TERM_CTRL));
  assert(prior_leaf_reads==leaf_reads[0]+leaf_reads[1]+leaf_reads[2]);
  grub_efi_console_set_leaf_inputs(fixture_count,fixture_inputs);
  assert(grub_console_getkey(&terminal)==('e'|GRUB_TERM_CTRL));
  assert(grub_console_getkey(&terminal)==('x'|GRUB_TERM_CTRL));
  assert(!d1_console_leaves[0].pending&&!d1_console_leaves[1].pending&&!d1_console_leaves[2].pending);
  puts("PASS multiple Ex and Basic leaves preserve per-device queues, modifiers, cache-once consumption and refreshed snapshots");
  fake_ms+=5;
  fixture_present[1]=0;unsigned removed_reads=leaf_reads[1];
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&leaf_reads[1]==removed_reads);
  fake_ms+=5;leaf_delay[0]=1900;assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY);
  assert(strstr(record,"operation=LeafReadKeyStrokeEx source=0 handle=0xa elapsed_ms=1900"));
  puts("PASS removed protocols are not called; a slow child is separately attributed rather than declared fixed");
  fake_ms+=5;leaf_delay[0]=0;leaf_pending[0]=1;
  assert(grub_console_getkeystatus(&terminal)&GRUB_TERM_STATUS_LCTRL);
  grub_efi_console_set_leaf_inputs(0,NULL);
  before_reads=reads;assert(grub_console_getkey(&terminal)==('c'|GRUB_TERM_CTRL)&&reads==before_reads);
  puts("PASS an already accepted cached key survives endpoint withdrawal and is consumed once without rereading firmware");
  leaf_delay[0]=0;grub_efi_console_set_leaf_inputs(0,NULL);assert(!d1_console_leaf_active());
  slow_check=slow_read=0;grub_efi_console_set_direct_poll(0);input.wait_for_key=(void*)1;
  before_checks=checks;assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&checks==before_checks+1);
  puts("PASS withdrawing the endpoint route restores the original aggregate event path before boot or return");
  grub_efi_console_set_direct_poll(1);grub_efi_console_set_empty_input_policy(1);
  slow_check=slow_read=2551;before_ms=fake_ms;before_checks=checks;before_reads=reads;
  for(unsigned i=0;i<1000;i++)assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY);
  assert(fake_ms==before_ms&&checks==before_checks&&reads==before_reads);
  puts("PASS positively empty input inventory keeps 1000 menu polls nonblocking and never triggers the 2551ms lazy aggregate");
  fixture_present[1]=1;grub_efi_console_set_leaf_inputs(fixture_count,fixture_inputs);
  fixture_keys[0]=(grub_efi_key_data_t){.key={.unicode_char='k'}};leaf_pending[0]=1;
  assert(grub_console_getkey(&terminal)=='k'&&checks==before_checks&&reads==before_reads);
  grub_efi_console_set_leaf_inputs(0,NULL);grub_efi_console_set_empty_input_policy(0);
  slow_read=slow_check=0;grub_efi_console_set_direct_poll(0);
  ready=1;assert(grub_console_getkey(&terminal)=='c');
  puts("PASS attached leaf keyboard resumes immediately; retracting empty policy restores the ordinary aggregate keyboard after handoff");
  grub_efi_console_log_input_summary();
  assert(strstr(record,"EFI_INPUT_SUMMARY CheckEvent_calls=")&&strstr(record,"aggregate_Ex_reads="));
  puts("PASS actual fallback/read/leaf/idle counts are recorded so a stable path is distinguished from repeated fallback");
  record[0]=0;grub_efi_console_set_direct_poll(1);grub_efi_console_set_leaf_inputs(1,fixture_inputs);
  fixture_present[0]=1;fixture_keys[0]=(grub_efi_key_data_t){.key={.scan_code=1}};leaf_pending[0]=1;
  assert(grub_console_getkey(&terminal)==GRUB_TERM_KEY_UP);
  assert(strstr(record,"EFI_NAV_DELIVER key=UP route=leaf handle=0xa"));
  unsigned nav_count=d1_console_navigation_keys;
  fake_ms+=5;fixture_keys[0]=(grub_efi_key_data_t){.key={.unicode_char='p'}};leaf_pending[0]=1;
  assert(grub_console_getkey(&terminal)=='p'&&d1_console_navigation_keys==nav_count);
  assert(!strstr(record,"key=p"));
  puts("PASS consumed EFI navigation keys carry source provenance while printable command/password characters are never logged");
  fake_ms+=5;unsigned poll_count=leaf_reads[0];for(unsigned i=0;i<1000;i++)assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY);
  assert(leaf_reads[0]==poll_count+1);fake_ms+=5;grub_console_getkey(&terminal);assert(leaf_reads[0]==poll_count+2);
  puts("PASS empty EFI queues poll at most every5ms;1000 busy-loop calls do not amplify firmware work or block GPIO sampling");
  /* Do not cache removed/reinstalled pointers or hide a callback wait by
     moving it to another boundary. Real code is included above. */
  unsigned queried=nonowning_queries,service_before=service_calls;
  for(unsigned i=0;i<1000;i++)assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY);
  assert(nonowning_queries==queried&&service_calls==service_before);
  fake_ms+=5;grub_efi_simple_text_input_ex_interface_t replaced={.read_key_stroke=read_leaf_ex};
  replacement_protocol[0]=&replaced;unsigned leaf_before=leaf_reads[0];
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&leaf_reads[0]==leaf_before);
  replacement_protocol[0]=NULL;
  fake_ms+=5;record[0]=0;query_delay=999;before_ms=fake_ms;
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&fake_ms-before_ms==999);
  assert(strstr(record,"operation=LeafGetProtocol source=0 elapsed_ms=999")&&strstr(record,"query_ms=999"));
  assert(strstr(record,"total_ms=999")&&strstr(record,"agent_tracking=0"));
  query_delay=0;fake_ms+=5;record[0]=0;service_delay=999;before_ms=fake_ms;
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&fake_ms-before_ms==999);
  assert(strstr(record,"operation=RestoreTPL")&&strstr(record,"service_ms=999")&&strstr(record,"total_ms=999"));
  assert(fixture_tpl==GRUB_EFI_TPL_APPLICATION);
  service_delay=0;fake_ms+=5;withdraw_on_service=1;leaf_before=leaf_reads[0];
  assert(grub_console_getkey(&terminal)==GRUB_TERM_NO_KEY&&!d1_console_leaf_count&&leaf_reads[0]==leaf_before);
  grub_efi_console_set_leaf_inputs(1,fixture_inputs);
  puts("PASS live nonowning queries avoid agent tracking, refuse reinstalled pointers, and separately retain999ms query/callback waits in complete-cycle totals; route withdrawal is safe");
  struct grub_efi_swm swm={.register_client=swm_register,.unregister_client=swm_unregister,.set_window_frame=swm_frame,.blt_window=swm_blt};
  services.close_event=swm_close_paint;
  grub_efi_console_configure_swm(&swm,&swm);assert(grub_efi_console_swm_surface(640,480));
  assert(grub_efi_console_swm_surface(640,480)&&swm_registers==1);
  assert(grub_efi_console_swm_blt(NULL,0,0,640,480,2560)==GRUB_EFI_SUCCESS&&swm_draws==1);
  assert(grub_efi_console_swm_surface(640,400)&&swm_updates==1);
  grub_efi_console_release_swm();assert(swm_unregisters==1&&swm_closed_paint==1&&!d1_swm_registered&&!d1_swm);
  swm_fail_registration=1;grub_efi_console_configure_swm(&swm,&swm);
  assert(!grub_efi_console_swm_surface(640,480)&&!grub_efi_console_swm_surface(640,480)&&swm_registers==2);
  grub_efi_console_release_swm();assert(swm_unregisters==1);
  puts("PASS GRUB compositor client registers once below OSK, never activates it, preserves firmware window ownership, and releases only its own client");
  return 0;
}
