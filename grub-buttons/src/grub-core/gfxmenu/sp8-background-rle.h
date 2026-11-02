/* Bounded expansion of RGBA runs. No filesystem or framebuffer access here. */
#ifndef SP8_BACKGROUND_RLE_H
#define SP8_BACKGROUND_RLE_H
static grub_uint32_t
d1_rle_u32 (const grub_uint8_t *p)
{
  return (grub_uint32_t) p[0] | (grub_uint32_t) p[1] << 8
         | (grub_uint32_t) p[2] << 16 | (grub_uint32_t) p[3] << 24;
}

static int
d1_expand_background_runs (const grub_uint8_t *bytes, grub_size_t length,
                           grub_uint32_t *data, grub_size_t pixels,
                           grub_size_t *done, void (*sample) (void))
{
  grub_size_t index, next_sample = *done + 16384;
  if (length % 8 || *done > pixels) return 0;
  for (index = 0; index < length; index += 8)
    {
      grub_uint32_t count = d1_rle_u32 (bytes + index);
      grub_uint32_t color = grub_cpu_to_le32 (d1_rle_u32 (bytes + index + 4));
      if (!count || count > pixels - *done) return 0;
      while (count)
        {
          grub_size_t chunk = grub_min ((grub_size_t) count, (grub_size_t) 16384);
          grub_size_t i;
          for (i = 0; i < chunk; i++) data[*done + i] = color;
          *done += chunk; count -= chunk;
          if (sample && *done >= next_sample)
            {
              sample ();
              next_sample = *done + 16384;
            }
        }
    }
  return 1;
}
#endif
