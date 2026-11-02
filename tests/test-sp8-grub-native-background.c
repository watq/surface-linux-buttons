/* Compare offline artwork to the actual GRUB scaler; exercise RLE bounds.
 * Works only on copied image bytes and heap buffers, never a host framebuffer. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#define GRUB_UTIL 1
#define GRUB_FILE "test-sp8-grub-native-background.c"
#include "../grub-buttons/src/grub-core/video/bitmap_scale.c"
#include "sp8-background-rle.h"

grub_err_t grub_errno;
char grub_errmsg[GRUB_MAX_ERRMSG];
void *grub_memmove(void *d,const void *s,grub_size_t n) { return memmove(d,s,n); }
grub_err_t grub_error(grub_err_t code,const char *format,...) {
  (void)format;grub_errno=code;return code;
}
grub_err_t grub_video_bitmap_create(struct grub_video_bitmap **dst,unsigned w,unsigned h,
                                   enum grub_video_blit_format format) {
  struct grub_video_bitmap *b=calloc(1,sizeof(*b));assert(b);
  assert(format==GRUB_VIDEO_BLIT_FORMAT_RGBA_8888);
  b->mode_info.width=w;b->mode_info.height=h;b->mode_info.pitch=w*4;
  b->mode_info.bpp=32;b->mode_info.bytes_per_pixel=4;b->mode_info.blit_format=format;
  b->mode_info.red_field_pos=0;b->mode_info.green_field_pos=8;b->mode_info.blue_field_pos=16;
  b->mode_info.reserved_field_pos=24;b->data=calloc(w*h,4);assert(b->data);*dst=b;return 0;
}
grub_err_t grub_video_bitmap_destroy(struct grub_video_bitmap *b) {
  if(b){free(b->data);free(b);}return 0;
}
static unsigned samples;
static void sample(void){samples++;}
static unsigned char *read_all(const char *path,size_t expected) {
  FILE *f=fopen(path,"rb");unsigned char *data=malloc(expected);assert(f&&data);
  assert(fread(data,1,expected,f)==expected&&fgetc(f)==EOF);fclose(f);return data;
}
static void put32(unsigned char *d,uint32_t n) {d[0]=n;d[1]=n>>8;d[2]=n>>16;d[3]=n>>24;}
int main(int argc,char **argv) {
  struct grub_video_bitmap *src,*scaled;
  unsigned char *original,*reference,*encoded;
  grub_size_t done=0;uint32_t pixels[16];unsigned char bad[16];
  size_t length;FILE *f;
  assert(argc==4);
  original=read_all(argv[1],16+1920*1080*4);reference=read_all(argv[2],2880*1920*4);
  assert(grub_video_bitmap_create(&src,1920,1080,GRUB_VIDEO_BLIT_FORMAT_RGBA_8888)==0);
  memcpy(src->data,original+16,1920*1080*4);
  assert(grub_video_bitmap_create_scaled(&scaled,2880,1920,src,GRUB_VIDEO_BITMAP_SCALE_METHOD_BEST)==0);
  assert(!memcmp(scaled->data,reference,2880*1920*4));
  puts("PASS prescaled native background is byte-for-byte identical to actual GRUB BEST/bilinear scaler at 2880x1920");
  f=fopen(argv[3],"rb");assert(f);fseek(f,0,SEEK_END);length=ftell(f);fclose(f);assert(length>=24);
  encoded=read_all(argv[3],length);assert(!memcmp(encoded,"SP8RLE01",8));
  memset(scaled->data,0,2880*1920*4);done=0;samples=0;
  for(size_t i=24;i<length;i+=32768){
    size_t count=length-i<32768?length-i:32768;
    assert(d1_expand_background_runs(encoded+i,count,scaled->data,2880*1920,&done,sample));
  }
  assert(done==2880*1920&&!memcmp(scaled->data,reference,2880*1920*4));assert(samples>0&&samples<400);
  puts("PASS encoded signed background expands to exactly the same pixels with bounded cooperative sampling");
  memset(bad,0,sizeof(bad));done=0;assert(!d1_expand_background_runs(bad,8,pixels,16,&done,sample));
  put32(bad,17);assert(!d1_expand_background_runs(bad,8,pixels,16,&done,sample)&&done==0);
  put32(bad,1);assert(!d1_expand_background_runs(bad,7,pixels,16,&done,sample));
  done=17;assert(!d1_expand_background_runs(bad,8,pixels,16,&done,sample));
  done=0;put32(bad,8);put32(bad+4,0xff123456);put32(bad+8,8);put32(bad+12,0xffabcdef);
  assert(d1_expand_background_runs(bad,16,pixels,16,&done,sample)&&done==16);
  for(unsigned i=0;i<8;i++)assert(pixels[i]==grub_cpu_to_le32(0xff123456));
  for(unsigned i=8;i<16;i++)assert(pixels[i]==grub_cpu_to_le32(0xffabcdef));
  assert(!d1_expand_background_runs(bad,8,pixels,16,&done,sample));
  puts("PASS zero/overflow/incomplete runs and already-full output are refused without writing outside the bounds");
  grub_video_bitmap_destroy(src);grub_video_bitmap_destroy(scaled);
  free(original);free(reference);free(encoded);return 0;
}
