// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: Tools > Options, with 7-Zip's pages done the Linux way: System (default application for archive
// types), 7-Zip (file-manager context menus), Folders (working folder), Editor (View/Edit/Diff programs)
// and Settings.
#pragma once

#include <gtk/gtk.h>

#include <functional>

namespace szl {

// `applied` runs after OK or Apply, so the main window can pick up view changes.
void options_dialog(GtkWindow *parent, std::function<void()> applied, int page = 0);

}  // namespace szl
