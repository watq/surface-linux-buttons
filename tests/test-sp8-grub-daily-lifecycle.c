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
static int direct_poll_policy;
void grub_efi_console_set_direct_poll(int enabled) {direct_poll_policy=!!enabled;}
static unsigned fixture_swm_configured, fixture_swm_released;
void grub_efi_console_configure_swm(struct grub_efi_swm *p,const struct grub_efi_swm *copy) {(void)p;(void)copy;fixture_swm_configured++;}
void grub_efi_console_release_swm(void) {fixture_swm_released++;}
static unsigned published_leaf_count;
static struct grub_efi_console_leaf published_leaves[GRUB_EFI_CONSOLE_MAX_LEAVES];
static unsigned fixture_leaf_mode;
static unsigned installed_leaf_count;
static unsigned empty_input_policy,fixture_hid_present,fixture_usb_present;
static unsigned fixture_usb_class,fixture_usb_failure,fixture_usb_reads,fixture_unknown_conin;
static unsigned fixture_osk_controller_available,fixture_connect_calls,fixture_connect_failure;
static unsigned fixture_connect_delay_ticks;
static unsigned fixture_touch_present, fixture_touch_bound, fixture_touch_connect_calls;
static unsigned fixture_touch_connect_failure, fixture_touch_connect_delay_ticks;
static unsigned fixture_touch_have_absolute, fixture_touch_unreadable, fixture_touch_unknown_inventory, fixture_touch_other_owner;
static D1_ABSOLUTE_PREFIX fixture_absolute;
static D1_ABSOLUTE_MODE fixture_absolute_mode;
static D1_HID_POINTER_PREFIX fixture_pointer_producer;
static D1_DRIVER_PREFIX fixture_swm_driver;
static grub_efi_open_protocol_information_entry_t fixture_absolute_owner;
static grub_efi_device_path_t fixture_touch_path[2];
static unsigned fixture_swm_present=1;
static unsigned char fixture_swm_handle, fixture_touch_handle;
static struct grub_efi_swm fixture_swm_protocol;
static unsigned fixture_osk_show_calls, fixture_osk_keyboard_visible, fixture_osk_icon_visible;
static unsigned fixture_reviewed_pci_available,fixture_reviewed_pci_connect_calls,fixture_reviewed_pci_bad_identity;
static unsigned fixture_reviewed_pointer_available,fixture_reviewed_pointer_connect_calls;
static unsigned fixture_reviewed_pointer_fail,fixture_reviewed_pointer_delay;
static D1_PCI_PREFIX fixture_pci;
static void advance_connection_clock(unsigned ticks);
static unsigned published_leaf_calls;
static D1_OSK osk_interface;
static grub_efi_runtime_services_t fixture_runtime;
static grub_uint8_t fixture_conin[39];
static grub_efi_status_t __grub_efi_api cached_usb_interface(D1_USB_IO *usb,D1_USB_INTERFACE *descriptor) {
  (void)usb;fixture_usb_reads++;
  *descriptor=(D1_USB_INTERFACE){.length=9,.type=4,.class_code=fixture_usb_class};
  return fixture_usb_failure?GRUB_EFI_DEVICE_ERROR:GRUB_EFI_SUCCESS;
}
static D1_USB_IO fixture_usb={.get_interface=cached_usb_interface};
static grub_efi_simple_text_input_ex_interface_t fixture_aggregate;
static struct {
  grub_efi_simple_text_input_ex_interface_t ex;
  grub_efi_simple_input_interface_t basic;
  grub_efi_device_path_t path[6];
} fixture_leaf[3];
static grub_efi_status_t __grub_efi_api leaf_read_ex(grub_efi_simple_text_input_ex_interface_t *input,grub_efi_key_data_t *key) {
  (void)input;(void)key;assert(!"Discovery must not consume firmware keys");return GRUB_EFI_NOT_READY;
}
static grub_efi_status_t __grub_efi_api leaf_read_basic(grub_efi_simple_input_interface_t *input,grub_efi_input_key_t *key) {
  (void)input;(void)key;assert(!"Discovery must not consume firmware keys");return GRUB_EFI_NOT_READY;
}
void grub_efi_console_set_leaf_inputs(unsigned count,const struct grub_efi_console_leaf *inputs) {
  published_leaf_calls++;
  published_leaf_count=count;
  if(count)memcpy(published_leaves,inputs,count*sizeof(*inputs));
}
void grub_efi_console_set_empty_input_policy(int enabled) {empty_input_policy=!!enabled;}
void grub_efi_console_log_input_summary(void) { }
void *grub_efi_open_protocol(grub_efi_handle_t handle,grub_guid_t *guid,grub_efi_uint32_t attributes) {
  grub_guid_t ex_guid=GRUB_EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
  assert(attributes==GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if(handle==&fixture_swm_handle&&!memcmp(guid,&d1_driver_binding_guid,sizeof(*guid)))return &fixture_swm_driver;
  if(handle==&fixture_pci&&!memcmp(guid,&d1_pci_io_guid,sizeof(*guid)))return &fixture_pci;
  if(handle==&fixture_touch_handle){
    if(!memcmp(guid,&d1_absolute_pointer_guid,sizeof(*guid)))return fixture_touch_have_absolute?&fixture_absolute:NULL;
    if(!memcmp(guid,&d1_hid_pointer_guid,sizeof(*guid)))return &fixture_pointer_producer;
    return NULL;
  }
  if(!memcmp(guid,&d1_loaded_image_guid,sizeof(*guid)))return NULL;
  if(handle==(void*)1)return &fixture_aggregate;
  if(handle==&fixture_usb)return &fixture_usb;
  if(fixture_osk_controller_available&&handle==&fixture_leaf[0]
      &&!memcmp(guid,&d1_osk_guid,sizeof(*guid)))return &osk_interface;
  for(unsigned i=0;i<installed_leaf_count;i++)
    if(handle==&fixture_leaf[i])return !memcmp(guid,&ex_guid,sizeof(*guid))?(void*)&fixture_leaf[i].ex:(void*)&fixture_leaf[i].basic;
  return NULL;
}
void *grub_efi_console_query_input_protocol(grub_efi_handle_t handle,int extended) {
  grub_guid_t ex=GRUB_EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID,basic=GRUB_EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
  return grub_efi_open_protocol(handle,extended?&ex:&basic,GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
}
grub_efi_device_path_t *grub_efi_get_device_path(grub_efi_handle_t handle) {
  if(handle==&fixture_touch_handle)return fixture_reviewed_pointer_available?fixture_leaf[2].path:fixture_touch_path;
  if(fixture_osk_controller_available&&handle==&fixture_leaf[0])return fixture_leaf[0].path;
  for(unsigned i=0;i<installed_leaf_count;i++)if(handle==&fixture_leaf[i])return fixture_leaf[i].path;
  return NULL;
}
static grub_efi_status_t __grub_efi_api mock_connect_osk(grub_efi_handle_t controller,
 grub_efi_handle_t *drivers,grub_efi_device_path_t *remaining,grub_efi_boolean_t recursive) {
  if(controller==&fixture_pci){assert(fixture_reviewed_pci_available&&!drivers&&!remaining&&!recursive);fixture_reviewed_pci_connect_calls++;fixture_touch_present=fixture_touch_have_absolute=fixture_touch_bound=1;return GRUB_EFI_SUCCESS;}
  if(controller==&fixture_touch_handle){
    assert(fixture_touch_present&&!remaining&&!recursive);
    if(drivers)assert(drivers[0]==&fixture_swm_handle&&!drivers[1]);
    fixture_touch_connect_calls++;
    if(fixture_reviewed_pointer_available){fixture_reviewed_pointer_connect_calls++;advance_connection_clock(fixture_reviewed_pointer_delay);if(fixture_reviewed_pointer_fail)return GRUB_EFI_DEVICE_ERROR;}
    advance_connection_clock(fixture_touch_connect_delay_ticks);
    if(fixture_touch_connect_failure)return GRUB_EFI_DEVICE_ERROR;
    fixture_touch_have_absolute=1;fixture_touch_bound=1;return GRUB_EFI_SUCCESS;
  }
  assert(controller==&fixture_leaf[0]&&fixture_osk_controller_available&&!drivers&&!remaining&&!recursive);
  fixture_connect_calls++;
  advance_connection_clock(fixture_connect_delay_ticks);
  if(fixture_connect_failure)return GRUB_EFI_DEVICE_ERROR;
  installed_leaf_count=1;return GRUB_EFI_SUCCESS;
}
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
static unsigned fixture_map_delay_ticks;
static void advance_connection_clock(unsigned ticks) {fake_tick+=ticks;}
static unsigned osk_available,osk_get_calls,osk_set_calls,osk_mode=7;
static grub_efi_status_t __grub_efi_api mock_osk_get(D1_OSK *p,grub_uint32_t *mode) {
  (void)p;osk_get_calls++;*mode=osk_mode;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api mock_osk_set(D1_OSK *p,grub_uint32_t mode) {
  (void)p;osk_set_calls++;osk_mode=mode;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api mock_show_keyboard(D1_OSK *p,grub_efi_boolean_t visible) {(void)p;assert(!visible);fixture_osk_keyboard_visible=0;fixture_osk_show_calls++;return GRUB_EFI_SUCCESS;}
static grub_efi_status_t __grub_efi_api mock_show_icon(D1_OSK *p,grub_efi_boolean_t visible) {(void)p;assert(!visible);fixture_osk_icon_visible=0;fixture_osk_show_calls++;return GRUB_EFI_SUCCESS;}
static D1_OSK osk_interface={.show_icon=mock_show_icon,.show_keyboard=mock_show_keyboard,.get_mode=mock_osk_get,.set_mode=mock_osk_set};
static grub_efi_status_t __grub_efi_api mock_pci_read(void *p,unsigned width,grub_uint32_t offset,grub_efi_uintn_t count,void *value) {assert(p==&fixture_pci&&width==2&&!offset&&count==1);*(grub_uint32_t*)value=fixture_reviewed_pci_bad_identity?0x12348086:0xa0d08086;return GRUB_EFI_SUCCESS;}
static grub_efi_status_t __grub_efi_api mock_pci_location(void *p,grub_efi_uintn_t *segment,grub_efi_uintn_t *bus,grub_efi_uintn_t *device,grub_efi_uintn_t *function) {assert(p==&fixture_pci);*segment=*bus=0;*device=0x10;*function=6;return GRUB_EFI_SUCCESS;}
static grub_efi_status_t __grub_efi_api mock_locate_panel(grub_guid_t *guid,grub_efi_device_path_t **path,grub_efi_handle_t *handle) {
  grub_guid_t device_path=GRUB_EFI_DEVICE_PATH_GUID;
  if(!memcmp(guid,&device_path,sizeof(*guid))){
    if(!fixture_reviewed_pointer_available)return GRUB_EFI_NOT_FOUND;
    *path=(void*)((unsigned char*)*path+20);*handle=&fixture_touch_handle;return GRUB_EFI_SUCCESS;
  }
  assert(!memcmp(guid,&d1_pci_io_guid,sizeof(*guid)));if(!fixture_reviewed_pci_available)return GRUB_EFI_NOT_FOUND;*path=(void*)((unsigned char*)*path+18);*handle=&fixture_pci;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api unused_pointer(void) {assert(!"GRUB must not read, reset or register a raw touch callback");return GRUB_EFI_NOT_READY;}
static grub_efi_status_t __grub_efi_api mock_absolute_owner(grub_efi_handle_t handle,grub_guid_t *guid,
 grub_efi_open_protocol_information_entry_t **entries,grub_efi_uintn_t *count) {
  assert(handle==&fixture_touch_handle&&!memcmp(guid,&d1_absolute_pointer_guid,sizeof(*guid)));
  fixture_absolute_owner=(grub_efi_open_protocol_information_entry_t){.agent_handle=fixture_touch_bound?&fixture_swm_handle:fixture_touch_other_owner?(void*)123:NULL,.controller_handle=handle,.attributes=fixture_touch_bound||fixture_touch_other_owner?GRUB_EFI_OPEN_PROTOCOL_BY_DRIVER:GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL,.open_count=1};
  *entries=&fixture_absolute_owner;*count=1;return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api mock_free_pool(void *pointer) {assert(pointer==&fixture_absolute_owner);return GRUB_EFI_SUCCESS;}
static unsigned char log_data[32768];
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
  else if(gp_range_contains((uintptr_t)&osk_interface,sizeof(osk_interface),address,bytes))
    memcpy(target,(unsigned char*)&osk_interface+(address-(uintptr_t)&osk_interface),bytes);
  else if(gp_range_contains((uintptr_t)fixture_leaf,sizeof(fixture_leaf),address,bytes))
    memcpy(target,(unsigned char*)fixture_leaf+(address-(uintptr_t)fixture_leaf),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_usb,sizeof(fixture_usb),address,bytes))
    memcpy(target,(unsigned char*)&fixture_usb+(address-(uintptr_t)&fixture_usb),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_absolute,sizeof(fixture_absolute),address,bytes))
    memcpy(target,(unsigned char*)&fixture_absolute+(address-(uintptr_t)&fixture_absolute),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_absolute_mode,sizeof(fixture_absolute_mode),address,bytes))
    memcpy(target,(unsigned char*)&fixture_absolute_mode+(address-(uintptr_t)&fixture_absolute_mode),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_pointer_producer,sizeof(fixture_pointer_producer),address,bytes))
    memcpy(target,(unsigned char*)&fixture_pointer_producer+(address-(uintptr_t)&fixture_pointer_producer),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_swm_driver,sizeof(fixture_swm_driver),address,bytes))
    memcpy(target,(unsigned char*)&fixture_swm_driver+(address-(uintptr_t)&fixture_swm_driver),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_absolute_owner,sizeof(fixture_absolute_owner),address,bytes))
    memcpy(target,(unsigned char*)&fixture_absolute_owner+(address-(uintptr_t)&fixture_absolute_owner),bytes);
  else if(gp_range_contains((uintptr_t)fixture_touch_path,sizeof(fixture_touch_path),address,bytes))
    memcpy(target,(unsigned char*)fixture_touch_path+(address-(uintptr_t)fixture_touch_path),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_pci,sizeof(fixture_pci),address,bytes))
    memcpy(target,(unsigned char*)&fixture_pci+(address-(uintptr_t)&fixture_pci),bytes);
  else if(gp_range_contains((uintptr_t)&fixture_swm_protocol,sizeof(fixture_swm_protocol),address,bytes))
    memcpy(target,(unsigned char*)&fixture_swm_protocol+(address-(uintptr_t)&fixture_swm_protocol),bytes);
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
  size_t required=28*sizeof(*map);map_reads++;*key=1;*stride=sizeof(*map);*version=1;
  if(!map||*size<required){*size=required;return GRUB_EFI_BUFFER_TOO_SMALL;}
  fake_tick+=fixture_map_delay_ticks;
  descriptor(map,(uintptr_t)&tables,sizeof(tables),4,8);
  descriptor(map+1,0x1000000,sizeof(nvs),10,8);
  descriptor(map+2,0xfd6e0000,0x10000,gcd_mode?0:11,1);
  descriptor(map+3,0xfd6a0000,0x10000,gcd_mode?0:11,1);
  descriptor(map+4,(uintptr_t)tables.dxe.query,1,3,0);
  descriptor(map+5,(uintptr_t)&osk_interface,sizeof(osk_interface),4,8);
  descriptor(map+6,(uintptr_t)mock_osk_get,1,3,0);
  descriptor(map+7,(uintptr_t)mock_osk_set,1,3,0);
  descriptor(map+8,(uintptr_t)fixture_leaf,sizeof(fixture_leaf),4,8);
  descriptor(map+9,(uintptr_t)leaf_read_ex,1,3,0);
  descriptor(map+10,(uintptr_t)leaf_read_basic,1,3,0);
  descriptor(map+11,(uintptr_t)&fixture_usb,sizeof(fixture_usb),4,8);
  descriptor(map+12,(uintptr_t)cached_usb_interface,1,3,0);
  descriptor(map+13,(uintptr_t)&fixture_absolute,sizeof(fixture_absolute),4,8);
  descriptor(map+14,(uintptr_t)&fixture_absolute_mode,sizeof(fixture_absolute_mode),4,8);
  descriptor(map+15,(uintptr_t)&fixture_pointer_producer,sizeof(fixture_pointer_producer),4,8);
  descriptor(map+16,(uintptr_t)&fixture_swm_driver,sizeof(fixture_swm_driver),4,8);
  descriptor(map+17,(uintptr_t)&fixture_absolute_owner,sizeof(fixture_absolute_owner),4,8);
  descriptor(map+18,(uintptr_t)unused_pointer,1,3,0);
  descriptor(map+19,(uintptr_t)fixture_touch_path,sizeof(fixture_touch_path),4,8);
  descriptor(map+20,(uintptr_t)mock_connect_osk,1,3,0);
  descriptor(map+21,(uintptr_t)mock_show_icon,1,3,0);descriptor(map+22,(uintptr_t)mock_show_keyboard,1,3,0);
  descriptor(map+23,(uintptr_t)&fixture_pci,sizeof(fixture_pci),4,8);
  descriptor(map+24,(uintptr_t)mock_pci_read,1,3,0);descriptor(map+25,(uintptr_t)mock_pci_location,1,3,0);
  descriptor(map+26,(uintptr_t)&fixture_swm_protocol,sizeof(fixture_swm_protocol),4,8);
  descriptor(map+27,(uintptr_t)mock_locate_panel,1,3,0);
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
static grub_efi_status_t __grub_efi_api mock_leaf_handles(grub_efi_locate_search_type_t type,grub_guid_t *guid,
 void *key,grub_efi_uintn_t *bytes,grub_efi_handle_t *handles) {
  grub_guid_t hid={0x540937ca,0xcac9,0x4d5c,{0x83,0xa9,0xa8,0x08,0x45,0x79,0x40,0xaf}};
  grub_guid_t usb=GRUB_EFI_USB_IO_PROTOCOL_GUID;
  (void)key;assert(type==GRUB_EFI_BY_PROTOCOL);
  if(fixture_leaf_mode==3){*bytes=4096;return GRUB_EFI_BUFFER_TOO_SMALL;}
  if(!memcmp(guid,&d1_swm_guid,sizeof(*guid))){
    if(!fixture_swm_present)return GRUB_EFI_NOT_FOUND;
    handles[0]=&fixture_swm_handle;*bytes=sizeof(*handles);return GRUB_EFI_SUCCESS;
  }
  if(!memcmp(guid,&d1_absolute_pointer_guid,sizeof(*guid))||!memcmp(guid,&d1_touch_panel_guid,sizeof(*guid))||!memcmp(guid,&d1_hid_pointer_guid,sizeof(*guid))){
    if(fixture_touch_unknown_inventory){*bytes=4096;return GRUB_EFI_BUFFER_TOO_SMALL;}
    if(!fixture_touch_present||(!memcmp(guid,&d1_absolute_pointer_guid,sizeof(*guid))&&!fixture_touch_have_absolute))return GRUB_EFI_NOT_FOUND;
    handles[0]=&fixture_touch_handle;*bytes=sizeof(*handles);return GRUB_EFI_SUCCESS;
  }
  if(!memcmp(guid,&d1_sre_guid,sizeof(*guid))||!memcmp(guid,&d1_simple_pointer_guid,sizeof(*guid)))return GRUB_EFI_NOT_FOUND;
  if(!memcmp(guid,&d1_osk_guid,sizeof(*guid))){
    if(!fixture_osk_controller_available)return GRUB_EFI_NOT_FOUND;
    handles[0]=&fixture_leaf[0];*bytes=sizeof(*handles);return GRUB_EFI_SUCCESS;
  }
  if(!memcmp(guid,&hid,sizeof(hid))){if(!fixture_hid_present)return GRUB_EFI_NOT_FOUND;handles[0]=(void*)20;*bytes=sizeof(*handles);return GRUB_EFI_SUCCESS;}
  if(!memcmp(guid,&usb,sizeof(usb))){if(!fixture_usb_present)return GRUB_EFI_NOT_FOUND;handles[0]=&fixture_usb;*bytes=sizeof(*handles);return GRUB_EFI_SUCCESS;}
  if(!installed_leaf_count)return GRUB_EFI_NOT_FOUND;
  assert(*bytes>=(installed_leaf_count+1)*sizeof(*handles));
  handles[0]=(void*)1;
  for(unsigned i=0;i<installed_leaf_count;i++)handles[i+1]=&fixture_leaf[i];
  *bytes=(installed_leaf_count+1)*sizeof(*handles);return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api mock_get_conin(grub_efi_char16_t *name,const grub_guid_t *guid,
 grub_efi_uint32_t *attributes,grub_efi_uintn_t *size,void *data) {
  (void)guid;*attributes=7;
  if(name[0]!='C'||name[1]!='o'||name[2]!='n'||name[3]!='I'||name[4]!='n'||name[5])return GRUB_EFI_NOT_FOUND;
  if(*size<sizeof(fixture_conin)){*size=sizeof(fixture_conin);return GRUB_EFI_BUFFER_TOO_SMALL;}
  memcpy(data,fixture_conin,sizeof(fixture_conin));*size=sizeof(fixture_conin);
  if(fixture_unknown_conin)((unsigned char*)data)[0]=2;
  return GRUB_EFI_SUCCESS;
}
static grub_efi_status_t __grub_efi_api mock_locate_protocol(grub_guid_t *guid,void *reg,void **protocol) {
  (void)reg;
  if(osk_available&&!memcmp(guid,&d1_osk_guid,sizeof(*guid))){*protocol=&osk_interface;return GRUB_EFI_SUCCESS;}
  if(fixture_swm_present&&!memcmp(guid,&d1_swm_guid,sizeof(*guid))){*protocol=&fixture_swm_protocol;return GRUB_EFI_SUCCESS;}
  *protocol=NULL;return GRUB_EFI_NOT_FOUND;
}
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
  boot_services.locate_protocol=mock_locate_protocol;
  boot_services.locate_device_path=mock_locate_panel;
  boot_services.locate_handle=mock_leaf_handles;boot_services.connect_controller=mock_connect_osk;
  boot_services.open_protocol_information=mock_absolute_owner;boot_services.free_pool=mock_free_pool;
  grub_efi_system_table=&system_table;grub_efi_is_finished=0;grub_errno=0;
  io[0][0]=io[1][0]=0x00960000;io[0][3]=io[1][3]=0x700;
  io[0][0x730/4]=0x44000102;io[1][0x860/4]=io[1][0x870/4]=0x84000102;
  m1_enabled=0;m1_log_file=NULL;fake_tick=raw_reads=map_reads=gcd_mode=map_mode=held_initial=change_config=0;
  fixture_map_delay_ticks=0;
  m1_log_pending_bytes=m1_log_busy=m1_log_metadata_lines=m1_log_force_after_io=m1_log_drain_depth=0;
  m1_log_inflight_bytes=0;
  m1_log_io_ms=m1_log_write_calls=m1_log_flush_calls=m1_log_max_write_ms=m1_log_max_flush_ms=0;
  m1_log_last_io_ms=m1_log_last_io_end_ms=m1_log_reentrant_overflow=0;
  d1_osk_scope_active=0;osk_available=osk_get_calls=osk_set_calls=0;osk_mode=7;
  direct_poll_policy=0;
  published_leaf_count=0;fixture_leaf_mode=installed_leaf_count=0;
  published_leaf_calls=fixture_osk_controller_available=fixture_connect_calls=fixture_connect_failure=0;
  fixture_connect_delay_ticks=0;
  empty_input_policy=fixture_hid_present=fixture_usb_present=fixture_usb_failure=fixture_usb_reads=fixture_unknown_conin=0;
  fixture_usb_class=9;
  d1_input_snapshot_empty_idle=0;
  d1_input_osk_wait_required=0;
  d1_input_touch_wait_required=0;d1_touch_probe_enabled=0;d1_fw_boot_context_recorded=0;
  d1_first_EFI_return_seen=d1_first_post_EFI_sample_recorded=0;
  fixture_touch_present=fixture_touch_have_absolute=fixture_touch_bound=1;fixture_swm_present=1;
  fixture_touch_connect_calls=fixture_touch_connect_failure=fixture_touch_connect_delay_ticks=fixture_touch_unreadable=fixture_touch_unknown_inventory=fixture_touch_other_owner=0;
  fixture_osk_show_calls=fixture_swm_configured=fixture_swm_released=0;fixture_osk_keyboard_visible=fixture_osk_icon_visible=1;
  fixture_reviewed_pci_available=fixture_reviewed_pci_connect_calls=fixture_reviewed_pci_bad_identity=0;
  fixture_reviewed_pointer_available=fixture_reviewed_pointer_connect_calls=0;
  fixture_reviewed_pointer_fail=fixture_reviewed_pointer_delay=0;
  fixture_pci=(D1_PCI_PREFIX){.pci_read=mock_pci_read,.location=mock_pci_location};
  memset(&fixture_swm_protocol,0,sizeof(fixture_swm_protocol));d1_osk_entry_suspended=0;
  fixture_absolute=(D1_ABSOLUTE_PREFIX){.reset=unused_pointer,.get_state=unused_pointer,.wait=(void*)99,.mode=&fixture_absolute_mode};
  fixture_absolute_mode=(D1_ABSOLUTE_MODE){.max_x=1024,.max_y=1024};
  fixture_pointer_producer=(D1_HID_POINTER_PREFIX){.register_callback=unused_pointer,.unregister_callback=unused_pointer};
  fixture_swm_driver=(D1_DRIVER_PREFIX){.supported=unused_pointer,.start=unused_pointer,.stop=unused_pointer,.image=&fixture_swm_handle,.binding=&fixture_swm_handle};
  fixture_touch_path[0]=(grub_efi_device_path_t){.type=3,.subtype=5,.length=4};fixture_touch_path[1]=(grub_efi_device_path_t){.type=127,.subtype=255,.length=4};
  d1_input_scan_count=d1_input_publish_count=d1_input_fallback_entries=d1_input_previous_fallback=0;
  d1_input_inventory_cached=d1_input_full_scans=d1_input_stable_checks=0;
  memset(d1_input_previous_inventory_count,0,sizeof(d1_input_previous_inventory_count));
  d1_input_aggregate_handle=NULL;d1_input_aggregate_ex=NULL;
  memset(&fixture_runtime,0,sizeof(fixture_runtime));fixture_runtime.get_variable=mock_get_conin;
  system_table.runtime_services=&fixture_runtime;
  const unsigned char conin[]={3,15,11,0,255,255,255,255,3,1,1,127,1,4,0,
    1,4,20,0,0xd4,0x78,0xbb,0xbe,0x74,0xa7,0x48,0x41,0xae,0x8b,0x16,0x22,0xf2,0x43,0x69,0xf9,127,255,4,0};
  memcpy(fixture_conin,conin,sizeof(conin));
  d1_input_leaves_enabled=0;d1_input_snapshot_count=0;d1_input_discovery_reports=0;
  memset(fixture_leaf,0,sizeof(fixture_leaf));
  for(unsigned i=0;i<3;i++){
    fixture_leaf[i].ex.read_key_stroke=leaf_read_ex;fixture_leaf[i].basic.read_key_stroke=leaf_read_basic;
    fixture_leaf[i].path[0]=(grub_efi_device_path_t){.type=3,.subtype=5,.length=4};
    fixture_leaf[i].path[1]=(grub_efi_device_path_t){.type=127,.subtype=255,.length=4};
  }
  log_used=0;memset(log_data,0,sizeof(log_data));
}
static void provide_osk_controller(void) {
  grub_guid_t path_guid={0xad603516,0xd94a,0x425e,{0x9b,0x42,0x0b,0x55,0xfd,0xdb,0xd8,0xa1}};
  fixture_osk_controller_available=1;system_table.console_in_handler=(void*)1;
  boot_services.locate_handle=mock_leaf_handles;boot_services.connect_controller=mock_connect_osk;
  memset(fixture_leaf[0].path,0,sizeof(fixture_leaf[0].path));
  fixture_leaf[0].path[0]=(grub_efi_device_path_t){.type=1,.subtype=4,.length=20};
  memcpy((unsigned char*)fixture_leaf[0].path+4,&path_guid,sizeof(path_guid));
  fixture_leaf[0].path[5]=(grub_efi_device_path_t){.type=127,.subtype=255,.length=4};
}
static void attach_log(void) { m1_log_file=&memory_log;m1_log_bytes=0;strcpy(m1_log_name,"GM0001.LOG"); }
static void read_exact(const char *path,void *data,size_t bytes) {
  FILE *f=fopen(path,"rb");assert(f);assert(fread(data,1,bytes,f)==bytes);assert(fgetc(f)==EOF);fclose(f);
}
int main(int argc,char **argv) {
  unsigned before;
  assert(argc==3);read_exact(argv[1],dsdt_original,sizeof(dsdt_original));read_exact(argv[2],ssdt_original,sizeof(ssdt_original));
  setup();osk_available=1;attach_log();assert(m1_enable()==0);
  assert(!grub_term_input_trace_hook);
  d1_start_input_trace (); assert(grub_term_input_trace_hook==d1_trace_input_component);
  for(unsigned trace_case=0;trace_case<12;trace_case++)
    d1_trace_input_component("net","idle_cards",100,1164,GRUB_TERM_NO_KEY);
  assert(d1_dispatch_slow_count[2]==12&&d1_dispatch_reports[2]==8&&d1_dispatch_max_ms[2]==1064);
  d1_trace_input_component("terminal","console",100,145,GRUB_TERM_KEY_UP);
  m1_log_flush_pending();
  assert(strstr((char*)log_data,"INPUT_COMPONENT phase=net name=idle_cards begin_ms=100 end_ms=1164 total_ms=1064 key_class=none"));
  assert(strstr((char*)log_data,"INPUT_COMPONENT phase=terminal name=console"));
  d1_stop_input_trace();assert(!grub_term_input_trace_hook);
  m1_log_flush_pending();assert(strstr((char*)log_data,"INPUT_COMPONENT_SUMMARY phase=net slow_calls=12 max_ms=1064 reports=8"));
  puts("PASS real module records per-component slow intervals with bounded reports and clears observer before stop");
  assert(osk_get_calls==1&&!osk_set_calls&&osk_mode==7&&d1_osk_scope_active);
  assert(direct_poll_policy==1);
  assert(strstr((char*)log_data,"OSK MODE_RETAINED"));
  d1_start_input_trace();assert(grub_term_input_trace_hook==d1_trace_input_component);
  assert(m1_preboot(0)==0);assert(osk_mode==7&&!osk_set_calls&&!d1_osk_scope_active);
  assert(!grub_term_input_trace_hook);
  assert(direct_poll_policy==0);
  grub_errno=GRUB_ERR_BAD_OS;assert(m1_restore()==0&&grub_errno==GRUB_ERR_BAD_OS);
  assert(!grub_term_input_trace_hook);
  attach_log(); /* This fixture does not provide a filesystem for reopening logs. */
  d1_start_input_trace();assert(grub_term_input_trace_hook==d1_trace_input_component);
  d1_stop_input_trace();assert(!grub_term_input_trace_hook);
  assert(osk_mode==7&&d1_osk_scope_active&&!osk_set_calls);
  assert(direct_poll_policy==1);
  m1_stop("osk_test_return");assert(osk_mode==7&&!osk_set_calls&&!d1_osk_scope_active);
  assert(direct_poll_policy==0);
  setup();osk_available=1;osk_mode=4;assert(m1_enable()==0);
  assert(direct_poll_policy==1&&d1_osk_scope_active&&!osk_set_calls);
  m1_stop("already_disabled_osk");assert(!direct_poll_policy&&osk_mode==4&&!osk_set_calls);
  setup();assert(m1_enable()==0);assert(direct_poll_policy==0);
  m1_stop("unknown_osk_preserves_event_gate");
  setup();osk_available=1;installed_leaf_count=2;
  provide_osk_controller();
  attach_log();assert(m1_enable()==0);assert(published_leaf_count==2);
  assert(published_leaves[0].ex==&fixture_leaf[0].ex&&!published_leaves[0].basic);
  assert(published_leaves[1].ex==&fixture_leaf[1].ex&&!published_leaves[1].basic);
  m1_log_flush_pending (); /* Inspect deferred metadata after an explicit checkpoint. */
  assert(strstr((char*)log_data,"EFI_INPUT_ROUTE leaves=2 aggregate_fallback=0"));
  assert(strstr((char*)log_data,"EFI_INPUT_DEVICE_PATH source=0"));
  assert(!fixture_connect_calls&&!osk_set_calls&&osk_mode==7);
  unsigned initial_publishes=published_leaf_calls;
  fake_tick+=100;m1_terminal_getkey(NULL);assert(published_leaf_calls==initial_publishes);
  assert(d1_input_full_scans==1&&d1_input_stable_checks==1);
  installed_leaf_count=3;fake_tick+=100;m1_terminal_getkey(NULL);assert(published_leaf_count==3);
  assert(m1_preboot(0)==0);assert(!published_leaf_count&&!d1_input_leaves_enabled&&!direct_poll_policy);
  assert(m1_restore()==0);assert(published_leaf_count==3&&direct_poll_policy);
  m1_stop("leaf_route_complete");assert(!published_leaf_count);
  puts("PASS endpoint discovery skips the aggregate, deduplicates Basic/Ex, records paths without key reads, refreshes hotplug and retracts on handoff/return");
  setup();osk_available=1;installed_leaf_count=2;system_table.console_in_handler=(void*)1;
  provide_osk_controller();fixture_leaf[1].path[0].length=0;
  assert(m1_enable()==0&&!published_leaf_count&&direct_poll_policy);
  m1_stop("malformed_path_fallback");
  setup();osk_available=1;installed_leaf_count=2;system_table.console_in_handler=(void*)1;
  provide_osk_controller();fixture_leaf[1].ex.read_key_stroke=(void*)0x8000;
  fixture_leaf[1].basic.read_key_stroke=(void*)0x8000;
  assert(m1_enable()==0&&!published_leaf_count);
  m1_stop("uncallable_protocol_fallback");
  setup();osk_available=1;installed_leaf_count=2;system_table.console_in_handler=(void*)1;
  provide_osk_controller();fixture_leaf_mode=3;
  assert(m1_enable()==0&&!published_leaf_count);
  m1_stop("capacity_fallback");
  puts("PASS malformed/unreadable input, uncallable code or inventory overflow retains the original aggregate rather than dropping a device");
  setup();osk_available=1;system_table.console_in_handler=(void*)1;
  boot_services.locate_handle=mock_leaf_handles;attach_log();assert(m1_enable()==0);
  assert(!empty_input_policy&&!published_leaf_count&&direct_poll_policy);
  assert(strstr((char*)log_data,"reason=OSK-input-not-ready"));
  fixture_hid_present=1;fake_tick+=100;m1_terminal_getkey(NULL);assert(!empty_input_policy);
  fixture_hid_present=0;installed_leaf_count=1;provide_osk_controller();fake_tick+=100;m1_terminal_getkey(NULL);
  assert(published_leaf_count==1&&!empty_input_policy);
  m1_stop("empty_then_keyboard");assert(!empty_input_policy&&!published_leaf_count&&!direct_poll_policy);
  puts("PASS missing OSK keeps compatibility instead of declaring it unnecessary; hotplugged OSK remains in the live snapshot");
  setup();osk_available=1;provide_osk_controller();attach_log();assert(m1_enable()==0);
  assert(fixture_connect_calls==1&&published_leaf_count==1&&published_leaves[0].ex==&fixture_leaf[0].ex);
  assert(osk_mode==7&&!osk_set_calls&&!empty_input_policy);
  assert(strstr((char*)log_data,"targeted_connect=1 recursive=0")&&strstr((char*)log_data,"input_ready=1"));
  before=published_leaf_calls;fake_tick+=100;m1_terminal_getkey(NULL);assert(published_leaf_calls==before&&fixture_connect_calls==1);
  fixture_hid_present=1;fake_tick+=100;m1_terminal_getkey(NULL);
  assert(!published_leaf_count&&d1_input_previous_fallback);
  fixture_hid_present=0;fake_tick+=100;m1_terminal_getkey(NULL);
  assert(published_leaf_count==1&&!d1_input_previous_fallback);
  fixture_hid_present=1;installed_leaf_count=2;fake_tick+=100;m1_terminal_getkey(NULL);
  assert(!published_leaf_count&&d1_input_previous_fallback);
  fixture_hid_present=0;fake_tick+=100;m1_terminal_getkey(NULL);
  assert(published_leaf_count==2&&!d1_input_previous_fallback);
  m1_stop("targeted_OSK_complete");assert(!published_leaf_count&&!empty_input_policy&&!direct_poll_policy&&osk_mode==7);
  puts("PASS exact OSK controller connects once without recursive USB/default-ConIn scan; original icon/self-refresh modes and stable snapshot remain intact");
  puts("PASS an attached keyboard producer without a published keyboard input retains aggregate compatibility alongside OSK");
  setup();osk_available=1;provide_osk_controller();fixture_connect_failure=1;
  attach_log();assert(m1_enable()==0&&fixture_connect_calls==1&&!published_leaf_count&&!empty_input_policy);
  assert(d1_input_previous_fallback&&d1_input_fallback_entries==1);
  for(unsigned i=0;i<3;i++){fake_tick+=100;m1_terminal_getkey(NULL);}
  assert(d1_input_fallback_entries==1&&fixture_connect_calls==1);
  m1_stop("targeted_OSK_failed_compatibility");
  puts("PASS failed targeted OSK initialization retains compatibility, does not reconnect every poll and counts fallback transitions accurately");
  setup();osk_available=1;provide_osk_controller();fixture_connect_delay_ticks=250;
  attach_log();assert(m1_enable()==0&&fixture_connect_calls==1&&published_leaf_count==1);
  assert(strstr((char*)log_data,"connect_ms=2500 total_ms=2500")&&osk_mode==7&&!osk_set_calls);
  m1_stop("slow_OSK_init_observed");
  puts("PASS slow targeted firmware initialization is explicitly logged at its own boundary, never hidden as an earlier speedup");
  setup();osk_available=1;system_table.console_in_handler=(void*)1;boot_services.locate_handle=mock_leaf_handles;
  fixture_usb_present=1;assert(m1_enable()==0);assert(!empty_input_policy&&fixture_usb_reads==1);
  fixture_usb_class=3;fake_tick+=100;m1_terminal_getkey(NULL);assert(!empty_input_policy);
  m1_stop("usb_HID_fallback");
  setup();osk_available=1;system_table.console_in_handler=(void*)1;boot_services.locate_handle=mock_leaf_handles;
  fixture_usb_present=fixture_usb_failure=1;assert(m1_enable()==0&&!empty_input_policy);
  m1_stop("unknown_USB_fallback");
  setup();osk_available=1;system_table.console_in_handler=(void*)1;boot_services.locate_handle=mock_leaf_handles;
  fixture_unknown_conin=1;assert(m1_enable()==0&&!empty_input_policy);m1_stop("unknown_ConIn_fallback");
  puts("PASS USB/presence checks do not disable a missing OSK; HID/failed descriptors and unfamiliar console retain compatibility");
  puts("PASS direct endpoint route requires validated OSK, preserves its original modes, survives nested contexts and clears before return");
  puts("PASS actual OSK mode is never written, hidden, reset or excluded; failures retain the ordinary input path");
  setup();osk_available=1;provide_osk_controller();fixture_touch_have_absolute=0;
  attach_log();assert(m1_enable()==0);
  assert(fixture_touch_connect_calls==1&&fixture_touch_have_absolute&&fixture_touch_bound&&published_leaf_count==1);
  assert(strstr((char*)log_data,"kind=existing_touch_panel")&&strstr((char*)log_data,"aggregate_compatibility_required=0"));
  fake_tick+=100;m1_terminal_getkey(NULL);assert(fixture_touch_connect_calls==1);
  m1_stop("touch_producer_initialized");
  puts("PASS existing touch producer without AbsolutePointer connects once, preserves raw queue ownership and OSK mode, and exposes all init timing");
  setup();osk_available=1;provide_osk_controller();fixture_touch_bound=0;
  attach_log();assert(m1_enable()==0&&fixture_touch_connect_calls==1&&fixture_touch_bound&&published_leaf_count==1);
  assert(strstr((char*)log_data,"kind=existing_SWM_binding"));
  m1_stop("SWM_unowned_pointer_bound");
  puts("PASS existing verified SWM binds only an unowned provider; GRUB never reads GetState, resets or registers a competing touch callback");
  setup();osk_available=1;provide_osk_controller();fixture_touch_have_absolute=0;fixture_touch_connect_failure=1;
  attach_log();assert(m1_enable()==0&&fixture_touch_connect_calls==1&&!published_leaf_count);
  assert(d1_input_touch_wait_required&&!empty_input_policy);
  for(unsigned i=0;i<3;i++){fake_tick+=100;m1_terminal_getkey(NULL);}
  assert(fixture_touch_connect_calls==1&&!published_leaf_count);
  assert(strstr((char*)log_data,"reason=touch-route-needs-compatibility"));
  m1_stop("touch_init_failure_preserves_aggregate");
  setup();osk_available=1;provide_osk_controller();fixture_touch_present=0;
  attach_log();assert(m1_enable()==0&&d1_input_touch_wait_required&&!published_leaf_count&&!fixture_touch_connect_calls);
  m1_stop("no_touch_route_preserves_aggregate");
  puts("PASS missing or failed touch route keeps aggregate compatibility, cannot declare OSK fully functional from its text protocol, and never reconnects every poll");
  setup();osk_available=1;provide_osk_controller();fixture_touch_have_absolute=0;fixture_touch_connect_delay_ticks=250;
  attach_log();assert(m1_enable()==0);
  assert(strstr((char*)log_data,"kind=existing_touch_panel")&&strstr((char*)log_data,"elapsed_ms=2500"));
  assert(osk_mode==7&&!osk_set_calls);m1_stop("slow_touch_init_logged");
  setup();osk_available=1;provide_osk_controller();fixture_absolute_mode.max_x=0;
  attach_log();assert(m1_enable()==0&&!published_leaf_count&&d1_input_touch_wait_required);
  m1_stop("invalid_pointer_mode_compatibility");
  puts("PASS slow targeted touch init is explicitly measured and invalid pointer mode never bypasses compatibility; no hidden shift of startup waiting");
  setup();osk_available=1;provide_osk_controller();fixture_touch_bound=0;fixture_touch_other_owner=1;
  attach_log();assert(m1_enable()==0&&!fixture_touch_connect_calls&&!published_leaf_count);
  assert(strstr((char*)log_data,"other_driver_owner=1"));m1_stop("other_pointer_owner_preserved");
  setup();osk_available=1;provide_osk_controller();fixture_touch_unknown_inventory=1;
  attach_log();assert(m1_enable()==0&&!fixture_touch_connect_calls&&!published_leaf_count);
  assert(strstr((char*)log_data,"inventory_known=0"));m1_stop("unknown_touch_inventory_compatibility");
  puts("PASS another pointer driver is never displaced and unknown/overflow touch inventory retains compatibility without connection attempts");
  setup();attach_log();d1_record_firmware_boot_context();
  m1_log_flush_pending (); /* Firmware context metadata must be checkpointed before inspection. */
  assert(strstr((char*)log_data,"FW_VAR name=BootCurrent")&&strstr((char*)log_data,"FW_VAR name=BootNext")&&strstr((char*)log_data,"FW_VAR name=ConIn"));
  size_t context_bytes=log_used;d1_record_firmware_boot_context();assert(log_used==context_bytes);
  grub_efi_is_finished=1;d1_initialize_touch_routes();d1_touch_routes_end();
  assert(!fixture_touch_connect_calls);grub_efi_is_finished=0;
  puts("PASS boot/console variable diagnosis is bounded and read-only; cold/warm restart is not invented from BootCurrent; no hardware work after ExitBootServices");
  setup();assert(m1_enable()==0);attach_log();d1_first_EFI_return_seen=1;fake_tick=3;m1_terminal_getkey(NULL);
  m1_log_flush_pending ();
  assert(strstr((char*)log_data,"FIRST_SAMPLE_AFTER_EFI_RETURN sample_tick=3 time_ms=30"));
  size_t sample_bytes=log_used;fake_tick=4;m1_terminal_getkey(NULL);assert(log_used==sample_bytes);
  puts("PASS the first post-EFI sampling timestamp is independent of a user's idle time or first physical key");
  setup();osk_available=1;provide_osk_controller();fixture_touch_present=0;fixture_reviewed_pci_available=1;
  attach_log();assert(m1_enable()==0&&fixture_reviewed_pci_connect_calls==1&&fixture_touch_present&&published_leaf_count==1);
  assert(strstr((char*)log_data,"identity=0xa0d08086")&&strstr((char*)log_data,"connect_attempted=1 recursive=0"));
  fake_tick+=100;m1_terminal_getkey(NULL);assert(fixture_reviewed_pci_connect_calls==1);
  m1_stop("reviewed_panel_connected");
  setup();osk_available=1;provide_osk_controller();fixture_touch_present=0;fixture_reviewed_pci_available=1;fixture_reviewed_pci_bad_identity=1;
  attach_log();assert(m1_enable()==0&&!fixture_reviewed_pci_connect_calls&&!published_leaf_count);
  assert(strstr((char*)log_data,"identity_or_callable_guard_refused=1"));m1_stop("foreign_panel_not_connected");
  puts("PASS exact already-existing PCI0:00:10.6 Intel THC controller connects once before published touch protocols; wrong identity is never connected");
  setup();osk_available=1;provide_osk_controller();fixture_reviewed_pointer_available=1;fixture_touch_have_absolute=0;
  static const unsigned char reviewed_pointer_path[]={1,4,20,0,0xd4,0x78,0xbb,0xbe,0x74,0xa7,0x48,0x41,0xae,0x8b,0x16,0x22,0xf2,0x43,0x69,0xf9,127,255,4,0};
  memcpy(fixture_leaf[2].path,reviewed_pointer_path,sizeof(reviewed_pointer_path));
  attach_log();d1_touch_attempted_count=0;d1_initialize_reviewed_pointer_controller();
  assert(fixture_reviewed_pointer_connect_calls==1&&fixture_touch_have_absolute&&fixture_touch_bound);
  d1_initialize_reviewed_pointer_controller();assert(fixture_reviewed_pointer_connect_calls==1);
  assert(strstr((char*)log_data,"exact_path=BEBB78D4_GM15")&&strstr((char*)log_data,"recursive=0"));
  fixture_touch_have_absolute=0;d1_touch_attempted_count=0;fixture_leaf[2].path[0].subtype=5;
  d1_initialize_reviewed_pointer_controller();assert(fixture_reviewed_pointer_connect_calls==1);
  memcpy(fixture_leaf[2].path,reviewed_pointer_path,sizeof(reviewed_pointer_path));
  fixture_reviewed_pointer_fail=1;fixture_reviewed_pointer_delay=250;
  d1_initialize_reviewed_pointer_controller();assert(fixture_reviewed_pointer_connect_calls==2&&!fixture_touch_have_absolute);
  assert(strstr((char*)log_data,"total_ms=2500"));
  d1_initialize_reviewed_pointer_controller();assert(fixture_reviewed_pointer_connect_calls==2);
  puts("PASS exact GM15 BEBB controller connects once when already present; wrong path rejected and failed2500ms connection remains logged without retry loops");
  setup();osk_available=1;provide_osk_controller();attach_log();assert(m1_enable()==0);
  assert(fixture_osk_keyboard_visible&&fixture_osk_icon_visible&&!fixture_osk_show_calls);
  d1_prepare_entry_ui();assert(!fixture_osk_keyboard_visible&&!fixture_osk_icon_visible&&fixture_osk_show_calls==2);
  assert(!direct_poll_policy&&!published_leaf_count&&d1_osk_entry_suspended&&osk_mode==7&&!osk_set_calls);
  assert(strstr((char*)log_data,"DISPLAY_EXIT reason=entry_selected")&&strstr((char*)log_data,"before_GRUB_clear=1"));
  d1_prepare_entry_ui();assert(fixture_osk_show_calls==2);
  d1_resume_entry_ui();assert(direct_poll_policy&&published_leaf_count==1&&!d1_osk_entry_suspended);
  assert(osk_mode==7&&!osk_set_calls&&strstr((char*)log_data,"ENTRY_RETURN"));
  m1_stop("selected_entry_failed_return");
  puts("PASS original OSK remains visible/usable while choosing, closes before selected-entry clear, and its input/mode are restored after failed entry");
  setup();assert(m1_enable()==0);assert(d1_gcd_queries==0);attach_log();
  for(fake_tick=0;fake_tick<30;fake_tick++)m1_terminal_getkey(NULL);
  assert(m1_enabled && m1_actions.delivered_count[1]==1);
  assert(m1_preboot(7)==0);
  assert(!m1_enabled && !m1_log_file && !m1_actions.count && !m1_actions.power_pending);
  assert(strstr((char*)log_data,"D1 BOOT HANDOFF chosen=test-selected-os loader_flags=7"));
  assert(strstr((char*)log_data,"END reason=system_boot"));
  before=raw_reads;fake_tick++;assert(m1_terminal_getkey(NULL)==0 && raw_reads==before);
  grub_errno=GRUB_ERR_BAD_OS;
  assert(m1_restore()==0 && grub_errno==GRUB_ERR_BAD_OS && m1_enabled);
  puts("PASS changed D1 preboot flushes END, closes log, disables sampling/queued keys; loader return restores GPIO and preserves original error");
  setup();gcd_mode=1;assert(m1_enable()==0);assert(d1_gcd_queries>0);m1_stop("GCD_fallback_verified");
  puts("PASS direct uncached memory map skips redundant DXE lookup, reserved apertures still use current GCD validation");
  setup();assert(m1_enable()==0);attach_log();assert(m1_preboot(0)==0);
  before=raw_reads;map_mode=1;grub_errno=GRUB_ERR_BAD_OS;
  assert(m1_restore()==0 && !m1_enabled && raw_reads==before && grub_errno==GRUB_ERR_BAD_OS);
  puts("PASS failed revalidation after loader return does not replace boot error or force physical reads");
  setup();assert(m1_enable()==0);attach_log();assert(m1_preboot(0)==0);
  before=raw_reads;grub_efi_is_finished=1;
  assert(m1_restore()==0 && !m1_enabled && raw_reads==before);
  puts("PASS no diagnostics restart or hardware access after ExitBootServices");
  setup();attach_log();grub_errno=GRUB_ERR_BAD_SIGNATURE;
  d1_log_error(grub_errno,"synthetic signature refusal");
  assert(grub_errno==GRUB_ERR_BAD_SIGNATURE && strstr((char*)log_data,"D1 ERROR errno="));
  assert(strstr((char*)log_data,"synthetic signature refusal"));
  puts("PASS new error sink records actual message without altering grub_errno");
  setup();assert(m1_enable()==0);attach_log();fake_tick=1;m1_terminal_getkey(NULL);
  fake_tick=100;fixture_map_delay_ticks=105;m1_terminal_getkey(NULL);
  assert(m1_last_sample_ms==2050&&m1_last_map_ms==2050);
  assert(strstr((char*)log_data,"MAINTENANCE memory_map_ms=1050")&&strstr((char*)log_data,"SAMPLE_GAP gap_ms=2040"));
  assert(!m1_actions.power_pending&&!m1_actions.return_chord_active);
  puts("PASS1050ms EFI maintenance is attributed and actual GPIO sampling uses post-return time, never a false pre-call debounce timestamp");
  puts("PASS this is D1 lifecycle simulation, not a repeated physical G1/M1 acceptance test");
  return 0;
}
