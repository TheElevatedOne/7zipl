// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: 7-Zip's "Add to Archive" dialog.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace szl {

struct CompressRequest {
    std::string archive;            // absolute path of the archive to create or update
    std::vector<std::string> args;  // 7-Zip command line (run() arguments)
    bool show_ratio = true;
};

// `files`: absolute paths to add. `base_dir`: where a relative archive name goes.
// `archive`: proposed archive name (path); empty = derived from the files.
void compress_dialog(GtkWindow *parent, const std::string &base_dir, const std::vector<std::string> &files,
                     const std::string &archive, std::function<void(const CompressRequest &)> cb);

// Builds an "add these files to this archive" command with the saved defaults, no dialog
// (used for quick "Add to name.7z" and for adding to an open archive).
CompressRequest quick_compress(const std::string &archive, const std::vector<std::string> &files,
                               const std::string &format = {}, const std::string &password = {});

}  // namespace szl
