/* Execute the exact GRUB module against intercepted ACPI/MMIO space.
 * Host GPIO, physical MMIO, EFI variables and real boot are never accessed.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#define GRUB_UTIL 1
#define GRUB_SP8_POLL_TEST 1
#define GRUB_FILE "test-sp8-grub-gpio-m1.c"
static uint32_t host_load32(uint64_t address);
static void host_copy(void *target,uint64_t address,size_t bytes);
#define M1_LOAD32(address) host_load32(address)
#define M1_COPY_ADDRESS(data,address,bytes) host_copy(data,address,bytes)
#include "../grub-buttons/src/grub-core/term/efi/sp8/sp8-grub-gpio-m1.c"

grub_efi_system_table_t *grub_efi_system_table;
grub_efi_handle_t grub_efi_image_handle;
int grub_efi_is_finished;
grub_err_t grub_errno;
char grub_errmsg[GRUB_MAX_ERRMSG];
void (*grub_sp8_error_hook)(grub_err_t,const char*);
void grub_efi_console_set_direct_poll(int enabled) {(void)enabled;}
void grub_efi_console_set_leaf_inputs(unsigned count,const struct grub_efi_console_leaf *inputs) {(void)count;(void)inputs;}
void grub_efi_console_set_empty_input_policy(int enabled) {(void)enabled;}
void grub_efi_console_log_input_summary(void) { }
void grub_efi_console_configure_swm(struct grub_efi_swm *p,const struct grub_efi_swm *copy) {(void)p;(void)copy;}
void grub_efi_console_release_swm(void) { }
void *grub_efi_open_protocol(grub_efi_handle_t handle,grub_guid_t *guid,grub_efi_uint32_t attributes) {(void)handle;(void)guid;(void)attributes;return NULL;}
void *grub_efi_console_query_input_protocol(grub_efi_handle_t handle,int extended) {(void)handle;(void)extended;return NULL;}
grub_efi_device_path_t *grub_efi_get_device_path(grub_efi_handle_t handle) {(void)handle;return NULL;}
void (*grub_term_poll_input_hook)(void);
grub_term_input_trace_t grub_term_input_trace_hook;
void (*grub_term_poll_usb)(int);
void (*grub_net_poll_cards_idle)(void);
struct grub_term_input *grub_term_inputs;
void grub_term_poll_input(void) {
  grub_err_t saved=grub_errno;char message[GRUB_MAX_ERRMSG];
  memcpy(message,grub_errmsg,sizeof(message));
  if(grub_term_poll_input_hook)grub_term_poll_input_hook();
  grub_errno=saved;memcpy(grub_errmsg,message,sizeof(message));
}
static grub_err_t saved_error;
static grub_efi_system_table_t system_table;
static grub_efi_boot_services_t boot_services;
static struct {
  grub_efi_configuration_table_t configs[2];
  unsigned char rsdp[36],xsdt[52],fadt[148],dsdt[SP8_DSDT_LENGTH],ssdt[SP8_SSDT_LENGTH];
  M1_DXE_PREFIX dxe;
} tables;
static unsigned char dsdt_original[SP8_DSDT_LENGTH],ssdt_original[SP8_SSDT_LENGTH],nvs[SP8_PNVA_SIZE];
static uint32_t io[2][0x10000/4];
static unsigned fake_tick,raw_reads,map_reads,gcd_mode,map_mode,held_initial,change_config;
static unsigned char log_data[8192];
static size_t log_used;

void *grub_malloc(grub_size_t n) { return malloc(n); }
void *grub_zalloc(grub_size_t n) { return calloc(1,n); }
void grub_free(void *p) { free(p); }
void *grub_memmove(void *a,const void *b,grub_size_t n) { return memmove(a,b,n); }
void *grub_memset(void *a,int c,grub_size_t n) { return memset(a,c,n); }
int grub_memcmp(const void *a,const void *b,grub_size_t n) { return memcmp(a,b,n); }
int grub_strcmp(const char *a,const char *b) { return strcmp(a,b); }
int grub_strncmp(const char *a,const char *b,grub_size_t n) { return strncmp(a,b,n); }
char *grub_strcpy(char *a,const char *b) { return strcpy(a,b); }
const char *grub_env_get(const char *name) { (void)name;return "test-selected-os"; }
void grub_error_push(void) { saved_error=grub_errno;grub_errno=0; }
int grub_error_pop(void) { grub_errno=saved_error;return 1; }
grub_size_t grub_strlen(const char *a) { return strlen(a); }
int grub_vsnprintf(char *out,grub_size_t n,const char *fmt,va_list args) { return vsnprintf(out,n,fmt,args); }
int grub_snprintf(char *out,grub_size_t n,const char *fmt,...) {
  int result;va_list args;va_start(args,fmt);result=vsnprintf(out,n,fmt,args);va_end(args);return result;
}
int grub_printf(const char *fmt,...) { (void)fmt;return 0; }
grub_err_t grub_env_set(const char *name,const char *value) { (void)name;(void)value;return 0; }
grub_err_t grub_error(grub_err_t code,const char *fmt,...) { (void)fmt;grub_errno=code;return code; }
grub_uint64_t grub_get_time_ms(void) { return (grub_uint64_t)fake_tick*10; }

static void put32(unsigned char *p,uint32_t v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static void put64(unsigned char *p,uint64_t v) { put32(p,v);put32(p+4,v>>32); }
static void checksum(unsigned char *p,size_t n,size_t index) {
  unsigned char total=0;size_t i;p[index]=0;for(i=0;i<n;i++)total+=p[i];p[index]=(unsigned char)(0-total);
}
static uint32_t crc(const unsigned char *p,size_t n) {
  uint32_t value=~0u;size_t i;unsigned bit;
  for(i=0;i<n;i++){value^=p[i];for(bit=0;bit<8;bit++)value=(value>>1)^(0xedb88320u&(0u-(value&1)));}
  return ~value;
}
static void host_copy(void *target,uint64_t address,size_t bytes) {
  uint64_t base=(uintptr_t)&tables;
  if(gp_range_contains(base,sizeof(tables),address,bytes))memcpy(target,(unsigned char*)&tables+(address-base),bytes);
  else {assert(gp_range_contains(0x1000000,sizeof(nvs),address,bytes));memcpy(target,nvs+(address-0x1000000),bytes);}
}
static uint32_t host_load32(uint64_t address) {
  unsigned bar,offset;uint32_t value;
  if(gp_range_contains(0xfd6e0000,0x10000,address,4)){bar=0;offset=address-0xfd6e0000;}
  else {assert(gp_range_contains(0xfd6a0000,0x10000,address,4));bar=1;offset=address-0xfd6a0000;}
  value=io[bar][offset/4];raw_reads++;
  if(bar==1&&offset==0x860&&fake_tick>=20&&fake_tick<40)value&=~2u;
  if(bar==1&&offset==0x870&&fake_tick>=60&&fake_tick<80)value&=~2u;
  if(bar==0&&offset==0x730&&((fake_tick>=100&&fake_tick<120)||(held_initial&&fake_tick<10)))value&=~2u;
  if(change_config&&fake_tick>=10&&bar==0&&offset==0x730)value|=1u<<9;
  return value;
}
static void descriptor(grub_efi_memory_descriptor_t *d,uint64_t base,uint64_t length,unsigned type,uint64_t attributes) {
  uint64_t aligned=base&~4095ULL;memset(d,0,sizeof(*d));d->physical_start=aligned;
  d->num_pages=((base-aligned)+length+4095)/4096;d->type=type;d->attribute=attributes;
}
static grub_efi_status_t __grub_efi_api mock_map(grub_efi_uintn_t *size,grub_efi_memory_descriptor_t *map,
                                               grub_efi_uintn_t *key,grub_efi_uintn_t *stride,grub_efi_uint32_t *version) {
  size_t required=5*sizeof(*map);map_reads++;*key=1;*stride=sizeof(*map);*version=1;
  if(!map||*size<required){*size=required;return GRUB_EFI_BUFFER_TOO_SMALL;}
  descriptor(map,(uintptr_t)&tables,sizeof(tables),4,8);
  descriptor(map+1,0x1000000,sizeof(nvs),10,8);
  descriptor(map+2,0xfd6e0000,0x10000,gcd_mode?0:11,1);
  descriptor(map+3,0xfd6a0000,0x10000,gcd_mode?0:11,1);
  descriptor(map+4,(uintptr_t)tables.dxe.query,1,3,0);
  if(map_mode==1)map[2].type=7;
  *size=required;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api mock_gcd(grub_efi_physical_address_t address,M1_GCD_DESCRIPTOR *d) {
  if(gp_range_contains(0xfd6e0000,0x10000,address,4))d->base=0xfd6e0000;
  else if(gp_range_contains(0xfd6a0000,0x10000,address,4))d->base=0xfd6a0000;
  else return GRUB_EFI_NOT_FOUND;
  d->length=0x10000;d->attributes=1;d->type=3;return 0;
}
static grub_efi_status_t __grub_efi_api mock_crc(void *p,grub_efi_uintn_t n,grub_efi_uint32_t *out) { *out=crc(p,n);return 0; }
static grub_efi_status_t __grub_efi_api mock_locate(grub_efi_locate_search_type_t type,grub_guid_t *guid,
 void *key,grub_efi_uintn_t *count,grub_efi_handle_t **handles) { (void)type;(void)guid;(void)key;*count=0;*handles=NULL;return GRUB_EFI_NOT_FOUND; }
static grub_efi_status_t __grub_efi_api mock_write(M1_FILE *file,grub_efi_uintn_t *n,void *p) {
  (void)file;assert(log_used+*n<sizeof(log_data));memcpy(log_data+log_used,p,*n);log_used+=*n;log_data[log_used]=0;return 0;
}
static grub_efi_status_t __grub_efi_api mock_flush(M1_FILE *file) { (void)file;return 0; }
static grub_efi_status_t __grub_efi_api mock_close(M1_FILE *file) { (void)file;return 0; }
static M1_FILE memory_log={.close=mock_close,.write=mock_write,.flush=mock_flush};

static void setup(void) {
  memset(&tables,0,sizeof(tables));memset(&system_table,0,sizeof(system_table));memset(&boot_services,0,sizeof(boot_services));
  memset(io,0,sizeof(io));memset(nvs,0,sizeof(nvs));
  memcpy(tables.dsdt,dsdt_original,sizeof(dsdt_original));memcpy(tables.ssdt,ssdt_original,sizeof(ssdt_original));
  put32(tables.dsdt+SP8_PNVB_VALUE_OFFSET,0x1000000);checksum(tables.dsdt,sizeof(tables.dsdt),9);
  nvs[0]=2;put32(nvs+SP8_SBRG_OFFSET,0xfd000000);
  memcpy(tables.fadt,"FACP",4);put32(tables.fadt+4,sizeof(tables.fadt));put64(tables.fadt+140,(uintptr_t)tables.dsdt);checksum(tables.fadt,sizeof(tables.fadt),9);
  memcpy(tables.xsdt,"XSDT",4);put32(tables.xsdt+4,sizeof(tables.xsdt));put64(tables.xsdt+36,(uintptr_t)tables.fadt);put64(tables.xsdt+44,(uintptr_t)tables.ssdt);checksum(tables.xsdt,sizeof(tables.xsdt),9);
  memcpy(tables.rsdp,"RSD PTR ",8);tables.rsdp[15]=2;put32(tables.rsdp+20,sizeof(tables.rsdp));put64(tables.rsdp+24,(uintptr_t)tables.xsdt);
  checksum(tables.rsdp,20,8);checksum(tables.rsdp,sizeof(tables.rsdp),32);
  tables.configs[0].vendor_guid=*(grub_packed_guid_t*)&m1_acpi_guid;tables.configs[0].vendor_table=tables.rsdp;
  tables.dxe.hdr.signature=M1_DXE_SIGNATURE;tables.dxe.hdr.header_size=sizeof(tables.dxe);tables.dxe.query=mock_gcd;
  tables.dxe.hdr.crc32=crc((unsigned char*)&tables.dxe,sizeof(tables.dxe));
  tables.configs[1].vendor_guid=*(grub_packed_guid_t*)&m1_dxe_guid;tables.configs[1].vendor_table=&tables.dxe;
  system_table.num_table_entries=2;system_table.configuration_table=tables.configs;system_table.boot_services=&boot_services;
  boot_services.get_memory_map=mock_map;boot_services.calculate_crc32=mock_crc;boot_services.locate_handle_buffer=mock_locate;
  grub_efi_system_table=&system_table;grub_efi_is_finished=0;grub_errno=0;
  io[0][0]=io[1][0]=0x00960000;io[0][3]=io[1][3]=0x700;
  io[0][0x730/4]=0x44000102;io[1][0x860/4]=io[1][0x870/4]=0x84000102;
  m1_enabled=0;m1_log_file=NULL;fake_tick=raw_reads=map_reads=gcd_mode=map_mode=held_initial=change_config=0;
  m1_log_pending_bytes=m1_log_busy=m1_log_metadata_lines=m1_log_force_after_io=m1_log_drain_depth=0;
  m1_log_inflight_bytes=0;
  log_used=0;memset(log_data,0,sizeof(log_data));
}
static void attach_log(void) { m1_log_file=&memory_log;m1_log_bytes=0;strcpy(m1_log_name,"GM0001.LOG"); }
static void read_exact(const char *path,void *data,size_t bytes) {
  FILE *f=fopen(path,"rb");assert(f);assert(fread(data,1,bytes,f)==bytes);assert(fgetc(f)==EOF);fclose(f);
}
int main(int argc,char **argv) {
  unsigned i,b,delivered=0;int events[3],key;unsigned state[3];M1_ACTIONS actions;
  assert(argc==3);read_exact(argv[1],dsdt_original,sizeof(dsdt_original));read_exact(argv[2],ssdt_original,sizeof(ssdt_original));
  memset(&actions,0,sizeof(actions));state[0]=state[1]=state[2]=0;
  for(i=0;i<=6;i++)assert(m1_actions_update(&actions,i,state,events));
  state[1]=1;for(i=7;i<=12;i++)assert(m1_actions_update(&actions,i,state,events));
  assert(m1_actions_pop(&actions,&b)==GRUB_TERM_KEY_UP && b==1);
  state[1]=0;for(i=13;i<=18;i++)assert(m1_actions_update(&actions,i,state,events));
  state[2]=1;for(i=19;i<=24;i++)assert(m1_actions_update(&actions,i,state,events));
  assert(m1_actions_pop(&actions,&b)==GRUB_TERM_KEY_DOWN && b==2);
  state[2]=0;for(i=25;i<=30;i++)assert(m1_actions_update(&actions,i,state,events));
  state[0]=1;for(i=31;i<=40;i++)assert(m1_actions_update(&actions,i,state,events));
  assert(m1_actions_pop(&actions,&b)==0);
  state[0]=0;for(i=41;i<=46;i++)assert(m1_actions_update(&actions,i,state,events));
  assert(m1_actions_pop(&actions,&b)=='\r' && b==0);
  puts("PASS existing single volume UP/DOWN and short power release ENTER remain at 50ms debounce, without a 1000ms gate");
  state[1]=state[2]=1;
  for(i=47;i<97;i++){assert(m1_actions_update(&actions,i,state,events));assert(m1_actions_pop(&actions,&b)==0);}
  assert(m1_actions_update(&actions,97,state,events));assert(m1_actions_pop(&actions,&b)==GRUB_TERM_ESC);
  for(i=98;i<=160;i++){assert(m1_actions_update(&actions,i,state,events));assert(m1_actions_pop(&actions,&b)==0);}
  state[1]=state[2]=0;for(i=161;i<=167;i++)assert(m1_actions_update(&actions,i,state,events));
  puts("PASS both volume buttons held 500ms deliver exactly one ESC, no simultaneous ENTER, rearm only after release");
  memset(&actions,0,sizeof(actions));state[1]=state[2]=1;
  for(i=0;i<=100;i++){assert(m1_actions_update(&actions,i,state,events));assert(m1_actions_pop(&actions,&b)==0);}
  puts("PASS initially held double-volume keys remain suppressed until stable release, preserving the boot safety guard");
  setup();attach_log();assert(m1_enable()==0);
  for(fake_tick=0;fake_tick<45;fake_tick++){
    key=m1_terminal_getkey(NULL);if(key==GRUB_TERM_KEY_UP){assert(fake_tick==25);delivered++;}
  }
  assert(delivered==1);assert(strstr((char*)log_data,"FIRST_GPIO_POLL"));
  assert(!strstr((char*)log_data,"key=ENTER"));
  puts("PASS actual D1 terminal reports first poll, delivers new first volume press at 250ms in simulation (press began at 200ms), no second-long delay");
  m1_stop("latency_test_finished");m1_log_finish("latency_test_finished");
  setup();attach_log();assert(m1_enable()==0);
  assert(grub_term_poll_input_hook==m1_sample_nonblocking);
  /* Rendering calls the cooperative sampler, never terminal getkey. All
     three short presses start and finish while the frame is still drawing. */
  for(fake_tick=0;fake_tick<136;fake_tick++)grub_term_poll_input();
  assert(m1_actions.count==3);
  assert(!m1_actions.delivered_count[0]&&!m1_actions.delivered_count[1]&&!m1_actions.delivered_count[2]);
  fake_tick=200; /* a long non-rendering gap must not discard confirmed keys */
  assert(m1_terminal_getkey(NULL)==GRUB_TERM_KEY_UP);
  assert(m1_terminal_getkey(NULL)==GRUB_TERM_KEY_DOWN);
  assert(m1_terminal_getkey(NULL)=='\r');
  assert(m1_terminal_getkey(NULL)==GRUB_TERM_NO_KEY);
  puts("PASS three short presses fully inside rendering are sampled without consumption, queued in order, and survive a later gap");
  m1_stop("cooperative_test_finished");m1_log_finish("cooperative_test_finished");
  assert(!grub_term_poll_input_hook);
  setup();assert(m1_enable()==0);change_config=1;
  for(fake_tick=0;fake_tick<16;fake_tick++)grub_term_poll_input();
  assert(!m1_enabled&&!grub_term_poll_input_hook&&!m1_actions.count);
  puts("PASS a changed GPIO configuration during drawing stops sampling and clears the hook/queue");
  puts("PASS software fixture only; no repeated physical G1/M1/src3/src7 tests");
  return 0;
}
