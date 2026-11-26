// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: small non-GTK helpers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace szl {

// "1 234 567" like 7-Zip File Manager's size columns.
std::string format_number(uint64_t v);
// "12 MiB" style short size for progress and status lines.
std::string format_size_short(uint64_t v);
// "2026-11-26 13:45:10" (local time), empty for 0 when `has` is false.
std::string format_time(int64_t unix_seconds);
std::string format_duration(uint64_t seconds);

std::string path_basename(const std::string &p);
std::string path_dirname(const std::string &p);
std::string path_join(const std::string &a, const std::string &b);
// Name without the last extension ("a.tar.gz" -> "a.tar"); multi-volume "a.7z.001" -> "a".
std::string strip_archive_ext(const std::string &name);
std::string home_dir();
// A fresh private directory below $XDG_RUNTIME_DIR (or /tmp) for temporary extraction; removed on exit.
// A new private folder in $XDG_RUNTIME_DIR/7zipl ("<tag>-<pid>-XXXXXX"), removed by cleanup_temp_dirs().
std::string make_temp_dir(const std::string &tag);
std::string temp_root();
void remove_tree(const std::string &path);
void cleanup_temp_dirs();

std::vector<std::string> split(const std::string &s, char sep);
std::string trim(const std::string &s);
// Splits a "Parameters" field ("x=9 mt=4") into words, honouring quotes.
std::vector<std::string> split_args(const std::string &s);
// Wildcard match for Select by mask ("*.txt"), case-insensitive.
bool mask_match(const std::string &mask, const std::string &name);

}  // namespace szl
