// SPDX-License-Identifier: LGPL-2.1-or-later
#include "settings.hpp"

#include "util.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>

namespace szl {

namespace {
std::string g_config_dir;

std::string get_s(GKeyFile *k, const char *g, const char *n, const std::string &def)
{
    char *v = g_key_file_get_string(k, g, n, nullptr);
    if (!v) return def;
    std::string s = v;
    g_free(v);
    return s;
}
int get_i(GKeyFile *k, const char *g, const char *n, int def)
{
    GError *e = nullptr;
    int v = g_key_file_get_integer(k, g, n, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v;
}
bool get_b(GKeyFile *k, const char *g, const char *n, bool def)
{
    GError *e = nullptr;
    gboolean v = g_key_file_get_boolean(k, g, n, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v;
}
std::vector<std::string> get_list(GKeyFile *k, const char *g, const char *n)
{
    std::vector<std::string> out;
    gsize len = 0;
    char **v = g_key_file_get_string_list(k, g, n, &len, nullptr);
    if (!v) return out;
    for (gsize i = 0; i < len; i++) out.emplace_back(v[i]);
    g_strfreev(v);
    return out;
}
void set_list(GKeyFile *k, const char *g, const char *n, const std::vector<std::string> &l)
{
    std::vector<const char *> p;
    for (const auto &s : l) p.push_back(s.c_str());
    g_key_file_set_string_list(k, g, n, p.data(), p.size());
}
}  // namespace

void set_config_dir(const std::string &dir) { g_config_dir = dir; }

std::string config_dir()
{
    if (g_config_dir.empty()) {
        const char *env = g_getenv("SZL_PROFILE");
        g_config_dir = env && *env ? env : path_join(g_get_user_config_dir(), "7zipl");
    }
    return g_config_dir;
}

Settings &settings()
{
    static Settings s;
    return s;
}

void Settings::load()
{
    GKeyFile *k = g_key_file_new();
    const std::string file = path_join(config_dir(), "7zipl.conf");
    if (!g_key_file_load_from_file(k, file.c_str(), G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(k);
        return;
    }
    show_hidden = get_b(k, "View", "ShowHidden", show_hidden);
    show_dotdot = get_b(k, "View", "ShowDotDot", show_dotdot);
    show_grid = get_b(k, "View", "ShowGrid", show_grid);
    single_click = get_b(k, "View", "SingleClick", single_click);
    show_toolbar = get_b(k, "View", "Toolbar", show_toolbar);
    toolbar_text = get_b(k, "View", "ToolbarText", toolbar_text);
    show_statusbar = get_b(k, "View", "StatusBar", show_statusbar);
    sort_column = get_i(k, "View", "SortColumn", sort_column);
    sort_descending = get_b(k, "View", "SortDescending", sort_descending);
    width = std::max(400, get_i(k, "View", "Width", width));
    height = std::max(300, get_i(k, "View", "Height", height));
    last_folder = get_s(k, "View", "LastFolder", last_folder);
    auto fav = get_list(k, "Favorites", "Folders");
    for (size_t i = 0; i < fav.size() && i < favorites.size(); i++) favorites[i] = fav[i];
    extract_history = get_list(k, "History", "Extract");
    archive_history = get_list(k, "History", "Archives");

    CompressDefaults &c = compress;
    c.format = get_s(k, "Compress", "Format", c.format);
    c.level = get_i(k, "Compress", "Level", c.level);
    c.method = get_s(k, "Compress", "Method", c.method);
    c.dictionary = get_s(k, "Compress", "Dictionary", c.dictionary);
    c.word = get_s(k, "Compress", "Word", c.word);
    c.solid = get_s(k, "Compress", "Solid", c.solid);
    c.threads = get_i(k, "Compress", "Threads", c.threads);
    c.update_mode = get_i(k, "Compress", "UpdateMode", c.update_mode);
    c.path_mode = get_i(k, "Compress", "PathMode", c.path_mode);
    c.encrypt_names = get_b(k, "Compress", "EncryptNames", c.encrypt_names);
    c.encryption = get_s(k, "Compress", "Encryption", c.encryption);
    c.store_symlinks = get_b(k, "Compress", "StoreSymLinks", c.store_symlinks);
    c.store_hardlinks = get_b(k, "Compress", "StoreHardLinks", c.store_hardlinks);
    c.shared_files = get_b(k, "Compress", "SharedFiles", c.shared_files);
    c.parameters = get_s(k, "Compress", "Parameters", c.parameters);

    extract.path_mode = get_i(k, "Extract", "PathMode", extract.path_mode);
    extract.overwrite = get_i(k, "Extract", "Overwrite", extract.overwrite);
    extract.subfolder = get_b(k, "Extract", "Subfolder", extract.subfolder);
    extract.elim_dup = get_b(k, "Extract", "ElimDup", extract.elim_dup);
    hash_method = get_s(k, "Tools", "HashMethod", hash_method);
    work_mode = get_i(k, "Folders", "WorkMode", work_mode);
    work_dir = get_s(k, "Folders", "WorkDir", work_dir);
    viewer = get_s(k, "Editor", "Viewer", viewer);
    editor = get_s(k, "Editor", "Editor", editor);
    diff = get_s(k, "Editor", "Diff", diff);
    real_icons = get_b(k, "View", "RealIcons", real_icons);
    mem_limit = get_b(k, "Settings", "MemLimit", mem_limit);
    mem_limit_gb = std::max(1, get_i(k, "Settings", "MemLimitGB", mem_limit_gb));
    ctx_items = (unsigned)get_i(k, "ContextMenu", "Items", (int)ctx_items);
    ctx_cascaded = get_b(k, "ContextMenu", "Cascaded", ctx_cascaded);
    view_mode = std::clamp(get_i(k, "View", "Mode", view_mode), 0, 3);
    two_panels = get_b(k, "View", "TwoPanels", two_panels);
    auto_refresh = get_b(k, "View", "AutoRefresh", auto_refresh);
    large_buttons = get_b(k, "View", "LargeButtons", large_buttons);
    last_folder2 = get_s(k, "View", "LastFolder2", last_folder2);
    folder_history = get_list(k, "History", "Folders");
    g_key_file_free(k);
}

void Settings::save() const
{
    GKeyFile *k = g_key_file_new();
    g_key_file_set_boolean(k, "View", "ShowHidden", show_hidden);
    g_key_file_set_boolean(k, "View", "ShowDotDot", show_dotdot);
    g_key_file_set_boolean(k, "View", "ShowGrid", show_grid);
    g_key_file_set_boolean(k, "View", "SingleClick", single_click);
    g_key_file_set_boolean(k, "View", "Toolbar", show_toolbar);
    g_key_file_set_boolean(k, "View", "ToolbarText", toolbar_text);
    g_key_file_set_boolean(k, "View", "StatusBar", show_statusbar);
    g_key_file_set_integer(k, "View", "SortColumn", sort_column);
    g_key_file_set_boolean(k, "View", "SortDescending", sort_descending);
    g_key_file_set_integer(k, "View", "Width", width);
    g_key_file_set_integer(k, "View", "Height", height);
    g_key_file_set_string(k, "View", "LastFolder", last_folder.c_str());
    set_list(k, "Favorites", "Folders", favorites);
    set_list(k, "History", "Extract", extract_history);
    set_list(k, "History", "Archives", archive_history);

    const CompressDefaults &c = compress;
    g_key_file_set_string(k, "Compress", "Format", c.format.c_str());
    g_key_file_set_integer(k, "Compress", "Level", c.level);
    g_key_file_set_string(k, "Compress", "Method", c.method.c_str());
    g_key_file_set_string(k, "Compress", "Dictionary", c.dictionary.c_str());
    g_key_file_set_string(k, "Compress", "Word", c.word.c_str());
    g_key_file_set_string(k, "Compress", "Solid", c.solid.c_str());
    g_key_file_set_integer(k, "Compress", "Threads", c.threads);
    g_key_file_set_integer(k, "Compress", "UpdateMode", c.update_mode);
    g_key_file_set_integer(k, "Compress", "PathMode", c.path_mode);
    g_key_file_set_boolean(k, "Compress", "EncryptNames", c.encrypt_names);
    g_key_file_set_string(k, "Compress", "Encryption", c.encryption.c_str());
    g_key_file_set_boolean(k, "Compress", "StoreSymLinks", c.store_symlinks);
    g_key_file_set_boolean(k, "Compress", "StoreHardLinks", c.store_hardlinks);
    g_key_file_set_boolean(k, "Compress", "SharedFiles", c.shared_files);
    g_key_file_set_string(k, "Compress", "Parameters", c.parameters.c_str());

    g_key_file_set_integer(k, "Extract", "PathMode", extract.path_mode);
    g_key_file_set_integer(k, "Extract", "Overwrite", extract.overwrite);
    g_key_file_set_boolean(k, "Extract", "Subfolder", extract.subfolder);
    g_key_file_set_boolean(k, "Extract", "ElimDup", extract.elim_dup);
    g_key_file_set_string(k, "Tools", "HashMethod", hash_method.c_str());
    g_key_file_set_integer(k, "Folders", "WorkMode", work_mode);
    g_key_file_set_string(k, "Folders", "WorkDir", work_dir.c_str());
    g_key_file_set_string(k, "Editor", "Viewer", viewer.c_str());
    g_key_file_set_string(k, "Editor", "Editor", editor.c_str());
    g_key_file_set_string(k, "Editor", "Diff", diff.c_str());
    g_key_file_set_boolean(k, "View", "RealIcons", real_icons);
    g_key_file_set_boolean(k, "Settings", "MemLimit", mem_limit);
    g_key_file_set_integer(k, "Settings", "MemLimitGB", mem_limit_gb);
    g_key_file_set_integer(k, "ContextMenu", "Items", (int)ctx_items);
    g_key_file_set_boolean(k, "ContextMenu", "Cascaded", ctx_cascaded);
    g_key_file_set_integer(k, "View", "Mode", view_mode);
    g_key_file_set_boolean(k, "View", "TwoPanels", two_panels);
    g_key_file_set_boolean(k, "View", "AutoRefresh", auto_refresh);
    g_key_file_set_boolean(k, "View", "LargeButtons", large_buttons);
    g_key_file_set_string(k, "View", "LastFolder2", last_folder2.c_str());
    set_list(k, "History", "Folders", folder_history);

    g_mkdir_with_parents(config_dir().c_str(), 0700);
    const std::string file = path_join(config_dir(), "7zipl.conf");
    g_key_file_save_to_file(k, file.c_str(), nullptr);
    g_key_file_free(k);
}

std::vector<std::string> Settings::work_args() const
{
    if (work_mode == 0) return {"-w" + std::string(g_get_tmp_dir())};
    if (work_mode == 2 && !work_dir.empty() && g_file_test(work_dir.c_str(), G_FILE_TEST_IS_DIR)) return {"-w" + work_dir};
    return {};  // 7-Zip's default: next to the archive
}

void Settings::push_history(std::vector<std::string> &list, const std::string &value)
{
    if (value.empty()) return;
    list.erase(std::remove(list.begin(), list.end(), value), list.end());
    list.insert(list.begin(), value);
    if (list.size() > 20) list.resize(20);
}

}  // namespace szl
