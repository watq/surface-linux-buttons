/* Time the real GRUB dispatch without changing key order or error state.
 * All clocks and callbacks are in memory; no EFI/desktop/device access. */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define GRUB_UTIL 1
#define GRUB_SP8_POLL_TEST 1
#define GRUB_FILE "test-sp8-grub-input-dispatch-trace.c"
#include "../grub-buttons/src/grub-core/kern/term.c"

grub_err_t grub_errno;
char grub_errmsg[GRUB_MAX_ERRMSG];
void *grub_memmove (void *d, const void *s, grub_size_t n) { return memmove(d,s,n); }
static grub_uint64_t fake_now;
static unsigned clock_reads, order[16], order_count;
static unsigned slow_case, withdraw_trace;
static int pending_key;
struct observed {
  char phase[16], name[48];
  grub_uint64_t begin, end;
  int key;
};
static struct observed rows[16];
static unsigned row_count;

grub_uint64_t grub_get_time_ms (void) { clock_reads++; return fake_now; }
int grub_snprintf (char *s, grub_size_t n, const char *fmt, ...) {
  va_list args; int rc; va_start(args,fmt); rc=vsnprintf(s,n,fmt,args);va_end(args);return rc;
}
static void record (const char *phase, const char *name, grub_uint64_t begin,
                    grub_uint64_t end, int key) {
  struct observed *r=&rows[row_count++];
  assert(row_count<=16);
  snprintf(r->phase,sizeof(r->phase),"%s",phase);
  snprintf(r->name,sizeof(r->name),"%s",name);
  r->begin=begin;r->end=end;r->key=key;
  /* Diagnostic delay must remain outside each returned operation interval.
     Recursive reporting and error corruption are intercepted by the real core. */
  fake_now+=47;
  grub_errno=GRUB_ERR_BAD_DEVICE;strcpy(grub_errmsg,"diagnostic error");
  grub_term_trace_completed(record,"net","recursive",begin,key);
}
static void sample (void) {
  order[order_count++]=1;fake_now+=slow_case?33:2;
}
static void usb (int wait) {
  assert(!wait);order[order_count++]=2;fake_now+=slow_case?71:2;
}
static void net (void) {
  order[order_count++]=3;fake_now+=slow_case?1064:2;
  if(withdraw_trace)grub_term_input_trace_hook=NULL;
}
static int console_key (struct grub_term_input *term) {
  (void)term;order[order_count++]=4;fake_now+=slow_case?25:2;return GRUB_TERM_NO_KEY;
}
static int keyboard_key (struct grub_term_input *term) {
  (void)term;order[order_count++]=5;fake_now+=slow_case?26:2;
  grub_errno=GRUB_ERR_BAD_SIGNATURE;strcpy(grub_errmsg,"driver error retained");
  return pending_key;
}
static int gpio_key (struct grub_term_input *term) {
  (void)term;order[order_count++]=6;fake_now+=2;return GRUB_TERM_NO_KEY;
}
static struct grub_term_input gpio={.name="sp8_gpio",.getkey=gpio_key};
static struct grub_term_input keyboard={.name="ordinary-keyboard",.next=&gpio,.getkey=keyboard_key};
static struct grub_term_input console={.name="console",.next=&keyboard,.getkey=console_key};
static void setup (void) {
  fake_now=100;clock_reads=order_count=row_count=slow_case=withdraw_trace=0;
  pending_key=GRUB_TERM_KEY_UP;memset(rows,0,sizeof(rows));grub_term_inputs=&console;
  grub_term_poll_input_hook=sample;grub_term_poll_usb=usb;grub_net_poll_cards_idle=net;
  grub_term_input_trace_hook=NULL;grub_errno=GRUB_ERR_NONE;grub_errmsg[0]=0;
}
int main (void) {
  int key;
  setup();key=grub_getkey_noblock();
  assert(key==GRUB_TERM_KEY_UP&&order_count==5&&!clock_reads&&!row_count);
  for(unsigned i=0;i<5;i++)assert(order[i]==i+1);
  setup();slow_case=1;grub_term_input_trace_hook=record;key=grub_getkey_noblock();
  assert(key==GRUB_TERM_KEY_UP&&order_count==5&&row_count==5);
  for(unsigned i=0;i<5;i++)assert(order[i]==i+1);
  assert(!strcmp(rows[0].phase,"sample")&&rows[0].end-rows[0].begin==33);
  assert(!strcmp(rows[1].phase,"usb")&&rows[1].end-rows[1].begin==71);
  assert(!strcmp(rows[2].phase,"net")&&rows[2].end-rows[2].begin==1064);
  assert(!strcmp(rows[3].name,"console")&&rows[3].end-rows[3].begin==25&&rows[3].key==GRUB_TERM_NO_KEY);
  assert(!strcmp(rows[4].name,"ordinary-keyboard")&&rows[4].end-rows[4].begin==26&&rows[4].key==GRUB_TERM_KEY_UP);
  assert(grub_errno==GRUB_ERR_BAD_SIGNATURE&&!strcmp(grub_errmsg,"driver error retained"));
  puts("PASS real dispatch attributes injected 1064ms network phase, USB/sample/terminal phases independently; original key order and error survive observer I/O/reentry");
  setup();grub_term_input_trace_hook=record;key=grub_getkey_noblock();
  assert(key==GRUB_TERM_KEY_UP&&!row_count);
  setup();slow_case=withdraw_trace=1;grub_term_input_trace_hook=record;key=grub_getkey_noblock();
  assert(key==GRUB_TERM_KEY_UP&&row_count==2&&!grub_term_input_trace_hook);
  puts("PASS short operations emit no records; trace withdrawal avoids stale callback; absent observer adds no time reads");
  setup();pending_key='\r';grub_term_input_trace_hook=record;
  assert(grub_getkey_noblock()=='\r'&&!row_count);
  setup();pending_key=GRUB_TERM_NO_KEY;grub_term_input_trace_hook=record;
  assert(grub_getkey_noblock()==GRUB_TERM_NO_KEY&&order_count==6&&!row_count);
  puts("PASS Enter and empty queues retain original exactly-once dispatch; no host input/device operations");
  return 0;
}
