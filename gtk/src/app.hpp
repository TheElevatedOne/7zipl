// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the GtkApplication and the command-line modes (the jobs 7zG does for a file manager).
#pragma once

#include <string>
#include <vector>

namespace szl {

enum class Mode { Browse, Extract, ExtractHere, ExtractToFolder, Compress, CompressTo, Test, Hash, SelfTest };

struct AppOptions {
    Mode mode = Mode::Browse;
    std::vector<std::string> files;  // absolute paths
    std::string format = "7z";       // --compress-to
    std::string hash = "CRC32";      // --hash
    std::string screenshot;          // --screenshot FILE
};

int run_app(const char *argv0, const AppOptions &opts);

}  // namespace szl
