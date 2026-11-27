// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: runs the system's 7-Zip (7zz / 7z from the distribution's package) and turns its console
// output into the engine API. Passwords and overwrite answers go to 7-Zip's prompts on stdin, never on
// the command line.
#include "engine.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace szl {

namespace {

std::string g_prog;     // the 7-Zip binary
std::string g_version;  // "26.06"
std::vector<FormatInfo> g_formats;
std::vector<std::string> g_hashers;
uint64_t g_memory_limit = 0;
bool g_inited = false;

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

bool starts_with(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

std::vector<std::string> split_ws(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

uint64_t to_u64(const std::string &s)
{
    const std::string t = trim(s);
    return t.empty() ? 0 : g_ascii_strtoull(t.c_str(), nullptr, 10);
}

// "2026-11-27 00:11:35.5774881" (local time, as 7-Zip prints it) to unix seconds.
bool parse_time(const std::string &s, int64_t &out)
{
    int y, mo, d, h, mi, se;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    tm.tm_isdst = -1;
    out = (int64_t)std::mktime(&tm);
    return true;
}

// The full path of the first program found on PATH.
std::string find_program()
{
    if (const char *e = g_getenv("SZL_7Z"); e && *e) {
        char *p = g_find_program_in_path(e);
        std::string s = p ? p : "";
        g_free(p);
        return s;
    }
    // 7zz is always Igor Pavlov's 7-Zip (Debian/Ubuntu's 7zip package); 7z is 7-Zip on Arch and Fedora.
    for (const char *n : {"7zz", "7z", "7za"}) {
        char *p = g_find_program_in_path(n);
        if (p) {
            std::string s = p;
            g_free(p);
            return s;
        }
    }
    return {};
}

bool is_update_command(const std::string &c) { return c == "a" || c == "u" || c == "d" || c == "rn"; }
bool is_extract_command(const std::string &c) { return c == "x" || c == "e" || c == "t" || c == "l"; }

// ---------- One 7-Zip process ----------

struct Proc {
    Ui &ui;
    Result &res;
    std::string archive;        // for the password prompt
    std::string password;       // answer to password prompts
    bool have_password = false;
    bool password_asked = false;  // the UI was asked
    uint64_t total = 0;         // bytes, when known: turns percentages into byte counts
    std::function<void(const std::string &)> on_line;  // each line of standard output
    std::function<void(const std::string &)> on_text;  // raw standard output (benchmark)

    std::vector<std::string> lines;  // all output lines
    int exit_code = -1;
    bool cancelled = false;
    bool failed_to_start = false;
    uint64_t files_seen = 0;
    uint64_t mem_required = 0, mem_allowed = 0;  // "Memory usage limit was exceeded"

    Proc(Ui &u, Result &r) : ui(u), res(r) {}

    // Parse state for messages
    enum class Block { None, Errors, Warnings, ScanWarnings };
    Block block = Block::None;
    bool next_is_error = false;
    std::string pending_error;  // "ERROR: message" may be followed by the file name on its own line
    bool pending_is_warning = false;
    // Overwrite prompt
    int ow_section = 0;
    std::string ow_path[2];
    uint64_t ow_size[2] = {0, 0};
    int64_t ow_time[2] = {0, 0};

    // "errno=2 : No such file or directory" -> "No such file or directory"
    static std::string clean(std::string s)
    {
        for (size_t k; (k = s.find("errno=")) != std::string::npos;) {
            const auto c = s.find(" : ", k);
            if (c == std::string::npos) break;
            s.erase(k, c + 3 - k);
        }
        return s;
    }
    void error(std::string s)
    {
        s = clean(s);
        if (s.empty()) return;
        for (const auto &m : res.messages)
            if (m == s || (m.size() > s.size() && m.compare(m.size() - s.size(), s.size(), s) == 0)) return;
        res.errors++;
        res.messages.push_back(s);
        ui.message(Ui::Level::Error, s);
    }
    void warning(std::string s)
    {
        s = clean(s);
        if (s.empty()) return;
        for (const auto &m : res.messages)
            if (m == s || (m.size() > s.size() && m.compare(m.size() - s.size(), s.size(), s) == 0)) return;
        res.warnings++;
        res.messages.push_back(s);
        ui.message(Ui::Level::Warning, s);
    }

    void line(const std::string &raw)
    {
        const std::string l = trim(raw);
        lines.push_back(raw);
        if (on_line) on_line(raw);

        // Overwrite prompt details
        if (l == "Would you like to replace the existing file:") ow_section = 1;
        else if (l == "with the file from archive:") ow_section = 2;
        else if (ow_section && starts_with(l, "Path:")) ow_path[ow_section - 1] = trim(l.substr(5));
        else if (ow_section && starts_with(l, "Size:")) ow_size[ow_section - 1] = to_u64(l.substr(5));
        else if (ow_section && starts_with(l, "Modified:")) parse_time(trim(l.substr(9)), ow_time[ow_section - 1]);

        if (!pending_error.empty()) {
            // "ERROR: message" then the file name: reported as "name : message" (7-Zip's own order).
            std::string e = std::move(pending_error);
            const bool warn = pending_is_warning;
            pending_error.clear();
            if (!l.empty() && l.find(" = ") == std::string::npos && l.find(": ") == std::string::npos) {
                // "ERROR: a.7z" + "a.7z": the "Open ERROR" line that follows explains it
                if (l != e) warn ? warning(l + " : " + e) : error(l + " : " + e);
                return;
            }
            warn ? warning(e) : error(e);
        }
        if (next_is_error) {
            next_is_error = false;
            error(l);
            return;
        }
        if (block != Block::None) {
            if (block == Block::ScanWarnings) {
                if (starts_with(l, "----")) block = Block::None;
                else if (!l.empty()) warning(l);
                return;
            }
            if (l.empty()) {
                block = Block::None;
                return;
            }
            if (block == Block::Errors) error(l);
            else warning(l);
            return;
        }
        if (l == "ERROR:") next_is_error = true;
        else if (starts_with(l, "ERROR: ")) {
            if (l != "ERROR: Memory usage limit was exceeded.") {
                pending_error = l.substr(7);
                pending_is_warning = false;
            }
        } else if (starts_with(l, "Open ERROR: ")) error(l.substr(12));
        else if (starts_with(l, "WARNING: ")) {
            pending_error = l.substr(9);
            pending_is_warning = true;
        }
        else if (starts_with(l, "Open WARNING: ")) warning(l.substr(14));
        else if (l == "ERRORS:") block = Block::Errors;
        else if (l == "WARNINGS:") block = Block::Warnings;
        else if (l == "Scan WARNINGS for files and folders:") block = Block::ScanWarnings;
        else if (l == "System ERROR:" || l == "Command Line Error:") next_is_error = true;
        else if (starts_with(l, "Command Line Error: ")) error(l);
        else if (l == "Break signaled") cancelled = true;
        else if (l.find(" GB : required memory usage size") != std::string::npos) mem_required = to_u64(l) << 30;
        else if (l.find(" GB : allowed memory usage limit") != std::string::npos) mem_allowed = to_u64(l) << 30;
    }

    // One segment of the progress line: " 45% 12 - dir/file.txt" or "  0M Scan".
    void progress(const std::string &seg)
    {
        const std::string s = trim(seg);
        if (s.empty()) return;
        size_t i = 0;
        while (i < s.size() && g_ascii_isdigit(s[i])) i++;
        if (i == 0) return;
        const unsigned pct = (unsigned)to_u64(s.substr(0, i));
        const bool is_pct = i < s.size() && s[i] == '%';
        auto words = split_ws(s.substr(i + (i < s.size() ? 1 : 0)));
        size_t w = 0;
        if (w < words.size() && g_ascii_isdigit(words[w][0])) {
            const uint64_t f = to_u64(words[w++]);
            while (files_seen < f) {
                files_seen++;
                ui.file_done();
            }
        }
        if (is_pct) {
            if (total) ui.set_completed(total * std::min(pct, 100u) / 100);
            else ui.set_percent(std::min(pct, 100u));
        }
        if (w < words.size()) {
            const std::string cmd = words[w++];
            const char *what = cmd == "+" ? "Compressing" : cmd == "-" ? "Extracting" : cmd == "T" ? "Testing"
                             : cmd == "U" ? "Updating" : cmd == "D" ? "Deleting" : cmd == "R" ? "Renaming"
                             : cmd == "Scan" ? "Scanning" : cmd == "=" ? "Copying" : "Processing";
            const auto pos = s.find(cmd, i);
            std::string name = pos == std::string::npos ? "" : trim(s.substr(pos + cmd.size()));
            ui.set_current(what, name);
        }
    }

    // Partial line at the end of standard output: 7-Zip waits for an answer.
    // Returns the answer to write, or "" when it isn't a prompt.
    std::string prompt(const std::string &tail)
    {
        if (tail.find("Enter password") != std::string::npos || tail.find("Verify password") != std::string::npos) {
            if (!have_password) {
                password_asked = true;
                std::string pw;
                if (!ui.ask_password(archive, pw)) {
                    cancelled = true;
                    return "\x03";  // stop the process
                }
                password = pw;
                have_password = true;
            }
            return password + "\n";
        }
        if (tail.find("(Q)uit?") != std::string::npos) {
            ow_section = 0;
            switch (ui.ask_overwrite(ow_path[0], ow_size[0], ow_time[0], ow_path[1], ow_size[1], ow_time[1])) {
            case Ui::Overwrite::Yes: return "y\n";
            case Ui::Overwrite::YesAll: return "a\n";
            case Ui::Overwrite::No: return "n\n";
            case Ui::Overwrite::NoAll: return "s\n";
            case Ui::Overwrite::Rename: return "u\n";
            case Ui::Overwrite::Cancel:
                cancelled = true;
                return "q\n";
            }
        }
        return {};
    }

    void run(const std::vector<std::string> &args)
    {
        if (g_prog.empty()) {
            failed_to_start = true;
            error("7-Zip was not found. Install your distribution's 7zip package (it provides 7z or 7zz).");
            return;
        }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(g_prog.c_str()));
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        gchar **env = g_get_environ();
        env = g_environ_setenv(env, "LC_ALL", "C.UTF-8", TRUE);  // English messages, UTF-8 names
        GPid pid = 0;
        int in = -1, out = -1, err = -1;
        GError *e = nullptr;
        const bool ok = g_spawn_async_with_pipes(nullptr, argv.data(), env, G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr,
                                                 &pid, &in, &out, &err, &e);
        g_strfreev(env);
        if (!ok) {
            failed_to_start = true;
            error(std::string("Cannot run ") + g_prog + ": " + (e ? e->message : "unknown error"));
            if (e) g_error_free(e);
            return;
        }
        fcntl(out, F_SETFL, O_NONBLOCK);
        fcntl(err, F_SETFL, O_NONBLOCK);
        std::string obuf, ebuf;
        bool out_open = true, err_open = true;
        bool sent_int = false;
        auto int_time = std::chrono::steady_clock::now();
        char buf[16384];
        while (out_open || err_open) {
            pollfd p[2];
            int n = 0;
            if (out_open) p[n++] = {out, POLLIN, 0};
            if (err_open) p[n++] = {err, POLLIN, 0};
            poll(p, (nfds_t)n, 100);
            // stdout: lines, and prompts at the end of an unfinished line
            while (out_open) {
                const ssize_t r = read(out, buf, sizeof buf);
                if (r > 0) {
                    if (on_text) on_text(std::string(buf, (size_t)r));
                    obuf.append(buf, (size_t)r);
                    size_t nl;
                    while ((nl = obuf.find('\n')) != std::string::npos) {
                        line(obuf.substr(0, nl));
                        obuf.erase(0, nl + 1);
                    }
                } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                    out_open = false;
                } else {
                    break;
                }
            }
            if (!obuf.empty() && in >= 0 && !sent_int) {
                const std::string answer = prompt(obuf);
                if (!answer.empty()) {
                    line(obuf);
                    obuf.clear();
                    if (answer == "\x03") {
                        kill(pid, SIGINT);
                        sent_int = true;
                        int_time = std::chrono::steady_clock::now();
                    } else if (write(in, answer.data(), answer.size()) < 0) {
                        kill(pid, SIGINT);
                    }
                }
            }
            // stderr: the progress line, redrawn with backspaces
            while (err_open) {
                const ssize_t r = read(err, buf, sizeof buf);
                if (r > 0) {
                    ebuf.append(buf, (size_t)r);
                } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                    err_open = false;
                } else {
                    break;
                }
            }
            if (!ebuf.empty()) {
                // Segments end at a backspace, CR or newline; the last one may still be incomplete.
                size_t start = 0;
                for (size_t k = 0; k < ebuf.size(); k++) {
                    const char c = ebuf[k];
                    if (c == '\b' || c == '\r' || c == '\n') {
                        if (k > start) progress(ebuf.substr(start, k - start));
                        start = k + 1;
                    }
                }
                ebuf.erase(0, start);
                if (!ebuf.empty() && ebuf.back() == ' ') {
                    // complete enough; wait for the next backspace
                }
            }
            // Pause stops the process; cancel interrupts it (7-Zip removes its temporary files).
            if (ui.paused()) {
                kill(pid, SIGSTOP);
                const bool c = ui.cancelled();  // blocks while paused
                kill(pid, SIGCONT);
                if (c) cancelled = true;
            } else if (ui.cancelled()) {
                cancelled = true;
            }
            if (cancelled && !sent_int) {
                kill(pid, SIGINT);
                sent_int = true;
                int_time = std::chrono::steady_clock::now();
            } else if (sent_int && std::chrono::steady_clock::now() - int_time > std::chrono::seconds(5)) {
                kill(pid, SIGKILL);
            }
        }
        if (!obuf.empty()) line(obuf);
        if (!pending_error.empty()) pending_is_warning ? warning(pending_error) : error(pending_error);
        if (in >= 0) close(in);
        close(out);
        close(err);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        g_spawn_close_pid(pid);
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
        if (exit_code == 255) cancelled = true;
    }
};

// Builds 7-Zip's argument list: the command, our output switches, then the caller's arguments.
// A password switch "-pSECRET" is taken out; the password goes to 7-Zip's prompt instead.
std::vector<std::string> build_args(const std::vector<std::string> &args, Proc &p)
{
    std::vector<std::string> out;
    if (args.empty()) return out;
    const std::string &cmd = args[0];
    out.push_back(cmd);
    for (const char *s : {"-bso1", "-bse1", "-bsp2", "-sccUTF-8", "-scsUTF-8"}) out.emplace_back(s);
    bool after_dashes = false, archive_seen = false, wants_password = false;
    bool has_memx = false;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string &a = args[i];
        if (!after_dashes && a == "--") {
            after_dashes = true;
            continue;
        }
        if (!after_dashes && a.size() >= 2 && a[0] == '-' && a[1] == 'p' && cmd != "b") {
            wants_password = true;
            if (a.size() > 2) {
                p.password = a.substr(2);
                p.have_password = true;
            }
            continue;
        }
        if (!after_dashes && starts_with(a, "-smemx")) has_memx = true;
        if ((after_dashes || a.empty() || a[0] != '-') && !archive_seen) {
            archive_seen = true;
            p.archive = a;
        }
    }
    // Creating an encrypted archive needs "-p" (7-Zip then asks); extracting asks when it has to.
    if (wants_password && is_update_command(cmd)) out.emplace_back("-p");
    if (!has_memx && g_memory_limit && is_extract_command(cmd) && cmd != "l")
        out.push_back("-smemx" + std::to_string((g_memory_limit + (1ull << 30) - 1) >> 30) + "g");
    for (size_t i = 1; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "--") {
            out.push_back(a);
            out.insert(out.end(), args.begin() + (long)i + 1, args.end());
            break;
        }
        if (a.size() >= 2 && a[0] == '-' && a[1] == 'p' && cmd != "b") continue;
        out.push_back(a);
    }
    return out;
}

void finish(Result &r, const Proc &p)
{
    if (p.cancelled) {
        r.cancelled = true;
        r.ok = false;
        r.summary = "Operation was cancelled";
        return;
    }
    // 7-Zip's exit codes: 0 ok, 1 warning, 2 fatal error, 7 command line error, 8 not enough memory.
    if (p.exit_code >= 2 && r.errors == 0) {
        r.errors++;
        const std::string m = p.exit_code == 7 ? "Command line error"
                            : p.exit_code == 8 ? "Not enough memory"
                                               : "7-Zip reported an error (exit code " + std::to_string(p.exit_code) + ")";
        r.messages.push_back(m);
    }
    r.ok = r.errors == 0 && p.exit_code >= 0 && p.exit_code <= 1;
    if (r.ok) r.summary = r.warnings ? "Finished with warnings" : "Everything is Ok";
    else r.summary = "There are some errors";
}

// "Add new data to archive: 2 folders, 5 files, 1234 bytes (2 KiB)"
void parse_counts(const std::string &s, uint64_t &folders, uint64_t &files, uint64_t &bytes)
{
    const auto w = split_ws(s);
    for (size_t i = 1; i < w.size(); i++) {
        const std::string unit = w[i];
        if (!g_ascii_isdigit(w[i - 1][0])) continue;
        const uint64_t v = to_u64(w[i - 1]);
        if (starts_with(unit, "folder")) folders = v;
        else if (starts_with(unit, "file")) files = v;
        else if (starts_with(unit, "byte")) bytes = v;
    }
}

// Totals at the end of the output.
void parse_stats(Result &r, const std::vector<std::string> &lines)
{
    for (const auto &raw : lines) {
        const std::string l = trim(raw);
        auto value = [&](const char *key) { return to_u64(l.substr(strlen(key))); };
        if (starts_with(l, "Folders: ")) r.folders = value("Folders: ");
        else if (starts_with(l, "Files: ")) r.files = value("Files: ");
        else if (starts_with(l, "Size: ")) r.size = value("Size: ");
        else if (starts_with(l, "Compressed: ")) r.packed = value("Compressed: ");
        else if (starts_with(l, "Files read from disk: ")) r.files = value("Files read from disk: ");
        else if (starts_with(l, "Archive size: ")) r.packed = value("Archive size: ");
        else if (starts_with(l, "Add new data to archive: ")) {
            uint64_t fo = 0, fi = 0, by = 0;
            parse_counts(l.substr(25), fo, fi, by);
            r.folders = fo;
            r.size = by;
        }
    }
}

// The table of "7z h": digest columns, size, name, between two lines of dashes.
void parse_hash(Result &r, const std::vector<std::string> &lines)
{
    HashResult &h = r.hash;
    size_t i = 0;
    for (; i + 1 < lines.size(); i++)
        if (starts_with(lines[i + 1], "--------") && lines[i + 1].find(' ') != std::string::npos &&
            lines[i].find("Name") != std::string::npos)
            break;
    if (i + 1 >= lines.size()) return;
    const std::string &dash = lines[i + 1];
    std::vector<std::pair<size_t, size_t>> cols;  // [start, end)
    for (size_t k = 0; k < dash.size();) {
        if (dash[k] != '-') {
            k++;
            continue;
        }
        size_t e = k;
        while (e < dash.size() && dash[e] == '-') e++;
        cols.emplace_back(k, e);
        k = e;
    }
    if (cols.size() < 3) return;
    const size_t n_methods = cols.size() - 2;
    h.methods.clear();
    const auto head = split_ws(lines[i]);
    for (size_t m = 0; m < n_methods && m < head.size(); m++) h.methods.push_back(head[m]);
    auto col = [](const std::string &s, std::pair<size_t, size_t> c) {
        return c.first < s.size() ? trim(s.substr(c.first, c.second - c.first)) : std::string();
    };
    for (size_t k = i + 2; k < lines.size() && !starts_with(lines[k], "--------"); k++) {
        const std::string &l = lines[k];
        if (trim(l).empty()) continue;
        HashLine hl;
        hl.name = cols.back().first < l.size() ? l.substr(cols.back().first) : "";
        hl.size = to_u64(col(l, cols[n_methods]));
        for (size_t m = 0; m < n_methods; m++) hl.digests.push_back(col(l, cols[m]));
        hl.dir = std::all_of(hl.digests.begin(), hl.digests.end(), [](const std::string &d) { return d.empty(); }) &&
                 !hl.name.empty() && hl.name.back() == '/';
        if (hl.dir) {
            hl.name.pop_back();
            hl.digests.clear();
        }
        h.files.push_back(std::move(hl));
    }
    h.totals.assign(h.methods.size(), "");
    for (const auto &raw : lines) {
        const std::string l = trim(raw);
        const auto pos = l.find(" for data:");
        if (pos == std::string::npos) continue;
        const std::string m = trim(l.substr(0, pos));
        for (size_t k = 0; k < h.methods.size(); k++)
            if (h.methods[k] == m) h.totals[k] = trim(l.substr(pos + 10));
        if (starts_with(l, "Folders: ")) h.num_dirs = to_u64(l.substr(9));
    }
    for (const auto &raw : lines) {
        const std::string l = trim(raw);
        if (starts_with(l, "Folders: ")) h.num_dirs = to_u64(l.substr(9));
        else if (starts_with(l, "Files: ")) h.num_files = to_u64(l.substr(7));
        else if (starts_with(l, "Size: ")) h.total_size = to_u64(l.substr(6));
    }
    if (!h.num_files)
        h.num_files = (uint64_t)std::count_if(h.files.begin(), h.files.end(), [](const HashLine &x) { return !x.dir; });
    h.errors = r.errors;
}

// Moves everything inside `src` into `dst`, merging folders and asking before replacing files.
// Returns false when the user cancelled.
bool merge_move(const std::string &src, const std::string &dst, Ui &ui, Result &r, int &all)
{
    GDir *d = g_dir_open(src.c_str(), 0, nullptr);
    if (!d) return true;
    std::vector<std::string> names;
    const char *n;
    while ((n = g_dir_read_name(d))) names.emplace_back(n);
    g_dir_close(d);
    g_mkdir_with_parents(dst.c_str(), 0755);
    for (const auto &name : names) {
        const std::string s = std::string(src) + "/" + name, t = std::string(dst) + "/" + name;
        GStatBuf ss, ts;
        if (g_lstat(s.c_str(), &ss) != 0) continue;
        const bool exists = g_lstat(t.c_str(), &ts) == 0;
        if (exists && S_ISDIR(ss.st_mode) && S_ISDIR(ts.st_mode)) {
            if (!merge_move(s, t, ui, r, all)) return false;
            g_rmdir(s.c_str());
            continue;
        }
        std::string target = t;
        if (exists) {
            int answer = all;
            if (!answer) {
                switch (ui.ask_overwrite(t, (uint64_t)ts.st_size, ts.st_mtime, s, (uint64_t)ss.st_size, ss.st_mtime)) {
                case Ui::Overwrite::Yes: answer = 1; break;
                case Ui::Overwrite::YesAll: answer = all = 1; break;
                case Ui::Overwrite::No: answer = 2; break;
                case Ui::Overwrite::NoAll: answer = all = 2; break;
                case Ui::Overwrite::Rename: answer = 3; break;
                case Ui::Overwrite::Cancel: return false;
                }
            }
            if (answer == 2) continue;
            if (answer == 3) {
                const auto dot = name.rfind('.');
                const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
                const std::string ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
                for (int i = 1;; i++) {
                    target = dst + "/" + stem + "_" + std::to_string(i) + ext;
                    if (!g_file_test(target.c_str(), G_FILE_TEST_EXISTS)) break;
                }
            } else if (S_ISDIR(ts.st_mode)) {
                r.errors++;
                r.messages.push_back("Cannot replace the folder " + t + " with a file");
                continue;
            } else {
                g_unlink(t.c_str());
            }
        }
        if (g_rename(s.c_str(), target.c_str()) != 0) {
            r.errors++;
            r.messages.push_back("Cannot move " + s + " to " + target + ": " + g_strerror(errno));
        }
    }
    return true;
}

void remove_tree(const std::string &p)
{
    GStatBuf st;
    if (g_lstat(p.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (GDir *d = g_dir_open(p.c_str(), 0, nullptr)) {
            const char *n;
            while ((n = g_dir_read_name(d))) remove_tree(p + "/" + n);
            g_dir_close(d);
        }
        g_rmdir(p.c_str());
    } else {
        g_unlink(p.c_str());
    }
}

// Runs a command; when 7-Zip stops at the memory limit, asks the UI and runs it again with the higher limit.
Result run_with(const std::vector<std::string> &args, Ui &ui, uint64_t total,
                const std::function<void(Proc &)> &setup = {})
{
    std::vector<std::string> a = args;
    for (int attempt = 0;; attempt++) {
        Result r;
        Proc p(ui, r);
        p.total = total;
        if (setup) setup(p);
        p.run(build_args(a, p));
        if (p.mem_required && !p.cancelled && attempt == 0 &&
            ui.ask_memory(p.mem_required, p.mem_allowed ? p.mem_allowed : g_memory_limit, p.archive)) {
            std::vector<std::string> b{a[0], "-smemx" + std::to_string(p.mem_required >> 30) + "g"};
            b.insert(b.end(), a.begin() + 1, a.end());
            a = b;
            continue;
        }
        if (p.mem_required && !p.cancelled)
            p.error("The archive needs " + std::to_string(p.mem_required >> 30) +
                    " GB of memory to unpack, more than the limit of " +
                    std::to_string((p.mem_allowed ? p.mem_allowed : g_memory_limit) >> 30) + " GB");
        parse_stats(r, p.lines);
        if (!args.empty() && args[0] == "h") parse_hash(r, p.lines);
        if (r.size && r.packed) ui.set_ratio(r.size, r.packed);
        finish(r, p);
        return r;
    }
}

}  // namespace

void set_memory_limit(uint64_t bytes) { g_memory_limit = bytes; }

void init(const char *)
{
    if (g_inited) return;
    g_inited = true;
    g_prog = find_program();
    if (g_prog.empty()) return;
    gchar *out = nullptr;
    gchar **env = g_get_environ();
    env = g_environ_setenv(env, "LC_ALL", "C.UTF-8", TRUE);
    const char *argv[] = {g_prog.c_str(), "i", "-sccUTF-8", nullptr};
    const bool ok = g_spawn_sync(nullptr, const_cast<char **>(argv), env, G_SPAWN_STDERR_TO_DEV_NULL, nullptr, nullptr,
                                 &out, nullptr, nullptr, nullptr);
    g_strfreev(env);
    if (!ok || !out) return;
    std::vector<std::string> lines;
    {
        gchar **ls = g_strsplit(out, "\n", -1);
        for (gchar **l = ls; *l; l++) lines.emplace_back(*l);
        g_strfreev(ls);
    }
    g_free(out);
    // "7-Zip 26.06 (x64) : Copyright ...", "7-Zip (a) 24.09 ..."
    for (const auto &l : lines) {
        if (!starts_with(l, "7-Zip")) continue;
        for (const auto &w : split_ws(l))
            if (g_ascii_isdigit(w[0]) && w.find('.') != std::string::npos) {
                g_version = w;
                break;
            }
        break;
    }
    enum { None, Formats, Hashers } sec = None;
    for (const auto &raw : lines) {
        const std::string l = trim(raw);
        if (l == "Formats:") {
            sec = Formats;
            continue;
        }
        if (l == "Hashers:") {
            sec = Hashers;
            continue;
        }
        if (l.empty() || (l.back() == ':' && l.find(' ') == std::string::npos)) {
            sec = l.empty() ? sec : None;
            continue;
        }
        if (sec == Hashers) {
            const auto w = split_ws(l);
            if (w.size() >= 3 && std::find(g_hashers.begin(), g_hashers.end(), w.back()) == g_hashers.end())
                g_hashers.push_back(w.back());
        } else if (sec == Formats) {
            // " 0 C...F..........c.a.m+.. w...0  7z       7z            7 z BC AF ' 1C"
            // library index, flags (C = can create), optional time flags, name padded to 8, extensions padded
            // to 13, signature.
            size_t i = 0;
            while (i < raw.size() && (raw[i] == ' ' || g_ascii_isdigit(raw[i]))) i++;
            if (i >= raw.size()) continue;
            FormatInfo f;
            f.can_update = raw[i] == 'C';
            size_t j = raw.find(' ', i);  // end of the flags
            if (j == std::string::npos) continue;
            size_t k = j;
            while (k < raw.size() && raw[k] == ' ') k++;
            // time flags: "w...0"
            size_t e = raw.find(' ', k);
            if (e != std::string::npos && e - k == 5 && std::strchr("w.", raw[k]) && g_ascii_isdigit(raw[k + 4])) {
                k = e;
                while (k < raw.size() && raw[k] == ' ') k++;
            }
            e = raw.find(' ', k);
            f.name = raw.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (e == std::string::npos) {
                g_formats.push_back(f);
                continue;
            }
            const size_t ext_start = k + std::max<size_t>(8, f.name.size()) + 1;
            std::string exts = ext_start < raw.size() ? raw.substr(ext_start) : "";
            // The field is 13 wide unless longer; the signature follows.
            const auto words = split_ws(exts);
            size_t used = 0;
            for (const auto &w : words) {
                const size_t at = exts.find(w, used);
                const bool in_field = at < 13;
                const bool ext_like = w.size() >= 2 && std::all_of(w.begin(), w.end(), [](char c) {
                                          return g_ascii_islower(c) || g_ascii_isdigit(c) || c == '_';
                                      });
                if (!in_field && !ext_like) break;
                if (!in_field && at > used + 1) break;  // a gap of two spaces ends the field
                if (w.front() == '(') {
                    used = at + w.size();
                    continue;  // "tgz (tar)"
                }
                if (w.find('=') != std::string::npos) break;
                f.extensions.push_back(w);
                used = at + w.size();
            }
            g_formats.push_back(std::move(f));
        }
    }
}

bool available() { return !g_prog.empty(); }
std::string program() { return g_prog; }
std::string version() { return g_version; }
std::vector<FormatInfo> formats() { return g_formats; }
std::vector<std::string> hash_methods() { return g_hashers; }

std::vector<std::string> update_formats()
{
    // The Add to Archive dialog's list (7-Zip FM shows the formats that can create archives).
    static const char *const order[] = {"7z", "zip", "tar", "wim", "gzip", "bzip2", "xz", "zstd"};
    std::vector<std::string> out;
    for (const char *n : order)
        for (const auto &f : g_formats)
            if (f.can_update && f.name == n) out.push_back(f.name);
    return out;
}

bool looks_like_archive(const std::string &path)
{
    const auto slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string ext = g_ascii_strdown(name.substr(dot + 1).c_str(), -1);
    for (const auto &f : g_formats)
        for (const auto &e : f.extensions)
            if (e == ext) return true;
    // Multi-volume archives: name.7z.001
    if (ext.size() == 3 && g_ascii_isdigit(ext[0]) && g_ascii_isdigit(ext[1]) && g_ascii_isdigit(ext[2]))
        return name.size() > 4;
    return false;
}

ArchiveListing open_archive(const std::string &path, Ui &ui, const std::string &password, const std::string &type)
{
    ArchiveListing out;
    out.path = path;
    Result r;
    Proc p(ui, r);
    if (!password.empty()) {
        p.password = password;
        p.have_password = true;
    }
    std::vector<std::string> args{"l", "-slt"};
    if (!type.empty()) args.push_back("-t" + type);
    args.push_back("--");
    args.push_back(path);
    ui.set_current("Opening", path);
    p.run(build_args(args, p));
    if (p.cancelled) {
        out.cancelled = true;
        out.error = "Operation was cancelled";
        return out;
    }

    // Archive blocks ("--" then "Key = value" lines) before "----------", then one block per item.
    std::vector<std::vector<std::pair<std::string, std::string>>> levels;
    bool in_items = false;
    Entry cur;
    bool have_cur = false;
    uint32_t index = 0;
    auto flush = [&] {
        if (!have_cur) return;
        cur.index = index++;
        out.entries.push_back(std::move(cur));
        cur = Entry();
        have_cur = false;
    };
    for (const auto &raw : p.lines) {
        if (raw == "--" && !in_items) {
            levels.emplace_back();
            continue;
        }
        if (raw == "----------") {
            in_items = true;
            continue;
        }
        const auto eq = raw.find(" = ");
        const bool kv = eq != std::string::npos && eq > 0 && raw[0] != ' ';
        std::string key = kv ? raw.substr(0, eq) : "", val = kv ? raw.substr(eq + 3) : "";
        if (!kv && !raw.empty() && raw.back() == '=' && raw.size() > 2 && raw[raw.size() - 2] == ' ') {
            key = raw.substr(0, raw.size() - 2);  // "Key =" with an empty value
            val.clear();
        }
        if (!in_items) {
            if (!key.empty() && !levels.empty()) levels.back().emplace_back(key, val);
            continue;
        }
        if (key.empty()) {
            if (raw.empty()) flush();
            continue;
        }
        if (key == "Path") {
            flush();
            cur.path = val;
            have_cur = true;
            continue;
        }
        if (!have_cur) continue;
        if (key == "Folder") cur.dir = val == "+";
        else if (key == "Size" && !val.empty()) {
            cur.size = to_u64(val);
            cur.has_size = true;
        } else if (key == "Packed Size" && !val.empty()) {
            cur.packed = to_u64(val);
            cur.has_packed = true;
        } else if (key == "Modified") cur.has_mtime = parse_time(val, cur.mtime);
        else if (key == "Created") cur.has_ctime = parse_time(val, cur.ctime);
        else if (key == "Accessed") cur.has_atime = parse_time(val, cur.atime);
        else if (key == "CRC" && !val.empty()) {
            cur.crc = (uint32_t)g_ascii_strtoull(val.c_str(), nullptr, 16);
            cur.has_crc = true;
        } else if (key == "Encrypted") cur.encrypted = val == "+";
        else if (key == "Attributes") {
            cur.attrib = val;
            if (!val.empty() && val[0] == 'D') cur.dir = true;
        } else if (key == "Mode") {
            if (cur.attrib.empty()) cur.attrib = val;
            if (!val.empty() && val[0] == 'd') cur.dir = true;
        } else if (key == "Method") cur.method = val;
        else if (key == "Comment") cur.comment = val;
        else if (key == "User") cur.user = val;
        else if (key == "Group") cur.group = val;
        else if (key == "Symbolic Link" && !val.empty()) cur.link = val;
        else if (key == "Hard Link" && !val.empty() && cur.link.empty()) cur.link = val;
    }
    flush();
    // "./dir/file" (tar made with "tar -C dir .") and "dir/" to "dir/file" and "dir"; drop the "." item itself.
    for (auto &e : out.entries) {
        while (starts_with(e.path, "./")) e.path.erase(0, 2);
        for (size_t k; (k = e.path.find("/./")) != std::string::npos;) e.path.erase(k, 2);
        while (!e.path.empty() && e.path.back() == '/') {
            e.path.pop_back();
            e.dir = true;
        }
    }
    out.entries.erase(std::remove_if(out.entries.begin(), out.entries.end(),
                                     [](const Entry &e) { return e.path.empty() || e.path == "."; }),
                      out.entries.end());

    if (levels.empty() || !in_items) {
        if (p.password_asked || (p.have_password && !password.empty()))
            for (const auto &m : r.messages)
                if (m.find("Wrong password") != std::string::npos) {
                    out.error = "Cannot open encrypted archive '" + path + "'. Wrong password?";
                    return out;
                }
        std::string msg;
        for (const auto &m : r.messages)
            if (m.find("Cannot open the file as") != std::string::npos || m == "Is not archive") {
                msg = "Cannot open the file '" + path + "' as archive";
                break;
            }
        if (msg.empty() && !r.messages.empty()) msg = r.messages.front();
        if (msg.empty()) msg = "Cannot open '" + path + "'";
        out.error = msg;
        return out;
    }

    for (size_t lv = 0; lv < levels.size(); lv++) {
        std::string lpath;
        for (const auto &kv : levels[lv])
            if (kv.first == "Path") lpath = kv.second;
        if (levels.size() > 1) out.props.emplace_back("--", lpath);
        for (const auto &kv : levels[lv])
            if (kv.first != "Path" && !kv.second.empty()) out.props.emplace_back(kv.first, kv.second);
    }
    for (const auto &kv : levels.back())
        if (kv.first == "Type") out.format = kv.second;
    uint64_t volumes = 0;
    for (const auto &kv : levels.back())
        if (kv.first == "Volumes") volumes = to_u64(kv.second);
    for (const auto &f : g_formats)
        if (f.name == out.format) out.can_update = f.can_update && levels.size() == 1 && volumes <= 1;
    for (const auto &m : r.messages) out.props.emplace_back("Warning", m);
    if (p.have_password) out.password = p.password;
    out.ok = true;
    return out;
}

Result run(const std::vector<std::string> &args, Ui &ui)
{
    if (args.empty()) {
        Result r;
        r.errors = 1;
        r.messages.push_back("No command");
        r.summary = "There are some errors";
        return r;
    }
    // For "a" and "u", 7-Zip prints the input size after scanning; use it as the total.
    return run_with(args, ui, 0, [&ui](Proc &p) {
        p.on_line = [&ui, &p](const std::string &l) {
            if (starts_with(l, "Add new data to archive: ")) {
                uint64_t fo = 0, fi = 0, by = 0;
                parse_counts(l.substr(25), fo, fi, by);
                p.total = by;
                ui.set_total(by);
                ui.set_num_files(fi);
            }
        };
    });
}

Result extract_items(const ExtractRequest &req, Ui &ui)
{
    const std::string cmd = req.test ? "t" : req.path_mode == 1 ? "e" : "x";
    // 7-Zip can't remove a path prefix, so items from inside a folder of the archive are extracted to a
    // staging folder next to the destination and moved out of it.
    const bool staging = !req.test && req.path_mode == 0 && !req.strip_prefix.empty();
    std::string out_dir = req.out_dir;
    if (staging) {
        g_mkdir_with_parents(req.out_dir.c_str(), 0755);
        std::string tmpl = req.out_dir + "/.7zipl-XXXXXX";
        std::vector<char> b(tmpl.begin(), tmpl.end());
        b.push_back('\0');
        if (!g_mkdtemp(b.data())) {
            Result r;
            r.errors = 1;
            r.messages.push_back("Cannot create a folder in " + req.out_dir + ": " + g_strerror(errno));
            r.summary = "There are some errors";
            return r;
        }
        out_dir = b.data();
    }
    std::vector<std::string> args{cmd, "-spd"};
    if (!req.test) {
        args.push_back("-o" + out_dir);
        if (staging) args.emplace_back("-aoa");
        else if (req.overwrite == 1) args.emplace_back("-aoa");
        else if (req.overwrite == 2) args.emplace_back("-aos");
        else if (req.overwrite == 3) args.emplace_back("-aou");
        else if (req.overwrite == 4) args.emplace_back("-aot");
        if (req.path_mode == 2) args.emplace_back("-spf");
    }
    if (!req.password.empty()) args.push_back("-p" + req.password);
    // Many items go through a list file (the command line has a length limit).
    std::string list_file;
    size_t len = 0;
    for (const auto &i : req.items) len += i.size() + 1;
    if (req.items.size() > 200 || len > 60000) {
        gchar *name = nullptr;
        const int fd = g_file_open_tmp("7zipl-list-XXXXXX", &name, nullptr);
        if (fd >= 0) {
            std::string data;
            for (const auto &i : req.items) data += i + "\n";
            if (write(fd, data.data(), data.size()) < 0) {
            }
            close(fd);
            list_file = name;
        }
        g_free(name);
    }
    args.emplace_back("--");
    args.push_back(req.archive);
    if (!list_file.empty()) args.push_back("@" + list_file);
    else args.insert(args.end(), req.items.begin(), req.items.end());
    Result r = run_with(args, ui, 0);
    if (!list_file.empty()) g_unlink(list_file.c_str());
    if (staging) {
        if (!r.cancelled) {
            int all = req.overwrite == 1 ? 1 : req.overwrite == 2 ? 2 : 0;
            const std::string from = out_dir + "/" + req.strip_prefix;
            if (!merge_move(g_file_test(from.c_str(), G_FILE_TEST_IS_DIR) ? from : out_dir, req.out_dir, ui, r, all)) {
                r.cancelled = true;
                r.ok = false;
                r.summary = "Operation was cancelled";
            } else if (r.errors && r.ok) {
                r.ok = false;
                r.summary = "There are some errors";
            }
        }
        remove_tree(out_dir);
    }
    return r;
}

Result benchmark(unsigned threads, unsigned dict_log, unsigned passes, Ui &ui,
                 const std::function<void(const std::string &)> &print)
{
    std::vector<std::string> args{"b"};
    if (passes) args.push_back(std::to_string(passes));
    if (threads) args.push_back("-mmt" + std::to_string(threads));
    if (dict_log) args.push_back("-md" + std::to_string(dict_log));
    Result r;
    Proc p(ui, r);
    p.on_text = print;
    p.run(build_args(args, p));
    finish(r, p);
    return r;
}

}  // namespace szl
