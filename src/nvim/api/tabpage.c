// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include <stdbool.h>
#include <stdlib.h>

#include "nvim/api/private/converter.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/helpers.h"
#include "nvim/api/tabpage.h"
#include "nvim/api/vim.h"
#include "nvim/lua/executor.h"
#include "nvim/buffer_defs.h"
#include "nvim/eval/typval.h"
#include "nvim/eval/typval_defs.h"
#include "nvim/eval/typval_encode.h"
#include "nvim/eval/window.h"
#include "nvim/globals.h"
#include "nvim/memory.h"
#include "nvim/window.h"

/// Returns the layout of a tabpage as a tree-like nested list
///
/// @param tabpage Tabpage handle, or 0 for the current tabpage
/// @param[out] err Error details, if any.
/// @return Tree of windows and frames in tabpage, or an empty array if the tab is invalid
Array nvim_tabpage_get_layout(Tabpage tabpage, Error *err)
  FUNC_API_SINCE(10)
{
  tabpage_T *tab;
  if (tabpage == 0) {
    tab = curtab;
  } else {
    tab = find_tab_by_handle(tabpage, err);
  }

  if (!tab) {
    return (Array)ARRAY_DICT_INIT;
  }

  list_T *fr_list = tv_list_alloc(2);

  get_framelayout(tab->tp_topframe, fr_list, true);

  typval_T list_tv = {
    .vval.v_list = fr_list,
    .v_type = VAR_LIST,
  };

  Array rv = vim_to_object(&list_tv).data.array;
  tv_clear(&list_tv);
  return rv;
}

/// Sets the window layout of a tabpage based on a tree of windows and frames
///
/// @param tabpage Tabpage handle, or 0 for the current tabpage
/// @param layout The intended layout as a nested list
/// @param[out] err Error details, if any.
void nvim_tabpage_set_layout(Tabpage tabpage, Array layout, Error *err)
  FUNC_API_SINCE(10)
{
  tabpage_T *tab;
  if (tabpage == 0) {
    tab = curtab;
  } else {
    tab = find_tab_by_handle(tabpage, err);
  }

  if (!tab) {
    return;
  }

  RedrawingDisabled++;

  FOR_ALL_WINDOWS_IN_TAB(wp, tab) {
    if (wp != tab->tp_firstwin) {
      win_close(wp, false, true);
    }
  }

  MAXSIZE_TEMP_ARRAY(a, 2);
  ADD(a, TABPAGE_OBJ(tabpage));
  ADD(a, ARRAY_OBJ(layout));

  NLUA_EXEC_STATIC("vim._set_layout(...)", a, err);

  RedrawingDisabled--;
}

/// Gets the windows in a tabpage
///
/// @param tabpage  Tabpage handle, or 0 for current tabpage
/// @param[out] err Error details, if any
/// @return List of windows in `tabpage`
ArrayOf(Window) nvim_tabpage_list_wins(Tabpage tabpage, Error *err)
  FUNC_API_SINCE(1)
{
  Array rv = ARRAY_DICT_INIT;
  tabpage_T *tab = find_tab_by_handle(tabpage, err);

  if (!tab || !valid_tabpage(tab)) {
    return rv;
  }

  FOR_ALL_WINDOWS_IN_TAB(wp, tab) {
    rv.size++;
  }

  rv.items = xmalloc(sizeof(Object) * rv.size);
  size_t i = 0;

  FOR_ALL_WINDOWS_IN_TAB(wp, tab) {
    rv.items[i++] = WINDOW_OBJ(wp->handle);
  }

  return rv;
}

/// Gets a tab-scoped (t:) variable
///
/// @param tabpage  Tabpage handle, or 0 for current tabpage
/// @param name     Variable name
/// @param[out] err Error details, if any
/// @return Variable value
Object nvim_tabpage_get_var(Tabpage tabpage, String name, Error *err)
  FUNC_API_SINCE(1)
{
  tabpage_T *tab = find_tab_by_handle(tabpage, err);

  if (!tab) {
    return (Object)OBJECT_INIT;
  }

  return dict_get_value(tab->tp_vars, name, err);
}

/// Sets a tab-scoped (t:) variable
///
/// @param tabpage  Tabpage handle, or 0 for current tabpage
/// @param name     Variable name
/// @param value    Variable value
/// @param[out] err Error details, if any
void nvim_tabpage_set_var(Tabpage tabpage, String name, Object value, Error *err)
  FUNC_API_SINCE(1)
{
  tabpage_T *tab = find_tab_by_handle(tabpage, err);

  if (!tab) {
    return;
  }

  dict_set_var(tab->tp_vars, name, value, false, false, err);
}

/// Removes a tab-scoped (t:) variable
///
/// @param tabpage  Tabpage handle, or 0 for current tabpage
/// @param name     Variable name
/// @param[out] err Error details, if any
void nvim_tabpage_del_var(Tabpage tabpage, String name, Error *err)
  FUNC_API_SINCE(1)
{
  tabpage_T *tab = find_tab_by_handle(tabpage, err);

  if (!tab) {
    return;
  }

  dict_set_var(tab->tp_vars, name, NIL, true, false, err);
}

/// Gets the current window in a tabpage
///
/// @param tabpage  Tabpage handle, or 0 for current tabpage
/// @param[out] err Error details, if any
/// @return Window handle
Window nvim_tabpage_get_win(Tabpage tabpage, Error *err)
  FUNC_API_SINCE(1)
{
  tabpage_T *tab = find_tab_by_handle(tabpage, err);

  if (!tab || !valid_tabpage(tab)) {
    return 0;
  }

  if (tab == curtab) {
    return nvim_get_current_win();
  }
  FOR_ALL_WINDOWS_IN_TAB(wp, tab) {
    if (wp == tab->tp_curwin) {
      return wp->handle;
    }
  }
  // There should always be a current window for a tabpage
  abort();
}

/// Gets the tabpage number
///
/// @param tabpage  Tabpage handle, or 0 for current tabpage
/// @param[out] err Error details, if any
/// @return Tabpage number
Integer nvim_tabpage_get_number(Tabpage tabpage, Error *err)
  FUNC_API_SINCE(1)
{
  tabpage_T *tab = find_tab_by_handle(tabpage, err);

  if (!tab) {
    return 0;
  }

  return tabpage_index(tab);
}

/// Checks if a tabpage is valid
///
/// @param tabpage  Tabpage handle, or 0 for current tabpage
/// @return true if the tabpage is valid, false otherwise
Boolean nvim_tabpage_is_valid(Tabpage tabpage)
  FUNC_API_SINCE(1)
{
  Error stub = ERROR_INIT;
  Boolean ret = find_tab_by_handle(tabpage, &stub) != NULL;
  api_clear_error(&stub);
  return ret;
}
