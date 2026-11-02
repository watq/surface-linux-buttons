/* Exercise the real cooperative poll and framebuffer dispatch in memory.
 * Pixel bytes, recursion/error isolation and non-consuming input are checked;
 * no host video, GPIO, EFI variables or physical boot are accessed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#define GRUB_UTIL 1
#define GRUB_SP8_POLL_TEST 1
#define GRUB_FILE "test-sp8-grub-daily-render-poll.c"
#include "../grub-buttons/src/grub-core/kern/term.c"
#include "../grub-buttons/src/grub-core/video/fb/fbblit.c"
#include "../grub-buttons/src/grub-core/video/fb/fbfill.c"

grub_err_t grub_errno;
char grub_errmsg[GRUB_MAX_ERRMSG];
void *grub_memmove(void *d,const void *s,grub_size_t n) { return memmove(d,s,n); }

static struct grub_video_mode_info *display_mode;
grub_video_color_t grub_video_fb_map_color(grub_uint32_t index) {
  (void)index;assert(!"palette conversion is outside this truecolor test");return 0;
}
grub_video_color_t grub_video_fb_map_rgb(grub_uint8_t r,grub_uint8_t g,grub_uint8_t b) {
  return (uint32_t)r<<display_mode->red_field_pos | (uint32_t)g<<display_mode->green_field_pos |
         (uint32_t)b<<display_mode->blue_field_pos | (uint32_t)255<<display_mode->reserved_field_pos;
}
grub_video_color_t grub_video_fb_map_rgba(grub_uint8_t r,grub_uint8_t g,grub_uint8_t b,grub_uint8_t a) {
  return (uint32_t)r<<display_mode->red_field_pos | (uint32_t)g<<display_mode->green_field_pos |
         (uint32_t)b<<display_mode->blue_field_pos | (uint32_t)a<<display_mode->reserved_field_pos;
}
void grub_video_fb_unmap_color_int(struct grub_video_fbblit_info *s,grub_video_color_t c,
                                 grub_uint8_t *r,grub_uint8_t *g,grub_uint8_t *b,grub_uint8_t *a) {
  *r=c>>s->mode_info->red_field_pos;*g=c>>s->mode_info->green_field_pos;
  *b=c>>s->mode_info->blue_field_pos;*a=c>>s->mode_info->reserved_field_pos;
}
grub_video_color_t get_pixel(struct grub_video_fbblit_info *s,unsigned x,unsigned y) {
  grub_video_color_t c=0;memcpy(&c,s->data+y*s->mode_info->pitch+x*s->mode_info->bytes_per_pixel,
                              s->mode_info->bytes_per_pixel);return c;
}
void set_pixel(struct grub_video_fbblit_info *s,unsigned x,unsigned y,grub_video_color_t c) {
  memcpy(s->data+y*s->mode_info->pitch+x*s->mode_info->bytes_per_pixel,&c,s->mode_info->bytes_per_pixel);
}

static unsigned polls,queued,consumed;
static void sample(void) {
  polls++;
  if(polls==3||polls==6||polls==9)queued++;
  grub_errno=GRUB_ERR_BAD_DEVICE;strcpy(grub_errmsg,"injected sampler error");
  grub_term_poll_input(); /* The actual core recursion guard must stop this. */
}
int main(void) {
  enum {W=64,H=160,N=W*H*4};
  unsigned char *src=malloc(N),*dst=malloc(N),*expected=malloc(N);
  struct grub_video_mode_info rgba={.width=W,.height=H,.pitch=W*4,.bpp=32,.bytes_per_pixel=4,
      .red_field_pos=0,.green_field_pos=8,.blue_field_pos=16,.reserved_field_pos=24,
      .blit_format=GRUB_VIDEO_BLIT_FORMAT_RGBA_8888};
  struct grub_video_mode_info bgra=rgba;
  struct grub_video_fbblit_info from={&rgba,src},to={&bgra,dst};
  unsigned i;
  assert(src&&dst&&expected);bgra.blit_format=GRUB_VIDEO_BLIT_FORMAT_BGRA_8888;
  bgra.red_field_pos=16;bgra.blue_field_pos=0;display_mode=&bgra;
  for(i=0;i<N;i++)src[i]=(i*37u+i/11u)&255;
  memset(dst,0,N);grub_term_poll_input_hook=NULL;
  grub_video_fb_dispatch_blit(&to,&from,GRUB_VIDEO_BLIT_REPLACE,0,0,W,H,0,0);
  memcpy(expected,dst,N);memset(dst,0,N);
  grub_errno=GRUB_ERR_BAD_SIGNATURE;strcpy(grub_errmsg,"existing loader error");
  grub_term_poll_input_hook=sample;
  grub_video_fb_dispatch_blit(&to,&from,GRUB_VIDEO_BLIT_REPLACE,0,0,W,H,0,0);
  assert(!memcmp(dst,expected,N));assert(polls==11&&queued==3&&!consumed);
  assert(grub_errno==GRUB_ERR_BAD_SIGNATURE&&!strcmp(grub_errmsg,"existing loader error"));
  puts("PASS real blit samples inside rendering without consuming input; recursive callback is blocked and prior error/message preserved");
  /* Identical pixels through cached BGRA copies and the original RGBA swap. */
  memcpy(src,expected,N);from.mode_info=&bgra;polls=queued=0;memset(dst,0,N);
  grub_video_fb_dispatch_blit(&to,&from,GRUB_VIDEO_BLIT_REPLACE,0,0,W,H,0,0);
  assert(!memcmp(dst,expected,N)&&polls==11);
  puts("PASS cached display-order background and original per-pixel conversion produce identical framebuffer bytes");
  polls=queued=0;grub_video_fb_fill_dispatch(&to,0xff876543,0,0,W,H);
  assert(polls==11);for(i=0;i<N;i+=4)assert(dst[i]==0x43&&dst[i+1]==0x65&&dst[i+2]==0x87&&dst[i+3]==0xff);
  grub_term_poll_input_hook=NULL;polls=0;grub_term_poll_input();assert(!polls);
  puts("PASS fill also cooperatively samples with unchanged pixels; absent hook does nothing");
  free(src);free(dst);free(expected);return 0;
}
