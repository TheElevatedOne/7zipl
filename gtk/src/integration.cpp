// SPDX-License-Identifier: LGPL-2.1-or-later
#include "integration.hpp"

#include "settings.hpp"
#include "util.hpp"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <set>

namespace szl {

namespace {

const char *const kDesktopId = SZL_APP_ID ".desktop";
const char *const kMarker = "X-7zipl-Generated=true";

struct RawFormat {
    const char *name;
    const char *exts;
    const char *mimes;
    bool default_on;
};

// MIME types per format, legacy aliases included (the desktop entry lists both; associations use the
// canonical name this system's shared-mime-info has).
const RawFormat kFormats[] = {
    {"7z", "7z", "application/x-7z-compressed", true},
    {"zip", "zip zipx", "application/zip application/x-zip-compressed", true},
    {"rar", "rar", "application/vnd.rar application/x-rar application/x-rar-compressed", true},
    {"tar", "tar", "application/x-tar", true},
    {"gzip", "gz tgz", "application/gzip application/x-gzip application/x-compressed-tar", true},
    {"bzip2", "bz2 tbz2",
     "application/x-bzip2 application/x-bzip application/x-bzip2-compressed-tar application/x-bzip-compressed-tar", true},
    {"xz", "xz txz", "application/x-xz application/x-xz-compressed-tar", true},
    {"lzma", "lzma", "application/x-lzma application/x-lzma-compressed-tar", true},
    {"zstd", "zst tzst", "application/zstd application/x-zstd application/x-zstd-compressed-tar", true},
    {"Z", "Z", "application/x-compress application/x-tarz", true},
    {"ISO", "iso", "application/vnd.efi.iso application/x-cd-image application/x-iso9660-image", true},
    {"wim", "wim swm esd", "application/x-ms-wim", true},
    {"cab", "cab", "application/vnd.ms-cab-compressed", true},
    {"rpm", "rpm", "application/x-rpm application/x-source-rpm", true},
    {"deb", "deb", "application/vnd.debian.binary-package application/x-deb", true},
    {"cpio", "cpio", "application/x-cpio", true},
    {"arj", "arj", "application/x-arj", true},
    {"lzh", "lzh lha", "application/x-lha application/x-lzh-compressed", true},
    {"xar", "xar pkg", "application/x-xar", true},
    {"dmg", "dmg", "application/x-apple-diskimage", true},
    {"squashfs", "squashfs sqfs sfs", "application/vnd.squashfs application/x-squashfs", true},
    {"img", "img", "application/vnd.efi.img application/x-raw-disk-image", false},
    {"vhd", "vhd vhdx", "application/x-vhd-disk application/x-vhdx-disk", false},
    {"vmdk", "vmdk", "application/x-vmdk-disk", false},
    {"qcow", "qcow qcow2", "application/x-qemu-disk", false},
    {"vdi", "vdi", "application/x-vdi-disk application/x-virtualbox-vdi", false},
    {"msi", "msi", "application/x-msi", false},
    {"chm", "chm", "application/vnd.ms-htmlhelp", false},
    {"ar", "a ar", "application/x-archive", false},
};

bool mime_known(const std::string &mime)
{
    const std::string rel = "mime/" + mime + ".xml";
    if (g_file_test(path_join(g_get_user_data_dir(), rel).c_str(), G_FILE_TEST_EXISTS)) return true;
    for (const char *const *d = g_get_system_data_dirs(); *d; d++)
        if (g_file_test(path_join(*d, rel).c_str(), G_FILE_TEST_EXISTS)) return true;
    return false;
}

std::string canonical_mime(const std::string &mime)
{
    char *ct = g_content_type_from_mime_type(mime.c_str());
    std::string out = ct ? ct : mime;
    g_free(ct);
    return out;
}

bool write_text(const std::string &path, const std::string &text, int mode, std::string &error)
{
    g_mkdir_with_parents(path_dirname(path).c_str(), 0755);
    GError *e = nullptr;
    if (!g_file_set_contents(path.c_str(), text.data(), (gssize)text.size(), &e)) {
        error = e ? e->message : "write error";
        if (e) g_error_free(e);
        return false;
    }
    g_chmod(path.c_str(), mode);
    return true;
}

bool file_has_marker(const std::string &path)
{
    char *data = nullptr;
    if (!g_file_get_contents(path.c_str(), &data, nullptr, nullptr)) return false;
    const bool ok = strstr(data, kMarker) != nullptr || strstr(data, "7zipl context menu") != nullptr;
    g_free(data);
    return ok;
}

// Quoting for Exec= lines of desktop entries.
std::string desktop_quote(const std::string &s)
{
    if (s.find_first_of(" \t\n\"'\\$`<>~|&;*?#()") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') out += "\\\\";
        out += c;
    }
    return out + "\"";
}

std::string shell_quote(const std::string &s)
{
    char *q = g_shell_quote(s.c_str());
    std::string out = q;
    g_free(q);
    return out;
}

void update_desktop_database(const std::string &dir)
{
    char *prog = g_find_program_in_path("update-desktop-database");
    if (!prog) return;
    const char *argv[] = {prog, "-q", dir.c_str(), nullptr};
    g_spawn_sync(nullptr, const_cast<char **>(argv), nullptr, (GSpawnFlags)(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    g_free(prog);
}

std::string user_entry_path() { return path_join(g_get_user_data_dir(), std::string("applications/") + kDesktopId); }

GAppInfo *our_app_info()
{
    GDesktopAppInfo *a = g_desktop_app_info_new(kDesktopId);
    if (!a && g_file_test(user_entry_path().c_str(), G_FILE_TEST_EXISTS))
        a = g_desktop_app_info_new_from_filename(user_entry_path().c_str());
    return a ? G_APP_INFO(a) : nullptr;
}

// ---- context menu entries ----

struct Item {
    unsigned bit;
    const char *id;
    const char *label;
    const char *args;   // 7zipl arguments before the files
    bool archives;      // only for archives (else any file or folder)
    const char *icon;
};
const Item kItems[] = {
    {CTX_OPEN, "open", "Open archive", "", true, SZL_APP_ID},
    {CTX_EXTRACT, "extract", "Extract files...", "--extract", true, "archive-extract"},
    {CTX_EXTRACT_HERE, "extract-here", "Extract Here", "--extract-here", true, "archive-extract"},
    {CTX_EXTRACT_TO, "extract-to", "Extract to folder", "--extract-to-folder", true, "archive-extract"},
    {CTX_TEST, "test", "Test archive", "--test", true, "emblem-ok-symbolic"},
    {CTX_COMPRESS, "compress", "Add to archive...", "--compress", false, "archive-insert"},
    {CTX_COMPRESS_7Z, "compress-7z", "Add to .7z", "--compress-to=7z", false, "archive-insert"},
    {CTX_COMPRESS_ZIP, "compress-zip", "Add to .zip", "--compress-to=zip", false, "archive-insert"},
    {CTX_HASH, "hash", "CRC SHA", "--hash=*", false, "accessories-calculator"},
};

std::string archive_mimes_list()
{
    std::string s;
    for (const auto &m : desktop_mime_types()) s += m + ";";
    return s;
}

std::string archive_patterns()
{
    std::string s;
    for (const RawFormat &f : kFormats)
        for (const auto &e : split(f.exts, ' ')) s += "*." + e + ";";
    return s + "*.7z.001;*.zip.001;*.part1.rar;";
}

std::string exec_line(const Item &it, bool desktop_style, const char *files)
{
    std::string exe = desktop_style ? desktop_quote(self_exe()) : shell_quote(self_exe());
    std::string a = exe;
    if (*it.args) a += std::string(" ") + it.args;
    return a + " " + files;
}

// Dolphin / Konqueror: KIO service menus (KF6: ~/.local/share/kio/servicemenus, must be executable).
std::string kio_dir() { return path_join(g_get_user_data_dir(), "kio/servicemenus"); }

bool dolphin_set(unsigned items, bool cascaded, std::string &error)
{
    const std::string dir = kio_dir();
    for (const char *n : {"7zipl-archives.desktop", "7zipl-files.desktop"}) {
        const std::string p = path_join(dir, n);
        if (file_has_marker(p)) g_unlink(p.c_str());
    }
    if (!items) return true;
    for (int pass = 0; pass < 2; pass++) {
        const bool arch = pass == 0;
        std::string actions, body;
        for (const Item &it : kItems) {
            if (!(items & it.bit) || it.archives != arch) continue;
            actions += std::string(it.id) + ";";
            body += std::string("\n[Desktop Action ") + it.id + "]\nName=" + it.label + "\nIcon=" + it.icon +
                    "\nExec=" + exec_line(it, true, "%F") + "\n";
        }
        if (actions.empty()) continue;
        std::string text = "[Desktop Entry]\nType=Service\nX-KDE-ServiceTypes=KonqPopupMenu/Plugin\n";
        text += std::string("MimeType=") + (arch ? archive_mimes_list() : "all/allfiles;inode/directory;") + "\n";
        text += "Actions=" + actions + "\n";
        if (cascaded) text += "X-KDE-Submenu=7-Zip\nX-KDE-Submenu[x-test]=7-Zip\n";
        text += std::string("Icon=") + SZL_APP_ID + "\n" + kMarker + "\n" + body;
        if (!write_text(path_join(dir, arch ? "7zipl-archives.desktop" : "7zipl-files.desktop"), text, 0755, error))
            return false;
    }
    return true;
}

// Nautilus: executable scripts in ~/.local/share/nautilus/scripts (a "7-Zip" folder when cascaded).
std::string nautilus_dir() { return path_join(g_get_user_data_dir(), "nautilus/scripts"); }

void remove_marked_in(const std::string &dir)
{
    GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return;
    const char *n;
    std::vector<std::string> del;
    while ((n = g_dir_read_name(d))) {
        const std::string p = path_join(dir, n);
        if (!g_file_test(p.c_str(), G_FILE_TEST_IS_DIR) && file_has_marker(p)) del.push_back(p);
    }
    g_dir_close(d);
    for (const auto &p : del) g_unlink(p.c_str());
}

bool nautilus_set(unsigned items, bool cascaded, std::string &error)
{
    const std::string base = nautilus_dir(), sub = path_join(base, "7-Zip");
    remove_marked_in(base);
    remove_marked_in(sub);
    g_rmdir(sub.c_str());  // only when empty
    if (!items) return true;
    for (const Item &it : kItems) {
        if (!(items & it.bit)) continue;
        std::string text = "#!/bin/sh\n# 7zipl context menu entry (Tools > Options > 7-Zip removes it)\n";
        text += "set -f\nIFS='\n'\n";
        text += "exec " + exec_line(it, false, "$NAUTILUS_SCRIPT_SELECTED_FILE_PATHS") + "\n";
        const std::string p = cascaded ? path_join(sub, it.label) : path_join(base, std::string("7-Zip: ") + it.label);
        if (!write_text(p, text, 0755, error)) return false;
    }
    return true;
}

// Nemo: ~/.local/share/nemo/actions/*.nemo_action
std::string nemo_dir() { return path_join(g_get_user_data_dir(), "nemo/actions"); }

bool nemo_set(unsigned items, bool, std::string &error)
{
    remove_marked_in(nemo_dir());
    if (!items) return true;
    for (const Item &it : kItems) {
        if (!(items & it.bit)) continue;
        std::string text = "[Nemo Action]\n";
        text += std::string("Name=7-Zip: ") + it.label + "\nComment=" + it.label + "\nIcon-Name=" + it.icon + "\n";
        text += "Exec=" + exec_line(it, true, "%F") + "\nSelection=notnone\n";
        text += it.archives ? "Mimetypes=" + archive_mimes_list() + "\n" : std::string("Extensions=any;dir;\n");
        text += std::string(kMarker) + "\n";
        if (!write_text(path_join(nemo_dir(), std::string("7zipl-") + it.id + ".nemo_action"), text, 0644, error))
            return false;
    }
    return true;
}

// PCManFM-Qt, PCManFM, Caja: file-manager actions (~/.local/share/file-manager/actions).
std::string fma_dir() { return path_join(g_get_user_data_dir(), "file-manager/actions"); }

bool fma_set(unsigned items, bool cascaded, std::string &error)
{
    remove_marked_in(fma_dir());
    if (!items) return true;
    std::string list;
    for (const Item &it : kItems) {
        if (!(items & it.bit)) continue;
        const std::string id = std::string("7zipl-") + it.id;
        list += id + ";";
        std::string text = "[Desktop Entry]\nType=Action\n";
        text += std::string("Name=") + (cascaded ? "" : "7-Zip: ") + it.label + "\nIcon=" + it.icon + "\n";
        text += "Profiles=p;\n" + std::string(kMarker) + "\n\n[X-Action-Profile p]\n";
        text += it.archives ? "MimeTypes=" + archive_mimes_list() + "\n" : std::string("MimeTypes=all/all;\n");
        text += "Exec=" + exec_line(it, true, "%F") + "\nSelectionCount=>0\n";
        if (!write_text(path_join(fma_dir(), id + ".desktop"), text, 0644, error)) return false;
    }
    if (cascaded) {
        std::string menu = "[Desktop Entry]\nType=Menu\nName=7-Zip\nIcon=" SZL_APP_ID "\nItemsList=" + list + "\n" +
                           kMarker + "\n";
        if (!write_text(path_join(fma_dir(), "7zipl-menu.desktop"), menu, 0644, error)) return false;
    }
    return true;
}

// Beluga: groups "[Action 7zipl-…]" in ~/.config/beluga/actions.conf (it reads them on start).
std::string beluga_conf() { return path_join(g_get_user_config_dir(), "beluga/actions.conf"); }

// Beluga renumbers its groups ("Action 003") when its own actions editor saves, so 7zipl's entries are
// recognised by their command too.
bool beluga_group_is_ours(GKeyFile *k, const char *g)
{
    if (g_str_has_prefix(g, "Action 7zipl-")) return true;
    if (!g_str_has_prefix(g, "Action ")) return false;
    char *c = g_key_file_get_string(k, g, "Command", nullptr);
    if (!c) return false;
    const std::string cmd = c;
    g_free(c);
    const std::string exe = shell_quote(self_exe());
    if (cmd.rfind(exe + " ", 0) != 0) return false;
    for (const Item &it : kItems)
        if (cmd == exec_line(it, false, "%F")) return true;
    return false;
}

bool beluga_installed()
{
    GKeyFile *k = g_key_file_new();
    bool found = false;
    if (g_key_file_load_from_file(k, beluga_conf().c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr)) {
        gsize n = 0;
        char **groups = g_key_file_get_groups(k, &n);
        for (gsize i = 0; i < n; i++)
            if (beluga_group_is_ours(k, groups[i])) found = true;
        g_strfreev(groups);
    }
    g_key_file_free(k);
    return found;
}

bool beluga_set(unsigned items, bool cascaded, std::string &error)
{
    const std::string conf = beluga_conf();
    if (!g_file_test(conf.c_str(), G_FILE_TEST_EXISTS) && !items) return true;
    GKeyFile *k = g_key_file_new();
    g_key_file_load_from_file(k, conf.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    gsize n = 0;
    char **groups = g_key_file_get_groups(k, &n);
    for (gsize i = 0; i < n; i++)
        if (beluga_group_is_ours(k, groups[i])) g_key_file_remove_group(k, groups[i], nullptr);
    g_strfreev(groups);
    for (const Item &it : kItems) {
        if (!(items & it.bit)) continue;
        const std::string g = std::string("Action 7zipl-") + it.id;
        g_key_file_set_string(k, g.c_str(), "Name", (std::string(cascaded ? "7-Zip: " : "") + it.label).c_str());
        g_key_file_set_string(k, g.c_str(), "Icon", it.icon);
        g_key_file_set_string(k, g.c_str(), "Command", exec_line(it, false, "%F").c_str());
        g_key_file_set_string(k, g.c_str(), "Patterns", it.archives ? archive_patterns().c_str() : "");
        g_key_file_set_string(k, g.c_str(), "MimeTypes", "");
        g_key_file_set_boolean(k, g.c_str(), "Files", TRUE);
        g_key_file_set_boolean(k, g.c_str(), "Folders", !it.archives);
        g_key_file_set_boolean(k, g.c_str(), "Multiple", TRUE);
        g_key_file_set_boolean(k, g.c_str(), "Terminal", FALSE);
    }
    g_mkdir_with_parents(path_dirname(conf).c_str(), 0700);
    GError *e = nullptr;
    const bool ok = g_key_file_save_to_file(k, conf.c_str(), &e);
    if (!ok) {
        error = e ? e->message : "write error";
        if (e) g_error_free(e);
    }
    g_key_file_free(k);
    return ok;
}

bool any_marked_in(const std::string &dir)
{
    GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return false;
    const char *n;
    bool found = false;
    while (!found && (n = g_dir_read_name(d))) {
        const std::string p = path_join(dir, n);
        if (!g_file_test(p.c_str(), G_FILE_TEST_IS_DIR) && file_has_marker(p)) found = true;
    }
    g_dir_close(d);
    return found;
}

bool have_program(const char *name)
{
    char *p = g_find_program_in_path(name);
    g_free(p);
    return p != nullptr;
}

}  // namespace

std::string self_exe()
{
    char *p = g_file_read_link("/proc/self/exe", nullptr);
    std::string s = p ? p : "7zipl";
    g_free(p);
    return s;
}

const std::vector<AssocFormat> &assoc_formats()
{
    static std::vector<AssocFormat> out;
    if (!out.empty()) return out;
    for (const RawFormat &r : kFormats) {
        AssocFormat f;
        f.name = r.name;
        f.extensions = split(r.exts, ' ');
        f.default_on = r.default_on;
        std::set<std::string> seen;
        for (const auto &m : split(r.mimes, ' ')) {
            if (!mime_known(m)) {
                // An alias: use what it points to, when that is known.
                const std::string c = canonical_mime(m);
                if (c == m || !mime_known(c)) continue;
                if (seen.insert(c).second) f.mime_types.push_back(c);
                continue;
            }
            if (seen.insert(m).second) f.mime_types.push_back(m);
        }
        if (!f.mime_types.empty()) out.push_back(std::move(f));
    }
    return out;
}

std::vector<std::string> desktop_mime_types()
{
    std::vector<std::string> out;
    for (const RawFormat &r : kFormats)
        for (const auto &m : split(r.mimes, ' ')) out.push_back(m);
    return out;
}

const char *ctx_item_label(unsigned item)
{
    for (const Item &it : kItems)
        if (it.bit == item) return it.label;
    return "";
}

EntryState desktop_entry_state()
{
    for (const char *const *d = g_get_system_data_dirs(); *d; d++)
        if (g_file_test(path_join(*d, std::string("applications/") + kDesktopId).c_str(), G_FILE_TEST_EXISTS))
            return EntryState::System;
    if (g_file_test(user_entry_path().c_str(), G_FILE_TEST_EXISTS)) return EntryState::User;
    return EntryState::Missing;
}

std::string desktop_entry_path()
{
    GAppInfo *a = our_app_info();
    if (!a) return {};
    const char *f = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(a));
    std::string s = f ? f : "";
    g_object_unref(a);
    return s;
}

bool install_user_entry(std::string &error)
{
    std::string mimes;
    for (const auto &m : desktop_mime_types()) mimes += m + ";";
    std::string text =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=7zipl\n"
        "GenericName=Archive Manager\n"
        "Comment=Browse, create and extract 7z, zip, tar, rar and other archives\n"
        "Exec=" + desktop_quote(self_exe()) + " %f\n"
        "TryExec=" + desktop_quote(self_exe()) + "\n"
        "Icon=" SZL_APP_ID "\n"
        "Terminal=false\n"
        "StartupNotify=true\n"
        "StartupWMClass=7zipl\n"
        "Categories=GTK;Utility;Archiving;Compression;\n"
        "Keywords=7z;7-Zip;zip;rar;tar;iso;archive;extract;compress;unpack;\n"
        "MimeType=" + mimes + "\n"
        "Actions=new-window;\n" + std::string(kMarker) + "\n\n"
        "[Desktop Action new-window]\nName=New Window\nExec=" + desktop_quote(self_exe()) + "\n";
    const std::string path = user_entry_path();
    // GIO prefers entries in the user's folder, so the new entry would silently become the default for every
    // type it lists that has no explicit default. Keep today's programs: pin them first.
    for (const auto &f : assoc_formats())
        for (const auto &m : f.mime_types) {
            GAppInfo *a = g_app_info_get_default_for_type(m.c_str(), FALSE);
            if (a && g_strcmp0(g_app_info_get_id(a), kDesktopId) != 0) g_app_info_set_as_default_for_type(a, m.c_str(), nullptr);
            if (a) g_object_unref(a);
        }
    if (!write_text(path, text, 0644, error)) return false;
    // The icon, from the copy built into the binary.
    GBytes *icon = g_resources_lookup_data("/org/sevenzipl/SevenZipL/icons/scalable/apps/" SZL_APP_ID ".svg",
                                           G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (icon) {
        gsize len = 0;
        const char *data = (const char *)g_bytes_get_data(icon, &len);
        std::string e;
        write_text(path_join(g_get_user_data_dir(), "icons/hicolor/scalable/apps/" SZL_APP_ID ".svg"), std::string(data, len),
                   0644, e);
        g_bytes_unref(icon);
    }
    update_desktop_database(path_dirname(path));
    return true;
}

bool remove_user_entry()
{
    const std::string path = user_entry_path();
    if (!file_has_marker(path)) return false;
    // Associations that point to the entry would dangle: take them back first.
    for (const auto &f : assoc_formats())
        if (is_default_for(f)) {
            std::string e;
            set_default(f, false, e);
        }
    g_unlink(path.c_str());
    g_unlink(path_join(g_get_user_data_dir(), "icons/hicolor/scalable/apps/" SZL_APP_ID ".svg").c_str());
    update_desktop_database(path_dirname(path));
    return true;
}

bool is_default_for(const AssocFormat &f)
{
    for (const auto &m : f.mime_types) {
        GAppInfo *a = g_app_info_get_default_for_type(m.c_str(), FALSE);
        const bool ours = a && g_strcmp0(g_app_info_get_id(a), kDesktopId) == 0;
        if (a) g_object_unref(a);
        if (!ours) return false;
    }
    return !f.mime_types.empty();
}

std::string current_default_name(const AssocFormat &f)
{
    if (f.mime_types.empty()) return {};
    GAppInfo *a = g_app_info_get_default_for_type(f.mime_types[0].c_str(), FALSE);
    if (!a) return {};
    std::string n = g_app_info_get_display_name(a);
    g_object_unref(a);
    return n;
}

bool set_default(const AssocFormat &f, bool on, std::string &error)
{
    GKeyFile *prev = g_key_file_new();
    const std::string prev_path = path_join(config_dir(), "associations.conf");
    g_key_file_load_from_file(prev, prev_path.c_str(), G_KEY_FILE_NONE, nullptr);
    auto save_prev = [&] {
        g_mkdir_with_parents(config_dir().c_str(), 0700);
        g_key_file_save_to_file(prev, prev_path.c_str(), nullptr);
        g_key_file_free(prev);
    };
    if (!on) {
        // Back to the program that had the type before (like 7-Zip on Windows), else to the system's choice.
        for (const auto &m : f.mime_types) {
            GAppInfo *a = g_app_info_get_default_for_type(m.c_str(), FALSE);
            const bool ours = a && g_strcmp0(g_app_info_get_id(a), kDesktopId) == 0;
            if (a) g_object_unref(a);
            if (!ours) continue;
            g_app_info_reset_type_associations(m.c_str());
            char *old = g_key_file_get_string(prev, "Previous", m.c_str(), nullptr);
            GDesktopAppInfo *oa = old && *old ? g_desktop_app_info_new(old) : nullptr;
            if (oa) {
                g_app_info_set_as_default_for_type(G_APP_INFO(oa), m.c_str(), nullptr);
                g_object_unref(oa);
            }
            g_free(old);
            g_key_file_remove_key(prev, "Previous", m.c_str(), nullptr);
            // Still 7zipl (it ranks first when nothing is set): give it to the next recommended program.
            GAppInfo *now = g_app_info_get_default_for_type(m.c_str(), FALSE);
            if (now && g_strcmp0(g_app_info_get_id(now), kDesktopId) == 0) {
                GList *rec = g_app_info_get_recommended_for_type(m.c_str());
                for (GList *l = rec; l; l = l->next)
                    if (g_strcmp0(g_app_info_get_id(G_APP_INFO(l->data)), kDesktopId) != 0) {
                        g_app_info_set_as_default_for_type(G_APP_INFO(l->data), m.c_str(), nullptr);
                        break;
                    }
                g_list_free_full(rec, g_object_unref);
            }
            if (now) g_object_unref(now);
        }
        save_prev();
        return true;
    }
    for (const auto &m : f.mime_types) {
        GAppInfo *a = g_app_info_get_default_for_type(m.c_str(), FALSE);
        if (a && g_strcmp0(g_app_info_get_id(a), kDesktopId) != 0 && g_app_info_get_id(a))
            g_key_file_set_string(prev, "Previous", m.c_str(), g_app_info_get_id(a));
        if (a) g_object_unref(a);
    }
    save_prev();
    GAppInfo *app = our_app_info();
    if (!app) {
        error = "7zipl's desktop entry isn't installed";
        return false;
    }
    bool ok = true;
    for (const auto &m : f.mime_types) {
        GError *e = nullptr;
        if (!g_app_info_set_as_default_for_type(app, m.c_str(), &e)) {
            ok = false;
            error = e ? e->message : "error";
        }
        if (e) g_error_free(e);
    }
    g_object_unref(app);
    return ok;
}

std::vector<FileManager> file_managers()
{
    std::vector<FileManager> out;
    out.push_back({"dolphin", "Dolphin / Konqueror", have_program("dolphin") || have_program("konqueror"),
                   file_has_marker(path_join(kio_dir(), "7zipl-archives.desktop")) ||
                       file_has_marker(path_join(kio_dir(), "7zipl-files.desktop"))});
    out.push_back({"nautilus", "Files (Nautilus)", have_program("nautilus"),
                   any_marked_in(nautilus_dir()) || any_marked_in(path_join(nautilus_dir(), "7-Zip"))});
    out.push_back({"nemo", "Nemo", have_program("nemo"), any_marked_in(nemo_dir())});
    out.push_back({"fma", "PCManFM-Qt / PCManFM / Caja",
                   have_program("pcmanfm-qt") || have_program("pcmanfm") || have_program("caja"), any_marked_in(fma_dir())});
    out.push_back({"beluga", "Beluga",
                   have_program("beluga") || g_file_test(path_dirname(beluga_conf()).c_str(), G_FILE_TEST_IS_DIR),
                   beluga_installed()});
    return out;
}

bool set_context_menu(const std::string &fm, unsigned items, bool cascaded, std::string &error)
{
    if (fm == "dolphin") return dolphin_set(items, cascaded, error);
    if (fm == "nautilus") return nautilus_set(items, cascaded, error);
    if (fm == "nemo") return nemo_set(items, cascaded, error);
    if (fm == "fma") return fma_set(items, cascaded, error);
    if (fm == "beluga") return beluga_set(items, cascaded, error);
    error = "unknown file manager " + fm;
    return false;
}

}  // namespace szl
