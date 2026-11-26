// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: 7-Zip's "Extract" dialog.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace szl {

struct ExtractChoice {
    std::string out_dir;     // may end with "/*" for "one folder per archive"
    int path_mode = 0;       // 0 full, 1 none, 2 absolute
    int overwrite = 0;       // 0 ask, 1 overwrite, 2 skip, 3 rename new, 4 rename existing
    bool elim_dup = true;
    std::string password;
};

// `archives`: the archives (for the title and the proposed folder name). `in_archive`: extracting items
// from an open archive (no per-archive subfolder or root-folder elimination options).
void extract_dialog(GtkWindow *parent, const std::string &base_dir, const std::vector<std::string> &archives,
                    bool in_archive, std::function<void(const ExtractChoice &)> cb);

// 7-Zip command line for extracting whole archives on disk.
std::vector<std::string> extract_args(const ExtractChoice &c, const std::vector<std::string> &archives, bool test = false);

}  // namespace szl
