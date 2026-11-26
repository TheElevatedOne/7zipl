// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: desktop integration, the Linux way of 7-Zip's Options > System and Options > 7-Zip pages.
//  - File associations: the XDG default application per MIME type (~/.config/mimeapps.list), set through GIO,
//    for the current user (7-Zip's "Current user" column; "All users" would need root and isn't offered).
//  - The desktop entry and icon, installed for the current user when 7zipl isn't installed system-wide.
//  - Context-menu entries for file managers: Dolphin/Konqueror (KIO service menus), Nautilus (scripts),
//    Nemo (actions), PCManFM-Qt/PCManFM/Caja (file-manager actions) and Beluga (custom actions).
#pragma once

#include <string>
#include <vector>

namespace szl {

struct AssocFormat {
    std::string name;                     // "7z", "ISO", ...
    std::vector<std::string> extensions;  // without dots
    std::vector<std::string> mime_types;  // canonical, known to this system's shared-mime-info
    bool default_on = true;               // part of "select all" (not for formats usually opened elsewhere)
};

// The formats 7zipl can be the default for, with MIME types this system knows.
const std::vector<AssocFormat> &assoc_formats();
// Every MIME type 7zipl opens (also aliases), as listed in its desktop entry.
std::vector<std::string> desktop_mime_types();

// Desktop entry
enum class EntryState { System, User, Missing };
EntryState desktop_entry_state();
std::string desktop_entry_path();  // where GIO found it, or ""
// Writes ~/.local/share/applications/<app id>.desktop (Exec = this binary) and the icon.
bool install_user_entry(std::string &error);
bool remove_user_entry();

// Associations
bool is_default_for(const AssocFormat &f);                    // 7zipl is the default for all its MIME types
std::string current_default_name(const AssocFormat &f);       // the default app's name, "" if none
bool set_default(const AssocFormat &f, bool on, std::string &error);

// Context menus
enum CtxItem : unsigned {
    CTX_OPEN = 1u << 0,
    CTX_EXTRACT = 1u << 1,
    CTX_EXTRACT_HERE = 1u << 2,
    CTX_EXTRACT_TO = 1u << 3,
    CTX_TEST = 1u << 4,
    CTX_COMPRESS = 1u << 5,
    CTX_COMPRESS_7Z = 1u << 6,
    CTX_COMPRESS_ZIP = 1u << 7,
    CTX_HASH = 1u << 8,
};
constexpr unsigned kCtxDefault = 0x1FF;
const char *ctx_item_label(unsigned item);

struct FileManager {
    std::string id;       // "dolphin", "nautilus", "nemo", "fma", "beluga"
    std::string name;     // "Dolphin / Konqueror"
    bool detected;        // the program (or its config) is on this system
    bool installed;       // 7zipl's entries are there
};
std::vector<FileManager> file_managers();
// Writes (or removes, when items == 0) 7zipl's entries for one file manager.
bool set_context_menu(const std::string &fm, unsigned items, bool cascaded, std::string &error);

// The running binary, for Exec lines.
std::string self_exe();

}  // namespace szl
