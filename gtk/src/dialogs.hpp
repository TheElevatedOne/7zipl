// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: small dialogs (password, overwrite, properties, checksums, about).
#pragma once

#include "engine.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace szl {

// Enter password. `for_new` asks twice (a new archive). `cb` runs exactly once: (false, "") on cancel.
void password_dialog(GtkWindow *parent, const std::string &archive, bool for_new,
                     std::function<void(bool, const std::string &)> cb);

// "Confirm File Replace". `cb` runs exactly once (Cancel when the window is closed).
void overwrite_dialog(GtkWindow *parent, const std::string &existing, uint64_t existing_size, int64_t existing_mtime,
                      const std::string &incoming, uint64_t incoming_size, int64_t incoming_mtime,
                      std::function<void(Ui::Overwrite)> cb);

// Name/value list (File > Properties, archive info).
void properties_window(GtkWindow *parent, const std::string &title,
                       const std::vector<std::pair<std::string, std::string>> &rows);

// Checksum results (File > CRC).
void hash_window(GtkWindow *parent, const HashResult &h);

void about_dialog(GtkWindow *parent);

}  // namespace szl
