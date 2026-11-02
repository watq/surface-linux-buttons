/* Dispatch state for the real GRUB input terminal. No firmware access here. */
#ifndef SP8_GRUB_GPIO_ACTIONS_H
#define SP8_GRUB_GPIO_ACTIONS_H
typedef struct {
  GP_DEBOUNCE debounce[3];
  uint32_t power_press_tick;
  unsigned power_pending;
  unsigned pressed_count[3], released_count[3], delivered_count[3];
  int keys[8];
  unsigned button[8], head, count;
  uint32_t return_chord_since;
  unsigned return_chord_active, return_chord_sent;
} M1_ACTIONS;

static int
m1_actions_push (M1_ACTIONS *a, unsigned button, int key)
{
  unsigned index;
  if (a->count == 8) return 0;
  index = (a->head + a->count) % 8;
  a->keys[index] = key; a->button[index] = button; a->count++;
  return 1;
}

static int
m1_actions_pop (M1_ACTIONS *a, unsigned *button)
{
  int key;
  if (!a->count) return GRUB_TERM_NO_KEY;
  key = a->keys[a->head]; *button = a->button[a->head];
  a->head = (a->head + 1) % 8; a->count--;
  a->delivered_count[*button]++;
  return key;
}

static int
m1_actions_update (M1_ACTIONS *a, uint32_t tick, const unsigned pressed[3], int event[3])
{
  unsigned i, held = pressed[0] + pressed[1] + pressed[2];
  for (i = 0; i < 3; i++) {
    event[i] = gp_debounce (&a->debounce[i], tick, pressed[i]);
    if (event[i] > 0) a->pressed_count[i]++;
    if (event[i] < 0) a->released_count[i]++;
  }
  if (held > 1) a->power_pending = 0;
  /* Preserve single-key navigation/short power confirmation. Both volume
   * buttons held for 500ms provide Escape without adding a menu entry. */
  if (!pressed[0] && pressed[1] && pressed[2]) {
    if (!a->return_chord_active) {
      a->return_chord_since = tick; a->return_chord_active = 1;
    }
    if (!a->return_chord_sent && a->debounce[1].armed && a->debounce[2].armed
        && a->debounce[1].stable && a->debounce[2].stable
        && (uint32_t) (tick - a->return_chord_since) >= 50) {
      if (!m1_actions_push (a, 1, GRUB_TERM_ESC)) return 0;
      a->return_chord_sent = 1;
    }
  } else {
    a->return_chord_active = 0;
    if (!held) a->return_chord_sent = 0;
  }
  if (event[0] > 0 && held == 1) {
    a->power_pending = 1; a->power_press_tick = tick;
  }
  if (event[1] > 0 && held == 1 && !m1_actions_push (a, 1, GRUB_TERM_KEY_UP)) return 0;
  if (event[2] > 0 && held == 1 && !m1_actions_push (a, 2, GRUB_TERM_KEY_DOWN)) return 0;
  if (event[0] < 0) {
    if (a->power_pending && held == 0 && (uint32_t) (tick - a->power_press_tick) <= 120 &&
        !m1_actions_push (a, 0, '\r')) return 0;
    a->power_pending = 0;
  }
  return 1;
}
#endif
