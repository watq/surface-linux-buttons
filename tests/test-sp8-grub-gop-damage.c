/* The real framebuffer damage path + EFI GOP presenter on copied heap pixels. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#define GRUB_UTIL 1
#define GRUB_SP8_POLL_TEST 1
#define GRUB_FILE "test-sp8-grub-gop-damage.c"
/* The two upstream C units use a file-local variable named framebuffer. */
#define framebuffer test_fb_state
#include "../grub-buttons/src/grub-core/video/fb/video_fb.c"
#undef framebuffer
#undef GRUB_MOD_LICENSE
#define GRUB_MOD_LICENSE(value)
#undef GRUB_MOD_INIT
#undef GRUB_MOD_FINI
#define GRUB_MOD_INIT(name) static void test_unused_init_ ## name (void)
#define GRUB_MOD_FINI(name) static void test_unused_fini_ ## name (void)
#include "../grub-buttons/src/grub-core/video/fb/fbfill.c"
#include "../grub-buttons/src/grub-core/video/efi_gop.c"

grub_err_t grub_errno;
char grub_errmsg[GRUB_MAX_ERRMSG];
void *grub_malloc(grub_size_t n) { return malloc(n); }
void grub_free(void *p) { free(p); }
void *grub_memmove(void *d,const void *s,grub_size_t n) { return memmove(d,s,n); }
void *grub_memset(void *d,int c,grub_size_t n) { return memset(d,c,n); }
grub_err_t grub_error(grub_err_t code,const char *fmt,...) {(void)fmt;grub_errno=code;return code;}
int grub_snprintf(char *buf,grub_size_t n,const char *fmt,...) {
  int ret;va_list args;va_start(args,fmt);ret=vsnprintf(buf,n,fmt,args);va_end(args);return ret;
}
const char *grub_env_get(const char *name) {(void)name;return "1";}
struct grub_command *grub_command_list;
void *grub_named_list_find(grub_named_list_t list,const char *name) {(void)list;(void)name;return NULL;}
static unsigned fake_ms,samples,blit_calls,max_height,draw_pixels;
static unsigned compositor_available,compositor_calls,compositor_release,firmware_overlay_active,overlay_restore_capture_cycles;
int grub_efi_console_swm_surface(unsigned width,unsigned height) {assert(width==640&&height==480);return compositor_available;}
void grub_efi_console_release_swm(void) {compositor_release++;}
grub_uint64_t grub_get_time_ms(void) {return fake_ms;}
void (*grub_term_poll_input_hook)(void);
void grub_term_poll_input(void) {samples++;}
static unsigned char screen[640*480*4];
static unsigned char firmware_keyboard_background[640*160*4];
static void show_firmware_keyboard(void) {memset(screen+640*320*4,0xe7,sizeof(firmware_keyboard_background));}
grub_video_color_t get_pixel(struct grub_video_fbblit_info *s,unsigned x,unsigned y) {
  grub_video_color_t c=0;memcpy(&c,s->data+y*s->mode_info->pitch+x*4,4);return c;
}
void set_pixel(struct grub_video_fbblit_info *s,unsigned x,unsigned y,grub_video_color_t c) {
  memcpy(s->data+y*s->mode_info->pitch+x*4,&c,4);
}
static grub_efi_status_t __grub_efi_api mock_blt(struct grub_efi_gop *this,
  struct grub_efi_gop_blt_pixel *buffer,grub_efi_gop_blt_operation_t op,grub_efi_uintn_t sx,
  grub_efi_uintn_t sy,grub_efi_uintn_t dx,grub_efi_uintn_t dy,grub_efi_uintn_t width,
  grub_efi_uintn_t height,grub_efi_uintn_t delta) {
  (void)this;assert(op==GRUB_EFI_BLT_BUFFER_TO_VIDEO&&delta==640*4);
  assert(dx+width<=640&&dy+height<=480);assert(sx==dx&&sy==dy);
  if(firmware_overlay_active&&dy+height>320){
    memcpy(screen+640*320*4,firmware_keyboard_background,sizeof(firmware_keyboard_background));
    overlay_restore_capture_cycles++;fake_ms+=700;
  }
  blit_calls++;draw_pixels+=width*height;max_height=height>max_height?height:max_height;fake_ms+=height/4+1;
  for(unsigned row=0;row<height;row++)memcpy(screen+(dy+row)*640*4+dx*4,(unsigned char*)buffer+(sy+row)*delta+sx*4,width*4);
  if(firmware_overlay_active&&dy+height>320){
    memcpy(firmware_keyboard_background,screen+640*320*4,sizeof(firmware_keyboard_background));show_firmware_keyboard();
  }
  return GRUB_EFI_SUCCESS;
}
grub_efi_status_t grub_efi_console_swm_blt(struct grub_efi_gop_blt_pixel *buffer,unsigned x,unsigned y,unsigned width,unsigned height,unsigned pitch) {
  assert(compositor_available);compositor_calls++;
  return mock_blt(NULL,buffer,GRUB_EFI_BLT_BUFFER_TO_VIDEO,x,y,x,y,width,height,pitch);
}
int main(void) {
  struct grub_video_fbrender_target *display,*other;
  struct grub_video_mode_info mode={.width=640,.height=480,.pitch=640*4,.bpp=32,.bytes_per_pixel=4,
    .mode_type=GRUB_VIDEO_MODE_TYPE_RGB,.blit_format=GRUB_VIDEO_BLIT_FORMAT_BGRA_8888,
    .red_mask_size=8,.red_field_pos=16,.green_mask_size=8,.green_field_pos=8,
    .blue_mask_size=8,.blue_field_pos=0,.reserved_mask_size=8,.reserved_field_pos=24};
  struct grub_efi_gop protocol={.blt=mock_blt};
  unsigned char *shadow=calloc(640*480,4),*otherbuf=calloc(640*480,4);assert(shadow&&otherbuf);
  assert(grub_video_fb_create_render_target_from_pointer(&display,&mode,shadow)==0);
  assert(grub_video_fb_create_render_target_from_pointer(&other,&mode,otherbuf)==0);
  framebuffer.render_target=(struct grub_video_render_target*)display;
  framebuffer.mode_info=mode;framebuffer.offscreen=shadow;framebuffer.track_damage=1;gop=&protocol;
  grub_video_fb_track_damage(display);test_fb_state.render_target=display;
  assert(grub_video_fb_fill_rect(0xff102030,0,0,640,480)==0);
  assert(grub_video_gop_swap_buffers()==0);assert(blit_calls==15&&max_height==32&&samples>=15);
  assert(!memcmp(screen,shadow,sizeof(screen)));
  puts("PASS full EFI GOP frame is presented in 32-line bounded calls with input samples between, exact pixels retained");
  blit_calls=draw_pixels=samples=max_height=0;
  assert(grub_video_fb_fill_rect(0xffaabbcc,270,400,100,28)==0);
  assert(grub_video_gop_swap_buffers()==0);assert(blit_calls==1&&draw_pixels==2800&&max_height==28);
  assert(!memcmp(screen,shadow,sizeof(screen)));
  puts("PASS timeout-sized change presents only actual rectangle (2800 pixels), not full 307200-pixel screen");
  assert(grub_video_gop_swap_buffers()==0&&blit_calls==1);
  test_fb_state.render_target=other;grub_video_fb_fill_rect(0xff444444,0,0,640,480);
  assert(grub_video_gop_swap_buffers()==0&&blit_calls==1);
  test_fb_state.render_target=display;
  grub_video_fb_fill_rect(0xffabcdef,10,10,20,10);grub_video_fb_fill_rect(0xffabcdef,70,50,30,20);
  grub_video_rect_t damage;assert(grub_video_fb_peek_damage(display,&damage));
  assert(damage.x==10&&damage.y==10&&damage.width==90&&damage.height==60);
  assert(grub_video_gop_swap_buffers()==0&&!memcmp(screen,shadow,sizeof(screen)));
  puts("PASS empty updates and offscreen text-layer changes are skipped; separate screen damage rectangles merge exactly");
  compositor_available=1;blit_calls=draw_pixels=samples=max_height=0;
  test_fb_state.render_target=display;grub_video_fb_fill_rect(0xff333444,0,0,640,480);
  assert(grub_video_gop_swap_buffers()==0&&compositor_calls==1&&blit_calls==1&&max_height==480&&samples>=2);
  assert(!memcmp(screen,shadow,sizeof(screen)));
  assert(grub_video_gop_swap_buffers()==0&&compositor_calls==1);
  puts("PASS a firmware OSK compositor gets one exact full-frame transaction instead of60 overlapping restore/capture transactions; unchanged frames still skip");
  firmware_overlay_active=1;memcpy(firmware_keyboard_background,shadow+640*320*4,sizeof(firmware_keyboard_background));show_firmware_keyboard();
  overlay_restore_capture_cycles=0;compositor_calls=blit_calls=0;
  grub_video_fb_fill_rect(0xff456789,0,0,640,480);assert(grub_video_gop_swap_buffers()==0);
  assert(compositor_calls==1&&blit_calls==1&&overlay_restore_capture_cycles==1);
  assert(!memcmp(screen,shadow,640*320*4));
  assert(!memcmp(firmware_keyboard_background,shadow+640*320*4,sizeof(firmware_keyboard_background)));
  firmware_overlay_active=0;memcpy(screen+640*320*4,firmware_keyboard_background,sizeof(firmware_keyboard_background));
  assert(!memcmp(screen,shadow,sizeof(screen)));
  puts("PASS active original firmware keyboard stays on top and its captured background follows the whole GRUB frame, restoring exactly after keyboard dismissal");
  grub_video_fb_track_damage(NULL);grub_free(display);grub_free(other);free(shadow);free(otherbuf);return 0;
}
