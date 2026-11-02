/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2006,2007,2008  Free Software Foundation, Inc.
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
#include <grub/misc.h>
#include <grub/types.h>
#include <grub/err.h>
#include <grub/efi/efi.h>
#include <grub/efi/api.h>
#include <grub/efi/console.h>
#include <grub/efi/graphics_output.h>
#include <grub/efi/sp8_swm.h>
#include <grub/env.h>
#include <grub/time.h>
#include <grub/command.h>

typedef enum {
    GRUB_TEXT_MODE_UNDEFINED = -1,
    GRUB_TEXT_MODE_UNAVAILABLE = 0,
    GRUB_TEXT_MODE_AVAILABLE
}
grub_text_mode;

typedef enum {
    GRUB_CURSOR_MODE_UNDEFINED = -1,
    GRUB_CURSOR_MODE_OFF = 0,
    GRUB_CURSUR_MODE_ON
}
grub_cursor_mode;

static grub_text_mode text_mode = GRUB_TEXT_MODE_UNDEFINED;
static grub_cursor_mode cursor_mode = GRUB_CURSOR_MODE_UNDEFINED;
static grub_term_color_state text_colorstate = GRUB_TERM_COLOR_UNDEFINED;

static unsigned d1_console_reports[3];
static grub_uint64_t d1_console_longest_ms[3];
static grub_uint64_t d1_console_calls[3], d1_console_leaf_calls, d1_console_idle_polls;
static grub_uint64_t d1_console_navigation_keys;
static grub_uint64_t d1_console_queries, d1_console_query_ms, d1_console_query_longest;
static unsigned d1_console_query_reports;
static grub_uint64_t d1_console_service_calls, d1_console_service_ms, d1_console_service_longest;
static unsigned d1_console_service_reports;
static grub_efi_handle_t d1_console_cached_input_handle;
static unsigned d1_console_cached_leaf_input;
static int d1_console_direct_poll;
static grub_efi_key_data_t d1_console_key_data;
struct d1_console_leaf_state
{
  struct grub_efi_console_leaf input;
  grub_efi_key_data_t key;
  unsigned pending, reports;
  grub_uint64_t longest_ms;
  grub_uint64_t last_poll_ms;
  unsigned has_polled;
  unsigned cycle_reports;
  unsigned slow_cycle_mask;
  grub_uint64_t longest_cycle_ms;
};
static struct d1_console_leaf_state d1_console_leaves[GRUB_EFI_CONSOLE_MAX_LEAVES];
static unsigned d1_console_leaf_count, d1_console_next_leaf;
static unsigned d1_console_leaf_generation;
static int d1_console_empty_input_idle;
static struct grub_efi_swm *d1_swm;
static struct grub_efi_swm d1_swm_calls;
static unsigned d1_swm_registered, d1_swm_width, d1_swm_height, d1_swm_attempted;
static grub_efi_event_t d1_swm_paint;
static unsigned d1_swm_blits;

void
grub_efi_console_release_swm (void)
{
  if (d1_swm_registered && !grub_efi_is_finished)
    {
      char detail[160]; grub_uint64_t begin = grub_get_time_ms ();
      grub_efi_status_t status = d1_swm_calls.unregister_client (d1_swm, grub_efi_image_handle);
      if (status == GRUB_EFI_SUCCESS && d1_swm_paint
          && grub_efi_system_table->boot_services->close_event)
        grub_efi_system_table->boot_services->close_event (d1_swm_paint);
      grub_snprintf (detail, sizeof (detail), "SWM_CLIENT_RELEASE own_GRUB_client_only=1 status=0x%llx elapsed_ms=%llu blits=%u",
                     (unsigned long long) status, (unsigned long long) (grub_get_time_ms () - begin), d1_swm_blits);
      grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
    }
  d1_swm_registered = d1_swm_attempted = 0; d1_swm = NULL; d1_swm_paint = NULL;
}

void
grub_efi_console_configure_swm (struct grub_efi_swm *protocol, const struct grub_efi_swm *copy)
{
  grub_efi_console_release_swm ();
  if (!protocol || !copy) return;
  d1_swm = protocol; d1_swm_calls = *copy; d1_swm_blits = 0;
}

int
grub_efi_console_swm_surface (unsigned width, unsigned height)
{
  struct grub_efi_swm_rect rect;
  grub_efi_status_t status; grub_uint64_t begin; void *unused_pointer = NULL; char detail[192];
  if (!d1_swm || grub_efi_is_finished || !grub_env_get ("sp8_daily_active") || !width || !height) return 0;
  if (d1_swm_registered && width == d1_swm_width && height == d1_swm_height) return 1;
  if (!d1_swm_registered && d1_swm_attempted) return 0;
  rect = (struct grub_efi_swm_rect) {0, 0, width - 1, height - 1}; begin = grub_get_time_ms ();
  if (d1_swm_registered)
    status = d1_swm_calls.set_window_frame (d1_swm, grub_efi_image_handle, &rect);
  else
    {
      d1_swm_attempted = 1;
      status = d1_swm_calls.register_client (d1_swm, grub_efi_image_handle, 0x10, &rect,
                                             NULL, NULL, &unused_pointer, &d1_swm_paint);
    }
  if (status == GRUB_EFI_SUCCESS)
    { d1_swm_registered = 1; d1_swm_width = width; d1_swm_height = height; }
  grub_snprintf (detail, sizeof (detail), "SWM_CLIENT_SURFACE width=%u height=%u active=0 z_order=16 OSK_above=1 status=0x%llx elapsed_ms=%llu",
                 width, height, (unsigned long long) status, (unsigned long long) (grub_get_time_ms () - begin));
  grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
  return status == GRUB_EFI_SUCCESS;
}

grub_efi_status_t
grub_efi_console_swm_blt (struct grub_efi_gop_blt_pixel *buffer, unsigned x, unsigned y,
                         unsigned width, unsigned height, unsigned pitch)
{
  if (!d1_swm_registered || !d1_swm || grub_efi_is_finished) return GRUB_EFI_UNSUPPORTED;
  d1_swm_blits++;
  return d1_swm_calls.blt_window (d1_swm, grub_efi_image_handle, buffer, GRUB_EFI_BLT_BUFFER_TO_VIDEO,
                                 x, y, x, y, width, height, pitch);
}

void
grub_efi_console_set_direct_poll (int enabled)
{
  if (enabled && !d1_console_direct_poll)
    {
      grub_memset (d1_console_calls, 0, sizeof (d1_console_calls));
      d1_console_leaf_calls = d1_console_idle_polls = 0;
      d1_console_navigation_keys = 0;
      d1_console_queries = d1_console_query_ms = d1_console_query_longest = 0;
      d1_console_query_reports = 0;
      d1_console_service_calls = d1_console_service_ms = d1_console_service_longest = 0;
      d1_console_service_reports = 0;
    }
  d1_console_direct_poll = !!enabled;
}

void
grub_efi_console_log_input_summary (void)
{
  char detail[256];
  grub_snprintf (detail, sizeof (detail),
                 "EFI_INPUT_SUMMARY CheckEvent_calls=%llu aggregate_Basic_reads=%llu aggregate_Ex_reads=%llu leaf_reads=%llu empty_polls=%llu EFI_navigation_keys=%llu",
                 (unsigned long long) d1_console_calls[0],
                 (unsigned long long) d1_console_calls[1],
                 (unsigned long long) d1_console_calls[2],
                 (unsigned long long) d1_console_leaf_calls,
                 (unsigned long long) d1_console_idle_polls,
                 (unsigned long long) d1_console_navigation_keys);
  grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
  grub_snprintf (detail, sizeof (detail),
                 "EFI_INPUT_QUERY_SUMMARY live_queries=%llu query_total_ms=%llu query_max_ms=%llu agent_tracking=0 stale_protocol_cache=0 poll_interval_ms=5",
                 (unsigned long long) d1_console_queries,
                 (unsigned long long) d1_console_query_ms,
                 (unsigned long long) d1_console_query_longest);
  grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
  grub_snprintf (detail, sizeof (detail),
                 "EFI_INPUT_SERVICE_SUMMARY calls=%llu total_ms=%llu max_ms=%llu callbacks_preserved=1 total_wait_not_excluded=1",
                 (unsigned long long) d1_console_service_calls,
                 (unsigned long long) d1_console_service_ms,
                 (unsigned long long) d1_console_service_longest);
  grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
}

/* Let the original firmware dispatch pending input/OSK/SWM callbacks at a
   measured boundary. Raise and immediately restore: no read/query/blit is
   performed at raised TPL, no timer is cancelled, and no pointer queue is
   consumed here. This does not eliminate a slow firmware callback; its
   duration stays in the same complete console-cycle time and GPIO gap. */
static void
d1_console_service_callbacks (void)
{
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_tpl_t previous;
  grub_uint64_t begin, elapsed;
  if (grub_efi_is_finished || !bs->raise_tpl || !bs->restore_tpl) return;
  begin = grub_get_time_ms ();
  previous = bs->raise_tpl (GRUB_EFI_TPL_NOTIFY);
  bs->restore_tpl (previous);
  elapsed = grub_get_time_ms () - begin;
  d1_console_service_calls++; d1_console_service_ms += elapsed;
  if (d1_console_service_reports < 2 || elapsed > d1_console_service_longest + 20)
    {
      char detail[224];
      grub_snprintf (detail, sizeof (detail),
                     "EFI_INPUT_SERVICE operation=RestoreTPL previous_tpl=%llu elapsed_ms=%llu callbacks_preserved=1 total_wait_not_excluded=1",
                     (unsigned long long) previous, (unsigned long long) elapsed);
      grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
      d1_console_service_reports++;
    }
  d1_console_service_longest = grub_max (d1_console_service_longest, elapsed);
}

/* GET_PROTOCOL with a NULL agent is a live query of the same protocol
   database. EDK2 does not allocate/increment an agent OpenList item for it.
   GET_PROTOCOL with GRUB's image handle is not a lifetime pin: uninstall
   is allowed to discard that OpenList item. Keep the live check on every
   scheduled read, including USB hotplug, rather than cache a stale pointer.
   Stay at the caller's TPL so firmware OSK/SWM callbacks remain serviced.
   A long query can include firmware callback work; total timing is retained
   and is not claimed to be the protocol lookup's intrinsic cost. */
void *
grub_efi_console_query_input_protocol (grub_efi_handle_t handle, int extended)
{
  static grub_guid_t ex_guid = GRUB_EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
  static grub_guid_t basic_guid = GRUB_EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
  grub_efi_boot_services_t *bs = grub_efi_system_table->boot_services;
  grub_efi_status_t status;
  grub_uint64_t begin, elapsed;
  void *protocol = NULL;
  if (grub_efi_is_finished || !handle || !bs->open_protocol) return NULL;
  begin = grub_get_time_ms ();
  status = bs->open_protocol (handle, extended ? &ex_guid : &basic_guid,
                              &protocol, NULL, NULL,
                              GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  elapsed = grub_get_time_ms () - begin;
  if (d1_console_direct_poll && grub_env_get ("sp8_daily_active"))
    {
      d1_console_queries++; d1_console_query_ms += elapsed;
      if (d1_console_query_reports < 2 || elapsed > d1_console_query_longest + 20)
        {
          char detail[256];
          grub_snprintf (detail, sizeof (detail),
                         "EFI_INPUT_QUERY handle=0x%llx kind=%s elapsed_ms=%llu status=0x%llx agent_tracking=0 callbacks_included=1",
                         (unsigned long long) (grub_addr_t) handle, extended ? "Ex" : "Basic",
                         (unsigned long long) elapsed, (unsigned long long) status);
          grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
          d1_console_query_reports++;
        }
      d1_console_query_longest = grub_max (d1_console_query_longest, elapsed);
    }
  return status == GRUB_EFI_SUCCESS ? protocol : NULL;
}

void
grub_efi_console_set_empty_input_policy (int enabled)
{
  d1_console_empty_input_idle = !!enabled;
}

void
grub_efi_console_set_leaf_inputs (unsigned count,
                                const struct grub_efi_console_leaf *inputs)
{
  struct d1_console_leaf_state saved[GRUB_EFI_CONSOLE_MAX_LEAVES];
  unsigned i, j;
  if (count > GRUB_EFI_CONSOLE_MAX_LEAVES || (count && !inputs)) return;
  grub_memcpy (saved, d1_console_leaves, sizeof (saved));
  grub_memset (d1_console_leaves, 0, sizeof (d1_console_leaves));
  for (i = 0; i < count; i++)
    {
      for (j = 0; j < d1_console_leaf_count; j++)
        if (inputs[i].handle == saved[j].input.handle
            && inputs[i].ex == saved[j].input.ex
            && inputs[i].basic == saved[j].input.basic)
          {
            d1_console_leaves[i] = saved[j];
            break;
          }
      d1_console_leaves[i].input = inputs[i];
    }
  /* A key already cached by getkeystatus is independent of the endpoint
     lifetime. Keep it until getkey consumes it, including during refresh. */
  d1_console_leaf_count = count;
  d1_console_leaf_generation++;
  if (d1_console_next_leaf >= count) d1_console_next_leaf = 0;
}

static int
d1_console_leaf_active (void)
{
  return d1_console_direct_poll && (d1_console_leaf_count || d1_console_empty_input_idle)
         && grub_env_get ("sp8_daily_active");
}

static void
d1_console_leaf_timing (unsigned index, grub_uint64_t begin,
                        grub_efi_status_t status)
{
  struct d1_console_leaf_state *leaf = &d1_console_leaves[index];
  grub_uint64_t elapsed = grub_get_time_ms () - begin;
  if (leaf->reports < 2 || elapsed > leaf->longest_ms + 20)
    {
      char detail[256];
      grub_snprintf (detail, sizeof (detail),
                     "EFI_INPUT operation=LeafReadKeyStroke%s source=%u handle=0x%llx elapsed_ms=%llu status=0x%llx",
                     leaf->input.ex ? "Ex" : "", index,
                     (unsigned long long) (grub_addr_t) leaf->input.handle,
                     (unsigned long long) elapsed, (unsigned long long) status);
      grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
      leaf->reports++;
      leaf->longest_ms = grub_max (leaf->longest_ms, elapsed);
    }
}

/* Bypass ConSplitter's lazy-connect and aggregate wait/read callbacks. Keep
   one key per device until consumed, and merge modifiers across Ex inputs.
   A non-owning live query rechecks each handle without a persistent agent
   OpenList entry, connection, reset, or unsafe protocol pointer cache. */
static grub_efi_status_t
d1_console_read_leaf_key (grub_efi_key_data_t *result)
{
  grub_efi_key_state_t modifiers;
  unsigned i, serviced = 0, generation = d1_console_leaf_generation;
  grub_memset (&modifiers, 0, sizeof (modifiers));
  if (!d1_console_leaf_count) d1_console_idle_polls++;
  for (i = 0; i < d1_console_leaf_count; i++)
    {
      struct d1_console_leaf_state *leaf = &d1_console_leaves[i];
      if (!leaf->pending)
        {
          void *current;
          grub_efi_status_t status;
          grub_efi_key_data_t key;
          grub_uint64_t begin = grub_get_time_ms ();
          grub_uint64_t cycle_begin = begin, query_ms, read_ms, sample_ms, service_ms;
          /* Firmware queues are independent of GPIO's 10ms sample cadence.
             Poll them at most every5ms rather than millions of times per
             second; pending keys still deliver immediately and exactly once. */
          if (leaf->has_polled && begin >= leaf->last_poll_ms && begin - leaf->last_poll_ms < 5)
            goto merge_modifiers;
          leaf->has_polled = 1; leaf->last_poll_ms = begin;
          service_ms = 0;
          if (!serviced)
            {
              serviced = 1;
              d1_console_service_callbacks ();
              service_ms = grub_get_time_ms () - begin;
              if (service_ms > 20) grub_term_poll_input ();
              /* A callback or sampling hook can withdraw/rebuild inputs.
                 Never use the previous array slot after that refresh. */
              if (generation != d1_console_leaf_generation)
                break;
            }
          begin = grub_get_time_ms ();
          current = grub_efi_console_query_input_protocol (leaf->input.handle, !!leaf->input.ex);
          query_ms = grub_get_time_ms () - begin;
          if (grub_get_time_ms () - begin > 20)
            {
              char detail[192];
              grub_snprintf (detail, sizeof (detail),
                             "EFI_INPUT operation=LeafGetProtocol source=%u elapsed_ms=%llu status=%s",
                             i, (unsigned long long) (grub_get_time_ms () - begin),
                             current ? "available" : "unavailable");
              grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
            }
          if (current != (void *) (leaf->input.ex ? (void *) leaf->input.ex
                                                 : (void *) leaf->input.basic))
            {
              grub_memset (&leaf->key.key_state, 0, sizeof (leaf->key.key_state));
              continue;
            }
          grub_memset (&key, 0, sizeof (key));
          begin = grub_get_time_ms ();
          if (leaf->input.ex)
            status = leaf->input.ex->read_key_stroke (leaf->input.ex, &key);
          else
            status = leaf->input.basic->read_key_stroke (leaf->input.basic, &key.key);
          read_ms = grub_get_time_ms () - begin;
          d1_console_leaf_calls++;
          d1_console_leaf_timing (i, begin, status);
          if (status == GRUB_EFI_SUCCESS || status == GRUB_EFI_NOT_READY)
            {
              leaf->key.key_state = key.key_state;
              if (status == GRUB_EFI_SUCCESS
                  && (key.key.scan_code || key.key.unicode_char))
                {
                  leaf->key = key;
                  leaf->pending = 1;
                }
            }
          begin = grub_get_time_ms ();
          grub_term_poll_input ();
          sample_ms = grub_get_time_ms () - begin;
          if (generation != d1_console_leaf_generation)
            break;
          unsigned slow_mask = (service_ms > 20 ? 1u : 0u) | (query_ms > 20 ? 2u : 0u)
                               | (read_ms > 20 ? 4u : 0u) | (sample_ms > 20 ? 8u : 0u);
          if (leaf->cycle_reports < 2 || grub_get_time_ms () - cycle_begin > leaf->longest_cycle_ms + 20
              || (slow_mask && slow_mask != leaf->slow_cycle_mask))
            {
              char detail[320];
              grub_uint64_t total = grub_get_time_ms () - cycle_begin;
              grub_snprintf (detail, sizeof (detail),
                             "EFI_INPUT_CYCLE source=%u handle=0x%llx service_ms=%llu query_ms=%llu read_ms=%llu sample_hook_ms=%llu total_ms=%llu callbacks_included=1",
                             i, (unsigned long long) (grub_addr_t) leaf->input.handle,
                             (unsigned long long) service_ms,
                             (unsigned long long) query_ms, (unsigned long long) read_ms,
                             (unsigned long long) sample_ms, (unsigned long long) total);
              grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
              leaf->cycle_reports++;
              leaf->longest_cycle_ms = grub_max (leaf->longest_cycle_ms, total);
            }
          leaf->slow_cycle_mask = slow_mask;
        }
    merge_modifiers:
      if (leaf->key.key_state.key_shift_state & GRUB_EFI_SHIFT_STATE_VALID)
        modifiers.key_shift_state |= leaf->key.key_state.key_shift_state;
      if (leaf->key.key_state.key_toggle_state & GRUB_EFI_TOGGLE_STATE_VALID)
        modifiers.key_toggle_state |= leaf->key.key_state.key_toggle_state;
    }
  for (i = 0; i < d1_console_leaf_count; i++)
    {
      unsigned index = (d1_console_next_leaf + i) % d1_console_leaf_count;
      struct d1_console_leaf_state *leaf = &d1_console_leaves[index];
      if (leaf->pending)
        {
          *result = leaf->key;
          d1_console_cached_input_handle = leaf->input.handle;
          d1_console_cached_leaf_input = 1;
          result->key_state = modifiers;
          leaf->pending = 0;
          d1_console_next_leaf = (index + 1) % d1_console_leaf_count;
          return GRUB_EFI_SUCCESS;
        }
    }
  grub_memset (result, 0, sizeof (*result));
  result->key_state = modifiers;
  return GRUB_EFI_NOT_READY;
}

static void
d1_console_timing (const char *operation, grub_uint64_t begin, grub_efi_status_t status)
{
  grub_uint64_t elapsed = grub_get_time_ms () - begin;
  unsigned index = !grub_strcmp (operation, "CheckEvent") ? 0
                   : !grub_strcmp (operation, "ReadKeyStroke") ? 1 : 2;
  d1_console_calls[index]++;
  if (d1_console_reports[index] < 2 || elapsed > d1_console_longest_ms[index] + 20)
    {
      char detail[192];
      grub_snprintf (detail, sizeof (detail), "EFI_INPUT operation=%s elapsed_ms=%llu status=0x%llx",
                     operation, (unsigned long long) elapsed, (unsigned long long) status);
      grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
      d1_console_reports[index]++;
      d1_console_longest_ms[index] = grub_max (d1_console_longest_ms[index], elapsed);
    }
}

/* Both UEFI input protocols support direct polling (EFI_NOT_READY on an
   empty queue). This Surface's aggregate CheckEvent and first aggregate
   read have both blocked earlier revisions. With the OSK protocol verified,
   skip the event gate and prefer validated live endpoints, keeping all OSK
   modes intact. Compatibility aggregate reads and their real latency remain
   available; an unverified OSK keeps the previous event gate. */
static int
d1_console_ready (grub_efi_event_t event)
{
  grub_efi_status_t status;
  grub_uint64_t begin;
  if (grub_env_get ("sp8_daily_active") && d1_console_direct_poll)
    return 1;
  if (!grub_env_get ("sp8_daily_active") || !event) return 1;
  begin = grub_get_time_ms ();
  status = grub_efi_system_table->boot_services->check_event (event);
  d1_console_timing ("CheckEvent", begin, status);
  return status == GRUB_EFI_SUCCESS;
}

static grub_uint32_t
map_char (grub_uint32_t c)
{
  /* Map some unicode characters to the EFI character.  */
  switch (c)
    {
    case GRUB_UNICODE_LEFTARROW:
      c = GRUB_UNICODE_BLACK_LEFT_TRIANGLE;
      break;
    case GRUB_UNICODE_UPARROW:
      c = GRUB_UNICODE_BLACK_UP_TRIANGLE;
      break;
    case GRUB_UNICODE_RIGHTARROW:
      c = GRUB_UNICODE_BLACK_RIGHT_TRIANGLE;
      break;
    case GRUB_UNICODE_DOWNARROW:
      c = GRUB_UNICODE_BLACK_DOWN_TRIANGLE;
      break;
    case GRUB_UNICODE_HLINE:
      c = GRUB_UNICODE_LIGHT_HLINE;
      break;
    case GRUB_UNICODE_VLINE:
      c = GRUB_UNICODE_LIGHT_VLINE;
      break;
    case GRUB_UNICODE_CORNER_UL:
      c = GRUB_UNICODE_LIGHT_CORNER_UL;
      break;
    case GRUB_UNICODE_CORNER_UR:
      c = GRUB_UNICODE_LIGHT_CORNER_UR;
      break;
    case GRUB_UNICODE_CORNER_LL:
      c = GRUB_UNICODE_LIGHT_CORNER_LL;
      break;
    case GRUB_UNICODE_CORNER_LR:
      c = GRUB_UNICODE_LIGHT_CORNER_LR;
      break;
    }

  return c;
}

static void
grub_console_setcolorstate (struct grub_term_output *term
			    __attribute__ ((unused)),
			    grub_term_color_state state)
{
  grub_efi_simple_text_output_interface_t *o;

  if (grub_efi_is_finished || text_mode != GRUB_TEXT_MODE_AVAILABLE)
    {
      /*
       * Cache colorstate changes before the first text-output, this avoids
       * "color_normal" environment writes causing a switch to textmode.
       */
      text_colorstate = state;
      return;
    }

  if (grub_efi_is_finished)
    return;

  o = grub_efi_system_table->con_out;

  switch (state) {
    case GRUB_TERM_COLOR_STANDARD:
      o->set_attributes (o, GRUB_TERM_DEFAULT_STANDARD_COLOR & 0x7f);
      break;
    case GRUB_TERM_COLOR_NORMAL:
      o->set_attributes (o, grub_term_normal_color & 0x7f);
      break;
    case GRUB_TERM_COLOR_HIGHLIGHT:
      o->set_attributes (o, grub_term_highlight_color & 0x7f);
      break;
    default:
      break;
  }
}

static void
grub_console_setcursor (struct grub_term_output *term __attribute__ ((unused)),
			int on)
{
  grub_efi_simple_text_output_interface_t *o;

  if (grub_efi_is_finished || text_mode != GRUB_TEXT_MODE_AVAILABLE)
    {
      /* Cache cursor changes before the first text-output */
      cursor_mode = on;
      return;
    }

  o = grub_efi_system_table->con_out;
  o->enable_cursor (o, on);
}

static grub_err_t
grub_prepare_for_text_output (struct grub_term_output *term)
{
  if (grub_efi_is_finished)
    return GRUB_ERR_BAD_DEVICE;

  if (text_mode != GRUB_TEXT_MODE_UNDEFINED)
    return text_mode ? GRUB_ERR_NONE : GRUB_ERR_BAD_DEVICE;

  if (! grub_efi_set_text_mode (1))
    {
      /* This really should never happen */
      grub_error (GRUB_ERR_BAD_DEVICE, "cannot set text mode");
      text_mode = GRUB_TEXT_MODE_UNAVAILABLE;
      return GRUB_ERR_BAD_DEVICE;
    }

  if (cursor_mode != GRUB_CURSOR_MODE_UNDEFINED)
    grub_console_setcursor (term, cursor_mode);
  if (text_colorstate != GRUB_TERM_COLOR_UNDEFINED)
    grub_console_setcolorstate (term, text_colorstate);
  text_mode = GRUB_TEXT_MODE_AVAILABLE;
  return GRUB_ERR_NONE;
}

static void
grub_console_putchar (struct grub_term_output *term,
		      const struct grub_unicode_glyph *c)
{
  grub_efi_char16_t str[2 + 30];
  grub_efi_simple_text_output_interface_t *o;
  unsigned i, j;

  if (grub_prepare_for_text_output (term) != GRUB_ERR_NONE)
    return;

  o = grub_efi_system_table->con_out;

  /* For now, do not try to use a surrogate pair.  */
  if (c->base > 0xffff)
    str[0] = '?';
  else
    str[0] = (grub_efi_char16_t)  map_char (c->base & 0xffff);
  j = 1;
  for (i = 0; i < c->ncomb && j + 1 < ARRAY_SIZE (str); i++)
    if (c->base < 0xffff)
      str[j++] = grub_unicode_get_comb (c)[i].code;
  str[j] = 0;

  /* Should this test be cached?  */
  if ((c->base > 0x7f || c->ncomb)
      && o->test_string (o, str) != GRUB_EFI_SUCCESS)
    return;

  o->output_string (o, str);
}

const unsigned efi_codes[] =
  {
    0, GRUB_TERM_KEY_UP, GRUB_TERM_KEY_DOWN, GRUB_TERM_KEY_RIGHT,
    GRUB_TERM_KEY_LEFT, GRUB_TERM_KEY_HOME, GRUB_TERM_KEY_END, GRUB_TERM_KEY_INSERT,
    GRUB_TERM_KEY_DC, GRUB_TERM_KEY_PPAGE, GRUB_TERM_KEY_NPAGE, GRUB_TERM_KEY_F1,
    GRUB_TERM_KEY_F2, GRUB_TERM_KEY_F3, GRUB_TERM_KEY_F4, GRUB_TERM_KEY_F5,
    GRUB_TERM_KEY_F6, GRUB_TERM_KEY_F7, GRUB_TERM_KEY_F8, GRUB_TERM_KEY_F9,
    GRUB_TERM_KEY_F10, GRUB_TERM_KEY_F11, GRUB_TERM_KEY_F12, GRUB_TERM_ESC
  };

static int
grub_efi_translate_key (grub_efi_input_key_t key)
{
  if (key.scan_code == 0)
    {
      /* Some firmware implementations use VT100-style codes against the spec.
	 This is especially likely if driven by serial.
       */
      if (key.unicode_char < 0x20 && key.unicode_char != 0
	  && key.unicode_char != '\t' && key.unicode_char != '\b'
	  && key.unicode_char != '\n' && key.unicode_char != '\r')
	return GRUB_TERM_CTRL | (key.unicode_char - 1 + 'a');
      else
	return key.unicode_char;
    }
  /* Some devices send enter with scan_code 0x0d (F3) and unicode_char 0x0d. */
  else if (key.scan_code == '\r' && key.unicode_char == '\r')
    return key.unicode_char;
  /* UEFI SimpleTextInputEx standard codes; SP8 delivery needs hardware test. */
  else if (key.scan_code == 0x0080) /* SCAN_VOLUME_UP */
    return GRUB_TERM_KEY_UP;
  else if (key.scan_code == 0x0081) /* SCAN_VOLUME_DOWN */
    return GRUB_TERM_KEY_DOWN;
  else if (key.scan_code == 0x0102) /* SCAN_SUSPEND (short power if exposed) */
    return '\r';
  else if (key.scan_code < ARRAY_SIZE (efi_codes))
    return efi_codes[key.scan_code];

  if ((key.unicode_char >= 0x20 && key.unicode_char <= 0x7f)
      || key.unicode_char == '\t' || key.unicode_char == '\b'
      || key.unicode_char == '\n' || key.unicode_char == '\r')
    return key.unicode_char;

  return GRUB_TERM_NO_KEY;
}

/* Log consumed navigation keys only. Printable command/password characters
   never enter this log; provenance distinguishes EFI/OSK from GPIO delivery. */
static void
d1_console_record_navigation (int key, grub_efi_handle_t handle, unsigned leaf)
{
  const char *name = key == GRUB_TERM_KEY_UP ? "UP" : key == GRUB_TERM_KEY_DOWN ? "DOWN"
                     : key == GRUB_TERM_KEY_LEFT ? "LEFT" : key == GRUB_TERM_KEY_RIGHT ? "RIGHT"
                     : key == GRUB_TERM_ESC ? "BACK" : key == '\r' || key == '\n' ? "ENTER" : NULL;
  char detail[160];
  if (!name || !grub_env_get ("sp8_daily_active")) return;
  d1_console_navigation_keys++;
  if (d1_console_navigation_keys > 12) return;
  grub_snprintf (detail, sizeof (detail), "EFI_NAV_DELIVER key=%s route=%s handle=0x%llx count=%llu",
                 name, leaf ? "leaf" : "aggregate", (unsigned long long) (grub_addr_t) handle,
                 (unsigned long long) d1_console_navigation_keys);
  grub_command_execute ("sp8_gpio", 2, (char *[]) {(char *) "log", detail});
}

static int
grub_console_getkey_con (struct grub_term_input *term __attribute__ ((unused)))
{
  grub_efi_simple_input_interface_t *i;
  grub_efi_input_key_t key;
  grub_efi_status_t status;

  i = grub_efi_system_table->con_in;
  if (!d1_console_ready (i->wait_for_key)) return GRUB_TERM_NO_KEY;
  grub_uint64_t begin = grub_get_time_ms ();
  status = i->read_key_stroke (i, &key);
  if (grub_env_get ("sp8_daily_active")) d1_console_timing ("ReadKeyStroke", begin, status);

  if (status != GRUB_EFI_SUCCESS)
    return GRUB_TERM_NO_KEY;

  int translated = grub_efi_translate_key (key);
  d1_console_record_navigation (translated, grub_efi_system_table->console_in_handler, 0);
  return translated;
}

/*
 * When more then just modifiers are pressed, our getkeystatus() consumes a
 * press from the queue, this function buffers the press for the regular
 * getkey() so that it does not get lost.
 */
static grub_err_t
grub_console_read_key_stroke (
                   grub_efi_simple_text_input_ex_interface_t *text_input,
                   grub_efi_key_data_t *key_data_ret, int *key_ret,
                   int consume)
{
  grub_efi_key_data_t *key_data = &d1_console_key_data;
  grub_efi_status_t status;
  int key;

  if (!text_input && !d1_console_leaf_active ())
    return GRUB_ERR_EOF;

  key = grub_efi_translate_key (key_data->key);
  if (key == GRUB_TERM_NO_KEY) {
    if (d1_console_leaf_active ())
      status = d1_console_read_leaf_key (key_data);
    else
      {
        grub_uint64_t begin;
        if (!d1_console_ready (text_input->wait_for_key)) return GRUB_ERR_EOF;
        begin = grub_get_time_ms ();
        status = text_input->read_key_stroke (text_input, key_data);
        d1_console_cached_input_handle = grub_efi_system_table->console_in_handler;
        d1_console_cached_leaf_input = 0;
        if (grub_env_get ("sp8_daily_active")) d1_console_timing ("ReadKeyStrokeEx", begin, status);
      }
    if (status != GRUB_EFI_SUCCESS)
      return GRUB_ERR_EOF;

    key = grub_efi_translate_key (key_data->key);
  }

  *key_data_ret = *key_data;
  *key_ret = key;

  if (consume) {
    key_data->key.scan_code = 0;
    key_data->key.unicode_char = 0;
  }

  return GRUB_ERR_NONE;
}

static int
grub_console_getkey_ex (struct grub_term_input *term)
{
  grub_efi_key_data_t key_data;
  grub_efi_uint32_t kss;
  grub_err_t err;
  int key = -1;

  err = grub_console_read_key_stroke (term->data, &key_data, &key, 1);
  if (err != GRUB_ERR_NONE || key == GRUB_TERM_NO_KEY)
    return GRUB_TERM_NO_KEY;

  kss = key_data.key_state.key_shift_state;
  if (kss & GRUB_EFI_SHIFT_STATE_VALID)
    {
      if ((kss & GRUB_EFI_LEFT_SHIFT_PRESSED
	   || kss & GRUB_EFI_RIGHT_SHIFT_PRESSED)
	  && (key & GRUB_TERM_EXTENDED))
	key |= GRUB_TERM_SHIFT;
      if (kss & GRUB_EFI_LEFT_ALT_PRESSED || kss & GRUB_EFI_RIGHT_ALT_PRESSED)
	key |= GRUB_TERM_ALT;
      if (kss & GRUB_EFI_LEFT_CONTROL_PRESSED
	  || kss & GRUB_EFI_RIGHT_CONTROL_PRESSED)
	key |= GRUB_TERM_CTRL;
    }

  d1_console_record_navigation (key, d1_console_cached_input_handle, d1_console_cached_leaf_input);
  return key;
}

static int
grub_console_getkeystatus (struct grub_term_input *term)
{
  grub_efi_key_data_t key_data;
  grub_efi_uint32_t kss;
  int key, mods = 0;

  if (grub_efi_is_finished)
    return 0;

  if (grub_console_read_key_stroke (term->data, &key_data, &key, 0))
    return 0;

  kss = key_data.key_state.key_shift_state;
  if (kss & GRUB_EFI_SHIFT_STATE_VALID)
    {
      if (kss & GRUB_EFI_LEFT_SHIFT_PRESSED)
        mods |= GRUB_TERM_STATUS_LSHIFT;
      if (kss & GRUB_EFI_RIGHT_SHIFT_PRESSED)
        mods |= GRUB_TERM_STATUS_RSHIFT;
      if (kss & GRUB_EFI_LEFT_ALT_PRESSED)
        mods |= GRUB_TERM_STATUS_LALT;
      if (kss & GRUB_EFI_RIGHT_ALT_PRESSED)
        mods |= GRUB_TERM_STATUS_RALT;
      if (kss & GRUB_EFI_LEFT_CONTROL_PRESSED)
        mods |= GRUB_TERM_STATUS_LCTRL;
      if (kss & GRUB_EFI_RIGHT_CONTROL_PRESSED)
        mods |= GRUB_TERM_STATUS_RCTRL;
    }

  return mods;
}

static grub_err_t
grub_efi_console_input_init (struct grub_term_input *term)
{
  static grub_guid_t text_input_ex_guid =
    GRUB_EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;

  if (grub_efi_is_finished)
    return 0;

  grub_efi_simple_text_input_ex_interface_t *text_input = term->data;
  if (text_input)
    return 0;

  text_input = grub_efi_open_protocol(grub_efi_system_table->console_in_handler,
				      &text_input_ex_guid,
				      GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  term->data = (void *)text_input;

  return 0;
}

static int
grub_console_getkey (struct grub_term_input *term)
{
  if (grub_efi_is_finished)
    return 0;

  if (term->data || d1_console_leaf_active ())
    return grub_console_getkey_ex(term);
  else
    return grub_console_getkey_con(term);
}

static struct grub_term_coordinate
grub_console_getwh (struct grub_term_output *term)
{
  grub_efi_simple_text_output_interface_t *o;
  grub_efi_uintn_t columns, rows;

  o = grub_efi_system_table->con_out;
  if (grub_prepare_for_text_output (term) != GRUB_ERR_NONE ||
      o->query_mode (o, o->mode->mode,
		     &columns, &rows) != GRUB_EFI_SUCCESS)
    {
      /* Why does this fail?  */
      columns = 80;
      rows = 25;
    }

  return (struct grub_term_coordinate) { columns, rows };
}

static struct grub_term_coordinate
grub_console_getxy (struct grub_term_output *term __attribute__ ((unused)))
{
  grub_efi_simple_text_output_interface_t *o;

  if (grub_efi_is_finished || text_mode != GRUB_TEXT_MODE_AVAILABLE)
    return (struct grub_term_coordinate) { 0, 0 };

  o = grub_efi_system_table->con_out;
  return (struct grub_term_coordinate) { o->mode->cursor_column, o->mode->cursor_row };
}

static void
grub_console_gotoxy (struct grub_term_output *term,
		     struct grub_term_coordinate pos)
{
  grub_efi_simple_text_output_interface_t *o;

  if (grub_prepare_for_text_output (term) != GRUB_ERR_NONE)
    return;

  o = grub_efi_system_table->con_out;
  o->set_cursor_position (o, pos.x, pos.y);
}

static void
grub_console_cls (struct grub_term_output *term __attribute__ ((unused)))
{
  grub_efi_simple_text_output_interface_t *o;
  grub_efi_int32_t orig_attr;

  if (grub_efi_is_finished || text_mode != GRUB_TEXT_MODE_AVAILABLE)
    return;

  o = grub_efi_system_table->con_out;
  orig_attr = o->mode->attribute;
  o->set_attributes (o, GRUB_EFI_BACKGROUND_BLACK);
  o->clear_screen (o);
  o->set_attributes (o, orig_attr);
}

static grub_err_t
grub_efi_console_output_fini (struct grub_term_output *term)
{
  if (text_mode != GRUB_TEXT_MODE_AVAILABLE)
    return 0;

  grub_console_setcursor (term, 0);
  grub_efi_set_text_mode (0);
  text_mode = GRUB_TEXT_MODE_UNDEFINED;
  return 0;
}

static struct grub_term_input grub_console_term_input =
  {
    .name = "console",
    .getkey = grub_console_getkey,
    .getkeystatus = grub_console_getkeystatus,
    .init = grub_efi_console_input_init,
  };

static struct grub_term_output grub_console_term_output =
  {
    .name = "console",
    .fini = grub_efi_console_output_fini,
    .putchar = grub_console_putchar,
    .getwh = grub_console_getwh,
    .getxy = grub_console_getxy,
    .gotoxy = grub_console_gotoxy,
    .cls = grub_console_cls,
    .setcolorstate = grub_console_setcolorstate,
    .setcursor = grub_console_setcursor,
    .flags = GRUB_TERM_CODE_TYPE_VISUAL_GLYPHS,
    .progress_update_divisor = GRUB_PROGRESS_FAST
  };

void
grub_console_init (void)
{
  grub_term_register_output ("console", &grub_console_term_output);
  grub_term_register_input ("console", &grub_console_term_input);
}

void
grub_console_fini (void)
{
  grub_term_unregister_input (&grub_console_term_input);
  grub_term_unregister_output (&grub_console_term_output);
}
