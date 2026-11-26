// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: settings in ~/.config/7zipl/7zipl.conf (a GKeyFile).
#pragma once

#include <string>
#include <vector>

namespace szl {

struct CompressDefaults {
    std::string format = "7z";
    int level = 5;            // 0 store .. 9 ultra
    std::string method;       // empty = format default
    std::string dictionary;   // empty = auto ("64m")
    std::string word;         // empty = auto
    std::string solid;        // empty = auto ("off", "4g", ...)
    int threads = 0;          // 0 = auto
    int update_mode = 0;      // 0 add+replace, 1 update+add, 2 freshen, 3 synchronize
    int path_mode = 0;        // 0 relative, 1 full, 2 absolute
    bool encrypt_names = false;
    std::string encryption = "AES-256";
    bool store_symlinks = false;
    bool store_hardlinks = false;
    bool shared_files = false;
    bool delete_after = false;
    std::string parameters;
};

struct ExtractDefaults {
    int path_mode = 0;   // 0 full, 1 none, 2 absolute
    int overwrite = 0;   // 0 ask, 1 overwrite, 2 skip, 3 rename new, 4 rename existing
    bool subfolder = true;       // extract into "<archive name>/"
    bool elim_dup = true;        // eliminate duplication of root folder
};

struct Settings {
    // View
    bool show_hidden = false;
    bool show_dotdot = true;      // ".." row at the top (7-Zip: Options > Settings)
    bool show_grid = false;
    bool single_click = false;
    bool show_toolbar = true;
    bool toolbar_text = true;
    bool show_statusbar = true;
    int sort_column = 0;          // 0 name
    bool sort_descending = false;
    int width = 900, height = 600;
    std::string last_folder;
    std::vector<std::string> favorites = std::vector<std::string>(10);  // Alt+0..9
    // Dialog histories
    std::vector<std::string> extract_history;
    std::vector<std::string> archive_history;
    CompressDefaults compress;
    ExtractDefaults extract;
    std::string hash_method = "CRC32";

    void load();
    void save() const;
    void push_history(std::vector<std::string> &list, const std::string &value);
};

// Set the configuration directory (--profile DIR / SZL_PROFILE); default ~/.config/7zipl.
void set_config_dir(const std::string &dir);
std::string config_dir();
Settings &settings();

}  // namespace szl
