// SPDX-License-Identifier: LGPL-2.1-or-later
#include "util.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <ctime>
#include <mutex>

namespace szl {

std::string format_number(uint64_t v)
{
    std::string s = std::to_string(v);
    std::string out;
    int n = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (n && n % 3 == 0) out.insert(out.begin(), ' ');
        out.insert(out.begin(), *it);
        n++;
    }
    return out;
}

std::string format_size_short(uint64_t v)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    if (v < 1024) return std::to_string(v) + " B";
    double d = (double)v;
    int u = 0;
    while (d >= 1024 && u < 5) {
        d /= 1024;
        u++;
    }
    char buf[64];
    g_snprintf(buf, sizeof buf, d < 10 ? "%.1f %s" : "%.0f %s", d, units[u]);
    return buf;
}

std::string format_time(int64_t t)
{
    GDateTime *dt = g_date_time_new_from_unix_local(t);
    if (!dt) return {};
    char *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
    std::string out = s ? s : "";
    g_free(s);
    g_date_time_unref(dt);
    return out;
}

std::string format_duration(uint64_t s)
{
    char buf[64];
    g_snprintf(buf, sizeof buf, "%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    return buf;
}

std::string path_basename(const std::string &p)
{
    if (p == "/") return p;
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const auto pos = s.rfind('/');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

std::string path_dirname(const std::string &p)
{
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const auto pos = s.rfind('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return s.substr(0, pos);
}

std::string path_join(const std::string &a, const std::string &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string strip_archive_ext(const std::string &name)
{
    std::string s = name;
    auto dot = s.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        const std::string ext = s.substr(dot + 1);
        const bool numeric = !ext.empty() && ext.find_first_not_of("0123456789") == std::string::npos;
        s = s.substr(0, dot);
        if (numeric) {  // name.7z.001 -> name
            dot = s.rfind('.');
            if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
        }
    }
    return s.empty() ? name : s;
}

std::string home_dir() { return g_get_home_dir(); }

namespace {
std::mutex g_temp_mutex;
std::vector<std::string> g_temp_dirs;
}  // namespace

std::string make_temp_dir(const std::string &tag)
{
    const char *base = g_get_user_runtime_dir();
    if (!base || !g_file_test(base, G_FILE_TEST_IS_DIR)) base = g_get_tmp_dir();
    std::string root = path_join(base, "7zipl");
    g_mkdir_with_parents(root.c_str(), 0700);
    std::string tmpl = path_join(root, tag + "-XXXXXX");
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!g_mkdtemp_full(buf.data(), 0700)) return {};
    std::string dir = buf.data();
    std::lock_guard<std::mutex> lock(g_temp_mutex);
    g_temp_dirs.push_back(dir);
    return dir;
}

void remove_tree(const std::string &path)
{
    if (path.empty() || path == "/") return;
    GStatBuf st;
    if (g_lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(path.c_str(), 0, nullptr);
        if (d) {
            const char *n;
            while ((n = g_dir_read_name(d))) remove_tree(path_join(path, n));
            g_dir_close(d);
        }
        g_rmdir(path.c_str());
    } else {
        g_unlink(path.c_str());
    }
}

void cleanup_temp_dirs()
{
    std::lock_guard<std::mutex> lock(g_temp_mutex);
    for (const auto &d : g_temp_dirs) remove_tree(d);
    g_temp_dirs.clear();
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string &s)
{
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_args(const std::string &s)
{
    std::vector<std::string> out;
    gint argc = 0;
    gchar **argv = nullptr;
    if (!trim(s).empty() && g_shell_parse_argv(s.c_str(), &argc, &argv, nullptr)) {
        for (int i = 0; i < argc; i++) out.emplace_back(argv[i]);
        g_strfreev(argv);
    }
    return out;
}

bool mask_match(const std::string &mask, const std::string &name)
{
    char *m = g_utf8_casefold(mask.c_str(), -1);
    char *n = g_utf8_casefold(name.c_str(), -1);
    const bool ok = g_pattern_match_simple(m, n);
    g_free(m);
    g_free(n);
    return ok;
}

}  // namespace szl
