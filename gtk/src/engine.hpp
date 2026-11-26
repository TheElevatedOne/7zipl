// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the 7-Zip engine behind a small C++ API. This is the only part of the GUI that includes
// 7-Zip's headers; everything here runs in-process on 7-Zip's own UI/Common code (no 7zz child process).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace szl {

// Called from the worker thread that runs an operation. The GUI implements it (job.cpp).
class Ui {
public:
    virtual ~Ui() = default;
    enum class Overwrite { Yes, YesAll, No, NoAll, Rename, Cancel };
    enum class Level { Info, Warning, Error };

    virtual void set_total(uint64_t bytes) = 0;
    virtual void set_completed(uint64_t bytes) = 0;
    virtual void set_ratio(uint64_t in, uint64_t out) = 0;
    virtual void set_num_files(uint64_t files) = 0;
    virtual void file_done() = 0;
    virtual void set_current(const std::string &what, const std::string &name) = 0;
    // true when the user cancelled; blocks while the operation is paused.
    virtual bool cancelled() = 0;
    // Asks for a password. Returns false when the user cancels.
    virtual bool ask_password(const std::string &archive, std::string &password) = 0;
    virtual Overwrite ask_overwrite(const std::string &existing, uint64_t existing_size, int64_t existing_mtime,
                                    const std::string &incoming, uint64_t incoming_size, int64_t incoming_mtime) = 0;
    virtual void message(Level level, const std::string &text) = 0;
    // An archive needs more memory to unpack than the limit allows (Options > Settings). true = allow it.
    virtual bool ask_memory(uint64_t required, uint64_t limit, const std::string &archive)
    {
        (void)required;
        (void)limit;
        (void)archive;
        return false;
    }
};

struct Entry {
    uint32_t index = 0;   // item index in the archive
    std::string path;     // full path inside the archive, '/' separated
    bool dir = false;
    bool has_size = false, has_packed = false, has_mtime = false, has_ctime = false, has_atime = false;
    bool has_crc = false, encrypted = false;
    uint64_t size = 0, packed = 0;
    int64_t mtime = 0, ctime = 0, atime = 0;  // unix seconds
    uint32_t crc = 0;
    std::string attrib, method, comment, user, group, link;
};

struct ArchiveListing {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string path;     // the archive file
    std::string format;   // innermost format name ("7z", "zip", ...)
    bool can_update = false;
    std::vector<std::pair<std::string, std::string>> props;  // archive properties, for the Info dialog
    std::vector<Entry> entries;
    std::string password; // the password that opened it (encrypted headers), reused for later operations
};

struct FormatInfo {
    std::string name;
    std::vector<std::string> extensions;
    bool can_update = false;
};

struct HashLine {
    std::string name;
    uint64_t size = 0;
    bool dir = false;
    std::vector<std::string> digests;  // one per method
};
struct HashResult {
    std::vector<std::string> methods;
    std::vector<HashLine> files;
    std::vector<std::string> totals;   // "data" sum per method
    uint64_t num_files = 0, num_dirs = 0, total_size = 0, errors = 0;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    uint64_t errors = 0, warnings = 0;
    std::string summary;              // short text for the final dialog
    std::vector<std::string> messages; // error/warning lines
    uint64_t files = 0, folders = 0, size = 0, packed = 0;
    HashResult hash;
};

// Loads the codecs. Call once at startup.
void init(const char *argv0);
// Memory archives may use to unpack without asking; 0 = no limit.
void set_memory_limit(uint64_t bytes);

std::string version();           // "26.06"
std::vector<FormatInfo> formats();
std::vector<std::string> hash_methods();
// Formats that can be written, in the order 7-Zip's Add dialog lists them.
std::vector<std::string> update_formats();
// True when the file name has an extension of a known archive format (not a guarantee that it opens).
bool looks_like_archive(const std::string &path);

// Lists an archive. Asks Ui for a password when the headers are encrypted.
// `type`: "" = by signature (default), "*" = try every handler (Open Inside *), "#" = parser mode, which finds
// archives embedded in the file (Open Inside #).
ArchiveListing open_archive(const std::string &path, Ui &ui, const std::string &password = {},
                            const std::string &type = {});

// Runs a 7-Zip command line (without the program name), e.g. {"x", "a.7z", "-o/tmp/out"} or
// {"a", "-t7z", "-mx9", "new.7z", "file"}: x e t a u d rn h. The same switches as 7zz.
Result run(const std::vector<std::string> &args, Ui &ui);

// Extracts the given items (indices of an open archive listing) of `archive` to `out_dir`.
// `strip_prefix` (e.g. "docs/img") is removed from the item paths, the way 7-Zip FM extracts
// from inside a folder of an archive. path_mode: 0 full paths, 1 no paths, 2 absolute paths.
// overwrite: 0 ask, 1 overwrite, 2 skip, 3 rename new, 4 rename existing.
struct ExtractRequest {
    std::string archive;
    std::string out_dir;
    std::vector<std::string> items;   // full paths inside the archive; empty = everything
    std::string strip_prefix;
    int path_mode = 0;
    int overwrite = 0;
    bool test = false;
    std::string password;
};
Result extract_items(const ExtractRequest &req, Ui &ui);

// 7-Zip's benchmark (7z b): `threads` 0 = all, `dict_log` dictionary size as a power of two (0 = default),
// `passes` number of passes. `print` gets the report text as it is produced (worker thread).
Result benchmark(unsigned threads, unsigned dict_log, unsigned passes, Ui &ui,
                 const std::function<void(const std::string &)> &print);

}  // namespace szl
