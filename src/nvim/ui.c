// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "klib/kvec.h"
#include "nvim/api/private/helpers.h"
#include "nvim/api/private/validate.h"
#include "nvim/api/ui.h"
#include "nvim/ascii.h"
#include "nvim/autocmd.h"
#include "nvim/buffer.h"
#include "nvim/cursor_shape.h"
#include "nvim/drawscreen.h"
#include "nvim/ex_getln.h"
#include "nvim/gettext.h"
#include "nvim/globals.h"
#include "nvim/grid.h"
#include "nvim/highlight.h"
#include "nvim/highlight_defs.h"
#include "nvim/log.h"
#include "nvim/lua/executor.h"
#include "nvim/map.h"
#include "nvim/memory.h"
#include "nvim/message.h"
#include "nvim/option.h"
#include "nvim/option_vars.h"
#include "nvim/os/time.h"
#include "nvim/strings.h"
#include "nvim/ui.h"
#include "nvim/ui_client.h"
#include "nvim/ui_compositor.h"
#include "nvim/vim.h"
#include "nvim/window.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "ui.c.generated.h"
#endif

#define MAX_UI_COUNT 16

static UI *uis[MAX_UI_COUNT];
static bool ui_ext[kUIExtCount] = { 0 };
static size_t ui_count = 0;
static int ui_mode_idx = SHAPE_IDX_N;
static int cursor_row = 0, cursor_col = 0;
static bool pending_cursor_update = false;
static int busy = 0;
static bool pending_mode_info_update = false;
static bool pending_mode_update = false;
static handle_T cursor_grid_handle = DEFAULT_GRID_HANDLE;

static PMap(uint32_t) ui_event_cbs = MAP_INIT;
bool ui_cb_ext[kUIExtCount];  ///< Internalized UI capabilities.

static bool has_mouse = false;
static int pending_has_mouse = -1;

static Array call_buf = ARRAY_DICT_INIT;

#ifdef NVIM_LOG_DEBUG
static size_t uilog_seen = 0;
static const char *uilog_last_event = NULL;

static void ui_log(const char *funname)
{
# ifdef EXITFREE
  if (entered_free_all_mem) {
    return;  // do nothing, we cannot log now
  }
# endif

  if (uilog_last_event == funname) {
    uilog_seen++;
  } else {
    if (uilog_seen > 0) {
      logmsg(LOGLVL_DBG, "UI: ", NULL, -1, true,
             "%s (+%zu times...)", uilog_last_event, uilog_seen);
    }
    logmsg(LOGLVL_DBG, "UI: ", NULL, -1, true, "%s", funname);
    uilog_seen = 0;
    uilog_last_event = funname;
  }
}
#else
# define ui_log(funname)
#endif

// UI_CALL invokes a function on all registered UI instances.
// This is called by code generated by generators/gen_api_ui_events.lua
// C code should use ui_call_{funname} instead.
#define UI_CALL(cond, funname, ...) \
  do { \
    bool any_call = false; \
    for (size_t i = 0; i < ui_count; i++) { \
      UI *ui = uis[i]; \
      if ((cond)) { \
        remote_ui_##funname(__VA_ARGS__); \
        any_call = true; \
      } \
    } \
    if (any_call) { \
      ui_log(STR(funname)); \
    } \
  } while (0)

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "ui_events_call.generated.h"
#endif

void ui_init(void)
{
  default_grid.handle = 1;
  msg_grid_adj.target = &default_grid;
  ui_comp_init();
  kv_ensure_space(call_buf, 16);
}

#ifdef EXITFREE
void ui_free_all_mem(void)
{
  kv_destroy(call_buf);

  UIEventCallback *event_cb;
  map_foreach_value(&ui_event_cbs, event_cb, {
    free_ui_event_callback(event_cb);
  })
  map_destroy(uint32_t, &ui_event_cbs);
}
#endif

/// Returns true if any `rgb=true` UI is attached.
bool ui_rgb_attached(void)
{
  if (!headless_mode && p_tgc) {
    return true;
  }
  for (size_t i = 0; i < ui_count; i++) {
    if (uis[i]->rgb) {
      return true;
    }
  }
  return false;
}

/// Returns true if a GUI is attached.
bool ui_gui_attached(void)
{
  for (size_t i = 0; i < ui_count; i++) {
    bool tui = uis[i]->stdin_tty || uis[i]->stdout_tty;
    if (!tui) {
      return true;
    }
  }
  return false;
}

/// Returns true if any UI requested `override=true`.
bool ui_override(void)
{
  for (size_t i = 0; i < ui_count; i++) {
    if (uis[i]->override) {
      return true;
    }
  }
  return false;
}

bool ui_active(void)
{
  return ui_count > 0;
}

void ui_refresh(void)
{
  if (ui_client_channel_id) {
    abort();
  }

  if (!ui_active()) {
    return;
  }

  if (updating_screen) {
    ui_schedule_refresh();
    return;
  }

  int width = INT_MAX, height = INT_MAX;
  bool ext_widgets[kUIExtCount];
  for (UIExtension i = 0; (int)i < kUIExtCount; i++) {
    ext_widgets[i] = true;
  }

  bool inclusive = ui_override();
  for (size_t i = 0; i < ui_count; i++) {
    UI *ui = uis[i];
    width = MIN(ui->width, width);
    height = MIN(ui->height, height);
    for (UIExtension j = 0; (int)j < kUIExtCount; j++) {
      ext_widgets[j] &= (ui->ui_ext[j] || inclusive);
    }
  }

  cursor_row = cursor_col = 0;
  pending_cursor_update = true;

  for (UIExtension i = 0; (int)i < kUIExtCount; i++) {
    if (i < kUIGlobalCount) {
      ext_widgets[i] |= ui_cb_ext[i];
    }
    ui_ext[i] = ext_widgets[i];
    if (i < kUIGlobalCount) {
      ui_call_option_set(cstr_as_string((char *)ui_ext_names[i]),
                         BOOLEAN_OBJ(ext_widgets[i]));
    }
  }

  ui_default_colors_set();

  int save_p_lz = p_lz;
  p_lz = false;  // convince redrawing() to return true ...
  screen_resize(width, height);
  p_lz = save_p_lz;

  if (ext_widgets[kUIMessages]) {
    set_option_value("cmdheight", NUMBER_OPTVAL(0), 0);
    command_height();
  }
  ui_mode_info_set();
  pending_mode_update = true;
  ui_cursor_shape();
  pending_has_mouse = -1;
}

int ui_pum_get_height(void)
{
  int pum_height = 0;
  for (size_t i = 0; i < ui_count; i++) {
    int ui_pum_height = uis[i]->pum_nlines;
    if (ui_pum_height) {
      pum_height =
        pum_height != 0 ? MIN(pum_height, ui_pum_height) : ui_pum_height;
    }
  }
  return pum_height;
}

bool ui_pum_get_pos(double *pwidth, double *pheight, double *prow, double *pcol)
{
  for (size_t i = 0; i < ui_count; i++) {
    if (!uis[i]->pum_pos) {
      continue;
    }
    *pwidth = uis[i]->pum_width;
    *pheight = uis[i]->pum_height;
    *prow = uis[i]->pum_row;
    *pcol = uis[i]->pum_col;
    return true;
  }
  return false;
}

static void ui_refresh_event(void **argv)
{
  ui_refresh();
}

void ui_schedule_refresh(void)
{
  multiqueue_put(resize_events, ui_refresh_event, 0);
}

void ui_default_colors_set(void)
{
  ui_call_default_colors_set(normal_fg, normal_bg, normal_sp,
                             cterm_normal_fg_color, cterm_normal_bg_color);
}

void ui_busy_start(void)
{
  if (!(busy++)) {
    ui_call_busy_start();
  }
}

void ui_busy_stop(void)
{
  if (!(--busy)) {
    ui_call_busy_stop();
  }
}

/// Emit a bell or visualbell as a warning
///
/// val is one of the BO_ values, e.g., BO_OPER
void vim_beep(unsigned val)
{
  called_vim_beep = true;

  if (emsg_silent != 0 || in_assert_fails) {
    return;
  }

  if (!((bo_flags & val) || (bo_flags & BO_ALL))) {
    static int beeps = 0;
    static uint64_t start_time = 0;

    // Only beep up to three times per half a second,
    // otherwise a sequence of beeps would freeze Vim.
    if (start_time == 0 || os_hrtime() - start_time > 500000000U) {
      beeps = 0;
      start_time = os_hrtime();
    }
    beeps++;
    if (beeps <= 3) {
      if (p_vb) {
        ui_call_visual_bell();
      } else {
        ui_call_bell();
      }
    }
  }

  // When 'debug' contains "beep" produce a message.  If we are sourcing
  // a script or executing a function give the user a hint where the beep
  // comes from.
  if (vim_strchr(p_debug, 'e') != NULL) {
    msg_source(HL_ATTR(HLF_W));
    msg(_("Beep!"), HL_ATTR(HLF_W));
  }
}

void ui_attach_impl(UI *ui, uint64_t chanid)
{
  if (ui_count == MAX_UI_COUNT) {
    abort();
  }
  if (!ui->ui_ext[kUIMultigrid] && !ui->ui_ext[kUIFloatDebug]
      && !ui_client_channel_id) {
    ui_comp_attach(ui);
  }

  uis[ui_count++] = ui;
  ui_refresh_options();
  resettitle();

  for (UIExtension i = kUIGlobalCount; (int)i < kUIExtCount; i++) {
    ui_set_ext_option(ui, i, ui->ui_ext[i]);
  }

  bool sent = false;
  if (ui->ui_ext[kUIHlState]) {
    sent = highlight_use_hlstate();
  }
  if (!sent) {
    ui_send_all_hls(ui);
  }
  ui_refresh();

  do_autocmd_uienter(chanid, true);
}

void ui_detach_impl(UI *ui, uint64_t chanid)
{
  size_t shift_index = MAX_UI_COUNT;

  // Find the index that will be removed
  for (size_t i = 0; i < ui_count; i++) {
    if (uis[i] == ui) {
      shift_index = i;
      break;
    }
  }

  if (shift_index == MAX_UI_COUNT) {
    abort();
  }

  // Shift UIs at "shift_index"
  while (shift_index < ui_count - 1) {
    uis[shift_index] = uis[shift_index + 1];
    shift_index++;
  }

  if (--ui_count
      // During teardown/exit the loop was already destroyed, cannot schedule.
      // https://github.com/neovim/neovim/pull/5119#issuecomment-258667046
      && !exiting) {
    ui_schedule_refresh();
  }

  if (!ui->ui_ext[kUIMultigrid] && !ui->ui_ext[kUIFloatDebug]) {
    ui_comp_detach(ui);
  }

  do_autocmd_uienter(chanid, false);
}

void ui_set_ext_option(UI *ui, UIExtension ext, bool active)
{
  if (ext < kUIGlobalCount) {
    ui_refresh();
    return;
  }
  if (ui_ext_names[ext][0] != '_' || active) {
    remote_ui_option_set(ui, cstr_as_string((char *)ui_ext_names[ext]),
                         BOOLEAN_OBJ(active));
  }
  if (ext == kUITermColors) {
    ui_default_colors_set();
  }
}

void ui_line(ScreenGrid *grid, int row, int startcol, int endcol, int clearcol, int clearattr,
             bool wrap)
{
  assert(0 <= row && row < grid->rows);
  LineFlags flags = wrap ? kLineFlagWrap : 0;
  if (startcol == -1) {
    startcol = 0;
    flags |= kLineFlagInvalid;
  }

  size_t off = grid->line_offset[row] + (size_t)startcol;

  ui_call_raw_line(grid->handle, row, startcol, endcol, clearcol, clearattr,
                   flags, (const schar_T *)grid->chars + off,
                   (const sattr_T *)grid->attrs + off);

  // 'writedelay': flush & delay each time.
  if (p_wd && (rdb_flags & RDB_LINE)) {
    // If 'writedelay' is active, set the cursor to indicate what was drawn.
    ui_call_grid_cursor_goto(grid->handle, row,
                             MIN(clearcol, (int)grid->cols - 1));
    ui_call_flush();
    uint64_t wd = (uint64_t)llabs(p_wd);
    os_sleep(wd);
    pending_cursor_update = true;  // restore the cursor later
  }
}

void ui_cursor_goto(int new_row, int new_col)
{
  ui_grid_cursor_goto(DEFAULT_GRID_HANDLE, new_row, new_col);
}

void ui_grid_cursor_goto(handle_T grid_handle, int new_row, int new_col)
{
  if (new_row == cursor_row
      && new_col == cursor_col
      && grid_handle == cursor_grid_handle) {
    return;
  }

  cursor_row = new_row;
  cursor_col = new_col;
  cursor_grid_handle = grid_handle;
  pending_cursor_update = true;
}

/// moving the cursor grid will implicitly move the cursor
void ui_check_cursor_grid(handle_T grid_handle)
{
  if (cursor_grid_handle == grid_handle) {
    pending_cursor_update = true;
  }
}

void ui_mode_info_set(void)
{
  pending_mode_info_update = true;
}

int ui_current_row(void)
{
  return cursor_row;
}

int ui_current_col(void)
{
  return cursor_col;
}

void ui_flush(void)
{
  assert(!ui_client_channel_id);
  if (!ui_active()) {
    return;
  }
  cmdline_ui_flush();
  win_ui_flush(false);
  msg_ext_ui_flush();
  msg_scroll_flush();

  if (pending_cursor_update) {
    ui_call_grid_cursor_goto(cursor_grid_handle, cursor_row, cursor_col);
    pending_cursor_update = false;
  }
  if (pending_mode_info_update) {
    Arena arena = ARENA_EMPTY;
    Array style = mode_style_array(&arena);
    bool enabled = (*p_guicursor != NUL);
    ui_call_mode_info_set(enabled, style);
    arena_mem_free(arena_finish(&arena));
    pending_mode_info_update = false;
  }
  if (pending_mode_update && !starting) {
    char *full_name = shape_table[ui_mode_idx].full_name;
    ui_call_mode_change(cstr_as_string(full_name), ui_mode_idx);
    pending_mode_update = false;
  }
  if (pending_has_mouse != has_mouse) {
    (has_mouse ? ui_call_mouse_on : ui_call_mouse_off)();
    pending_has_mouse = has_mouse;
  }
  ui_call_flush();

  if (p_wd && (rdb_flags & RDB_FLUSH)) {
    os_sleep((uint64_t)llabs(p_wd));
  }
}

/// Check if 'mouse' is active for the current mode
///
/// TODO(bfredl): precompute the State -> active mapping when 'mouse' changes,
/// then this can be checked directly in ui_flush()
void ui_check_mouse(void)
{
  has_mouse = false;
  // Be quick when mouse is off.
  if (*p_mouse == NUL) {
    return;
  }

  int checkfor = MOUSE_NORMAL;  // assume normal mode
  if (VIsual_active) {
    checkfor = MOUSE_VISUAL;
  } else if (State == MODE_HITRETURN || State == MODE_ASKMORE || State == MODE_SETWSIZE) {
    checkfor = MOUSE_RETURN;
  } else if (State & MODE_INSERT) {
    checkfor = MOUSE_INSERT;
  } else if (State & MODE_CMDLINE) {
    checkfor = MOUSE_COMMAND;
  } else if (State == MODE_CONFIRM || State == MODE_EXTERNCMD) {
    checkfor = ' ';  // don't use mouse for ":confirm" or ":!cmd"
  }

  // mouse should be active if at least one of the following is true:
  // - "c" is in 'mouse', or
  // - 'a' is in 'mouse' and "c" is in MOUSE_A, or
  // - the current buffer is a help file and 'h' is in 'mouse' and we are in a
  //   normal editing mode (not at hit-return message).
  for (char *p = p_mouse; *p; p++) {
    switch (*p) {
    case 'a':
      if (vim_strchr(MOUSE_A, checkfor) != NULL) {
        has_mouse = true;
        return;
      }
      break;
    case MOUSE_HELP:
      if (checkfor != MOUSE_RETURN && curbuf->b_help) {
        has_mouse = true;
        return;
      }
      break;
    default:
      if (checkfor == *p) {
        has_mouse = true;
        return;
      }
    }
  }
}

/// Check if current mode has changed.
///
/// May update the shape of the cursor.
void ui_cursor_shape(void)
{
  if (!full_screen) {
    return;
  }
  int new_mode_idx = cursor_get_mode_idx();

  if (new_mode_idx != ui_mode_idx) {
    ui_mode_idx = new_mode_idx;
    pending_mode_update = true;
  }
  conceal_check_cursor_line();
}

/// Returns true if the given UI extension is enabled.
bool ui_has(UIExtension ext)
{
  return ui_ext[ext];
}

Array ui_array(void)
{
  Array all_uis = ARRAY_DICT_INIT;
  for (size_t i = 0; i < ui_count; i++) {
    UI *ui = uis[i];
    Dictionary info = ARRAY_DICT_INIT;
    PUT(info, "width", INTEGER_OBJ(ui->width));
    PUT(info, "height", INTEGER_OBJ(ui->height));
    PUT(info, "rgb", BOOLEAN_OBJ(ui->rgb));
    PUT(info, "override", BOOLEAN_OBJ(ui->override));

    // TUI fields. (`stdin_fd` is intentionally omitted.)
    PUT(info, "term_name", CSTR_TO_OBJ(ui->term_name));
    PUT(info, "term_background", CSTR_TO_OBJ(ui->term_background));
    PUT(info, "term_colors", INTEGER_OBJ(ui->term_colors));
    PUT(info, "stdin_tty", BOOLEAN_OBJ(ui->stdin_tty));
    PUT(info, "stdout_tty", BOOLEAN_OBJ(ui->stdout_tty));

    for (UIExtension j = 0; j < kUIExtCount; j++) {
      if (ui_ext_names[j][0] != '_' || ui->ui_ext[j]) {
        PUT(info, ui_ext_names[j], BOOLEAN_OBJ(ui->ui_ext[j]));
      }
    }
    remote_ui_inspect(ui, &info);
    ADD(all_uis, DICTIONARY_OBJ(info));
  }
  return all_uis;
}

void ui_grid_resize(handle_T grid_handle, int width, int height, Error *err)
{
  if (grid_handle == DEFAULT_GRID_HANDLE) {
    screen_resize(width, height);
    return;
  }

  win_T *wp = get_win_by_grid_handle(grid_handle);
  VALIDATE_INT((wp != NULL), "window handle", (int64_t)grid_handle, {
    return;
  });

  if (wp->w_floating) {
    if (width != wp->w_width || height != wp->w_height) {
      wp->w_config.width = width;
      wp->w_config.height = height;
      win_config_float(wp, wp->w_config);
    }
  } else {
    // non-positive indicates no request
    wp->w_height_request = MAX(height, 0);
    wp->w_width_request = MAX(width, 0);
    win_set_inner_size(wp, true);
  }
}

void ui_call_event(char *name, Array args)
{
  UIEventCallback *event_cb;
  bool handled = false;
  map_foreach_value(&ui_event_cbs, event_cb, {
    Error err = ERROR_INIT;
    Object res = nlua_call_ref(event_cb->cb, name, args, false, &err);
    if (res.type == kObjectTypeBoolean && res.data.boolean == true) {
      handled = true;
    }
    if (ERROR_SET(&err)) {
      ELOG("Error while executing ui_comp_event callback: %s", err.msg);
    }
    api_clear_error(&err);
  })

  if (!handled) {
    UI_CALL(true, event, ui, name, args);
  }

  ui_log(name);
}

void ui_cb_update_ext(void)
{
  memset(ui_cb_ext, 0, ARRAY_SIZE(ui_cb_ext));

  for (size_t i = 0; i < kUIGlobalCount; i++) {
    UIEventCallback *event_cb;

    map_foreach_value(&ui_event_cbs, event_cb, {
      if (event_cb->ext_widgets[i]) {
        ui_cb_ext[i] = true;
        break;
      }
    })
  }
}

void free_ui_event_callback(UIEventCallback *event_cb)
{
  api_free_luaref(event_cb->cb);
  xfree(event_cb);
}

void ui_add_cb(uint32_t ns_id, LuaRef cb, bool *ext_widgets)
{
  UIEventCallback *event_cb = xcalloc(1, sizeof(UIEventCallback));
  event_cb->cb = cb;
  memcpy(event_cb->ext_widgets, ext_widgets, ARRAY_SIZE(event_cb->ext_widgets));
  if (event_cb->ext_widgets[kUIMessages]) {
    event_cb->ext_widgets[kUICmdline] = true;
  }

  ptr_t *item = pmap_put_ref(uint32_t)(&ui_event_cbs, ns_id, NULL, NULL);
  if (*item) {
    free_ui_event_callback((UIEventCallback *)(*item));
  }
  *item = event_cb;

  ui_cb_update_ext();
  ui_refresh();
}

void ui_remove_cb(uint32_t ns_id)
{
  if (map_has(uint32_t, &ui_event_cbs, ns_id)) {
    UIEventCallback *item = pmap_del(uint32_t)(&ui_event_cbs, ns_id, NULL);
    free_ui_event_callback(item);
  }
  ui_cb_update_ext();
  ui_refresh();
}
