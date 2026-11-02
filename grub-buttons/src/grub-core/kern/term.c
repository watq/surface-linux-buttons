/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2003,2005,2007,2008,2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/term.h>
#include <grub/err.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/env.h>
#include <grub/time.h>

struct grub_term_output *grub_term_outputs_disabled;
struct grub_term_input *grub_term_inputs_disabled;
struct grub_term_output *grub_term_outputs;
struct grub_term_input *grub_term_inputs;

/* Current color state.  */
grub_uint8_t grub_term_normal_color = GRUB_TERM_DEFAULT_NORMAL_COLOR;
grub_uint8_t grub_term_highlight_color = GRUB_TERM_DEFAULT_HIGHLIGHT_COLOR;

void (*grub_term_poll_usb) (int wait_for_completion) = NULL;
void (*grub_net_poll_cards_idle) (void) = NULL;
void (*grub_term_poll_input_hook) (void) = NULL;
grub_term_input_trace_t grub_term_input_trace_hook = NULL;

static void
grub_term_trace_completed (grub_term_input_trace_t trace, const char *phase,
                           const char *name, grub_uint64_t begin, int key)
{
  static int busy;
  grub_uint64_t end;
  grub_err_t saved;
  char message[GRUB_MAX_ERRMSG];
  if (!trace || trace != grub_term_input_trace_hook || busy)
    return;
  end = grub_get_time_ms ();
  if (end < begin || end - begin <= 20)
    return;
  /* The measured operation has already returned. Isolate diagnostic I/O
     and preserve the caller's error, including during firmware callbacks. */
  busy = 1;
  saved = grub_errno;
  grub_memcpy (message, grub_errmsg, sizeof (message));
  trace (phase, name, begin, end, key);
  grub_errno = saved;
  grub_memcpy (grub_errmsg, message, sizeof (message));
  busy = 0;
}

void
grub_term_poll_input (void)
{
  static int busy;
  grub_err_t saved;
  char message[GRUB_MAX_ERRMSG];
  if (!grub_term_poll_input_hook || busy)
    return;
  busy = 1;
  saved = grub_errno;
  grub_memcpy (message, grub_errmsg, sizeof (message));
  grub_errno = GRUB_ERR_NONE;
  grub_term_poll_input_hook ();
  grub_errno = saved;
  grub_memcpy (grub_errmsg, message, sizeof (message));
  busy = 0;
}

/* Put a Unicode character.  */
static void
grub_putcode_dumb (grub_uint32_t code,
		   struct grub_term_output *term)
{
  struct grub_unicode_glyph c =
    {
      .base = code,
      .variant = 0,
      .attributes = 0,
      .ncomb = 0,
      .estimated_width = 1
    };

  if (code == '\t' && term->getxy)
    {
      int n;

      n = GRUB_TERM_TAB_WIDTH - ((term->getxy (term).x)
				 % GRUB_TERM_TAB_WIDTH);
      while (n--)
	grub_putcode_dumb (' ', term);

      return;
    }

  (term->putchar) (term, &c);
  if (code == '\n')
    grub_putcode_dumb ('\r', term);
}

static void
grub_xputs_dumb (const char *str)
{
  for (; *str; str++)
    {
      grub_term_output_t term;
      grub_uint32_t code = *str;
      if (code > 0x7f)
	code = '?';

      FOR_ACTIVE_TERM_OUTPUTS(term)
	grub_putcode_dumb (code, term);
    }
}

void (*grub_xputs) (const char *str) = grub_xputs_dumb;

int
grub_getkey_noblock (void)
{
  grub_term_input_t term;
  grub_term_input_trace_t trace = grub_term_input_trace_hook;
  grub_uint64_t begin = trace ? grub_get_time_ms () : 0;

  grub_term_poll_input ();
  grub_term_trace_completed (trace, "sample", "cooperative", begin, GRUB_TERM_NO_KEY);

  if (grub_term_poll_usb)
    {
      begin = trace ? grub_get_time_ms () : 0;
      grub_term_poll_usb (0);
      grub_term_trace_completed (trace, "usb", "native_usb", begin, GRUB_TERM_NO_KEY);
    }

  if (grub_net_poll_cards_idle)
    {
      begin = trace ? grub_get_time_ms () : 0;
      grub_net_poll_cards_idle ();
      grub_term_trace_completed (trace, "net", "idle_cards", begin, GRUB_TERM_NO_KEY);
    }

  FOR_ACTIVE_TERM_INPUTS(term)
  {
    char name[48];
    if (trace)
      {
        const char *source = term->name ? term->name : "unnamed";
        unsigned i = 0;
        while (i + 1 < sizeof (name) && source[i])
          { name[i] = source[i]; i++; }
        name[i] = 0;
      }
    begin = trace ? grub_get_time_ms () : 0;
    int key = term->getkey (term);
    grub_term_trace_completed (trace, "terminal", trace ? name : "", begin, key);
    if (key != GRUB_TERM_NO_KEY)
      return key;
  }

  return GRUB_TERM_NO_KEY;
}

int
grub_getkey (void)
{
  int ret;

  grub_refresh ();

  while (1)
    {
      ret = grub_getkey_noblock ();
      if (ret != GRUB_TERM_NO_KEY)
	return ret;
      grub_cpu_idle ();
    }
}

int
grub_getkeystatus (void)
{
  int status = 0;
  grub_term_input_t term;

  if (grub_term_poll_usb)
    grub_term_poll_usb (0);

  FOR_ACTIVE_TERM_INPUTS(term)
  {
    if (term->getkeystatus)
      status |= term->getkeystatus (term);
  }

  return status;
}

int
grub_key_is_interrupt (int key)
{
  /*
   * ESC sometimes is the BIOS setup hotkey and may be hard to discover, also
   * check F4, which was chosen because is not used as a hotkey to enter the
   * BIOS setup by any vendor.
   */
  if (key == GRUB_TERM_ESC || key == GRUB_TERM_KEY_F4)
    return 1;

  /*
   * Pressing keys at the right time during boot is hard to time, also allow
   * interrupting sleeps / the menu countdown by keeping shift pressed.
   */
  if (grub_getkeystatus() & (GRUB_TERM_STATUS_LSHIFT | GRUB_TERM_STATUS_RSHIFT))
    return 1;

  return 0;
}

void
grub_refresh (void)
{
  struct grub_term_output *term;

  FOR_ACTIVE_TERM_OUTPUTS(term)
    grub_term_refresh (term);
}
