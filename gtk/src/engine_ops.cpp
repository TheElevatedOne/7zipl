// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the operations behind szl::run() (7-Zip's command letters a u d rn x e t h, with the switches the
// GUI uses), done with 7z.so's archive handlers, hashers and codecs, and the benchmark.
#include "engine_impl.hpp"

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <map>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace szl {
namespace impl {
namespace {

static_assert(sizeof(Prop) == sizeof(PROPVARIANT), "Prop arrays are passed as PROPVARIANT arrays");

std::string cwd()
{
    gchar *c = g_get_current_dir();
    std::string s = c;
    g_free(c);
    return s;
}

std::string absolute(const std::string &p)
{
    gchar *c = g_canonicalize_filename(p.c_str(), nullptr);
    std::string s = c;
    g_free(c);
    return s;
}

// ---------------------------------------------------------------- command line

struct Options {
    std::string cmd;
    std::vector<std::string> pos;
    std::string type;
    std::vector<std::pair<std::string, std::string>> props;  // -m name=value (value may be empty)
    std::string password;
    bool password_set = false;
    std::string volume;
    std::string out_dir;
    bool out_set = false;
    int overwrite = 0;
    bool yes = false;
    int spf = 0;  // 0 relative names, 1 absolute (-spf), 2 full without the root (-spf2)
    bool snl = false, snh = false, sdel = false, spd = false, spe = false;
    std::vector<std::string> hash_methods;
    std::string work_dir;
    std::string update_switches;  // what follows -u
};

bool parse(const std::vector<std::string> &args, Options &o, std::string &error)
{
    if (args.empty()) {
        error = "No command";
        return false;
    }
    o.cmd = args[0];
    bool switches = true;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string &a = args[i];
        if (switches && a == "--") {
            switches = false;
            continue;
        }
        if (!switches || a.size() < 2 || a[0] != '-') {
            o.pos.push_back(a);
            continue;
        }
        const std::string s = a.substr(1);
        auto is = [&](const char *p) { return starts_with(s, p); };
        if (is("t")) o.type = s.substr(1);
        else if (is("m")) {
            const std::string m = s.substr(1);
            const auto eq = m.find('=');
            if (eq == std::string::npos) o.props.emplace_back(m, "");
            else o.props.emplace_back(m.substr(0, eq), m.substr(eq + 1));
        } else if (is("p")) {
            o.password = s.substr(1);
            o.password_set = true;
        } else if (is("v")) o.volume = s.substr(1);
        else if (is("o")) {
            o.out_dir = s.substr(1);
            o.out_set = true;
        } else if (s == "aoa") o.overwrite = 1;
        else if (s == "aos") o.overwrite = 2;
        else if (s == "aou") o.overwrite = 3;
        else if (s == "aot") o.overwrite = 4;
        else if (s == "y") o.yes = true;
        else if (s == "spf") o.spf = 1;
        else if (s == "spf2") o.spf = 2;
        else if (s == "snl") o.snl = true;
        else if (s == "snh") o.snh = true;
        else if (s == "sdel") o.sdel = true;
        else if (s == "spd") o.spd = true;
        else if (s == "spe") o.spe = true;
        else if (s == "ssw") {
        } else if (is("scrc")) o.hash_methods.push_back(s.substr(4));
        else if (is("w")) o.work_dir = s.substr(1);
        else if (is("u")) o.update_switches += s.substr(1);
        else {
            error = "Unsupported switch: " + a;
            return false;
        }
    }
    if (o.yes && o.overwrite == 0) o.overwrite = 1;
    return true;
}

// ---------------------------------------------------------------- files to add

struct NewItem {
    std::string disk;   // path on disk
    std::string name;   // path in the archive
    bool dir = false;
    struct stat st {};
    bool link = false;  // stored as a symbolic link
    std::string target; // link target
    std::string hard;   // archive name of the first item this one is a hard link to
};

class Scanner {
public:
    Scanner(Ui &ui, Result &r, const Options &o, bool links, bool hards, dev_t skip_dev, ino_t skip_ino)
        : ui_(ui), r_(r), o_(o), links_(links), hards_(hards), skip_dev_(skip_dev), skip_ino_(skip_ino) {}

    std::vector<NewItem> items;
    std::vector<std::string> roots;  // archive names of the given paths
    bool cancelled = false;

    void add_arg(const std::string &arg)
    {
        std::vector<std::string> paths;
        if (!o_.spd && arg.find_first_of("*?") != std::string::npos) {
            glob_t g;
            if (glob(arg.c_str(), GLOB_NOSORT, nullptr, &g) == 0)
                for (size_t i = 0; i < g.gl_pathc; i++) paths.emplace_back(g.gl_pathv[i]);
            globfree(&g);
            std::sort(paths.begin(), paths.end());
            if (paths.empty()) {
                add_warning(r_, ui_, arg + " : " + errno_text(ENOENT));
                return;
            }
        } else {
            paths.push_back(arg);
        }
        for (const auto &p : paths) add_path(p);
    }

private:
    Ui &ui_;
    Result &r_;
    const Options &o_;
    bool links_, hards_;
    dev_t skip_dev_;
    ino_t skip_ino_;
    std::map<std::pair<dev_t, ino_t>, std::string> seen_;
    std::vector<std::pair<dev_t, ino_t>> stack_;

    void add_path(const std::string &arg)
    {
        const std::string abs = absolute(arg);
        std::string name;
        if (o_.spf == 1) name = abs;
        else if (o_.spf == 2) name = abs.substr(1);
        else name = base_name(abs);
        if (name.empty() || name == "/") {
            add_warning(r_, ui_, arg + " : cannot add the root folder");
            return;
        }
        roots.push_back(name[0] == '/' ? name.substr(1) : name);
        walk(abs, name);
    }

    void walk(const std::string &disk, const std::string &name)
    {
        if (cancelled || (cancelled = ui_.cancelled())) return;
        NewItem it;
        it.disk = disk;
        it.name = name;
        const int rc = links_ ? lstat(disk.c_str(), &it.st) : stat(disk.c_str(), &it.st);
        if (rc != 0) {
            add_warning(r_, ui_, disk + " : " + errno_text(errno));
            return;
        }
        if (it.st.st_dev == skip_dev_ && it.st.st_ino == skip_ino_) return;  // the archive itself
        if (S_ISLNK(it.st.st_mode)) {
            char buf[4096];
            const ssize_t n = readlink(disk.c_str(), buf, sizeof buf);
            if (n < 0) {
                add_warning(r_, ui_, disk + " : " + errno_text(errno));
                return;
            }
            it.link = true;
            it.target.assign(buf, (size_t)n);
            items.push_back(std::move(it));
            return;
        }
        if (!S_ISDIR(it.st.st_mode)) {
            if (!S_ISREG(it.st.st_mode)) {
                add_warning(r_, ui_, disk + " : not a regular file, skipped");
                return;
            }
            if (hards_ && it.st.st_nlink > 1) {
                const auto key = std::make_pair(it.st.st_dev, it.st.st_ino);
                const auto f = seen_.find(key);
                if (f != seen_.end()) it.hard = f->second;
                else seen_[key] = name;
            }
            items.push_back(std::move(it));
            return;
        }
        const auto key = std::make_pair(it.st.st_dev, it.st.st_ino);
        if (std::find(stack_.begin(), stack_.end(), key) != stack_.end()) {
            add_warning(r_, ui_, disk + " : folder loop, skipped");
            return;
        }
        it.dir = true;
        items.push_back(it);
        DIR *d = opendir(disk.c_str());
        if (!d) {
            add_warning(r_, ui_, disk + " : " + errno_text(errno));
            return;
        }
        std::vector<std::string> names;
        while (dirent *e = readdir(d))
            if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        stack_.push_back(key);
        for (const auto &n : names) walk(disk == "/" ? "/" + n : disk + "/" + n, name + "/" + n);
        stack_.pop_back();
    }
};

// ---------------------------------------------------------------- streams for updating

class MemIn final : public ISequentialInStream {
    std::string data_;
    size_t pos_ = 0;
    SZ_COM_BEGIN SZ_COM_IF(ISequentialInStream) SZ_COM_END(ISequentialInStream)
    explicit MemIn(std::string d) : data_(std::move(d)) {}
    HRESULT Read(void *data, uint32_t size, uint32_t *processed) override
    {
        const size_t n = std::min<size_t>(size, data_.size() - pos_);
        std::memcpy(data, data_.data() + pos_, n);
        pos_ += n;
        if (processed) *processed = (uint32_t)n;
        return S_OK;
    }
};

// Writes name.001, name.002, ... of `vol` bytes each.
class MultiVolOut final : public IOutStream {
    std::string base_;
    uint64_t vol_, pos_ = 0, length_ = 0;
    std::vector<int> fds_;
    SZ_COM_BEGIN SZ_COM_IF(IOutStream) SZ_COM_IF(ISequentialOutStream) SZ_COM_END(IOutStream)
    std::vector<std::string> names;

    MultiVolOut(std::string base, uint64_t vol) : base_(std::move(base)), vol_(vol) {}
    ~MultiVolOut()
    {
        for (int fd : fds_)
            if (fd >= 0) ::close(fd);
    }
    std::string name(size_t i) const
    {
        char b[32];
        std::snprintf(b, sizeof b, ".%03zu", i + 1);
        return base_ + b;
    }
    HRESULT Write(const void *data, uint32_t size, uint32_t *processed) override
    {
        if (processed) *processed = 0;
        if (size == 0) return S_OK;
        const size_t idx = (size_t)(pos_ / vol_);
        const uint64_t off = pos_ % vol_;
        while (fds_.size() <= idx) {
            const std::string n = name(fds_.size());
            const int fd = ::open(n.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
            if (fd < 0) return hr_from_errno(errno);
            fds_.push_back(fd);
            names.push_back(n);
        }
        const uint32_t n = (uint32_t)std::min<uint64_t>(size, vol_ - off);
        const ssize_t w = pwrite(fds_[idx], data, n, (off_t)off);
        if (w < 0) return hr_from_errno(errno);
        pos_ += (uint64_t)w;
        length_ = std::max(length_, pos_);
        if (processed) *processed = (uint32_t)w;
        return S_OK;
    }
    HRESULT Seek(int64_t offset, uint32_t origin, uint64_t *newpos) override
    {
        const int64_t base = origin == 0 ? 0 : origin == 1 ? (int64_t)pos_ : (int64_t)length_;
        if (origin > 2 || base + offset < 0) return E_INVALIDARG;
        pos_ = (uint64_t)(base + offset);
        if (newpos) *newpos = pos_;
        return S_OK;
    }
    HRESULT SetSize(uint64_t size) override
    {
        for (size_t i = 0; i < fds_.size(); i++) {
            const uint64_t start = i * vol_;
            const uint64_t want = size <= start ? 0 : std::min<uint64_t>(vol_, size - start);
            if (ftruncate(fds_[i], (off_t)want) != 0) return hr_from_errno(errno);
        }
        length_ = size;
        return S_OK;
    }
    int close_all()
    {
        int err = 0;
        for (int &fd : fds_) {
            if (fd >= 0 && ::close(fd) != 0 && !err) err = errno;
            fd = -1;
        }
        // Volumes past the end (after SetSize) are removed.
        while (!names.empty() && length_ <= (names.size() - 1) * vol_ && names.size() > 1) {
            unlink(names.back().c_str());
            names.pop_back();
        }
        return err;
    }
    void remove_all()
    {
        close_all();
        for (const auto &n : names) unlink(n.c_str());
    }
};

// ---------------------------------------------------------------- the update callback

struct UpdItem {
    int32_t old_index = -1;  // item of the existing archive
    int new_index = -1;      // NewItem
    bool new_props = false;  // renamed
    std::string name;        // new name when renamed
};

void set_file_props(Prop &p, PROPID id, const NewItem &n, bool store_owner)
{
    const struct stat &st = n.st;
    switch (id) {
    case kpidPath: p.set_str(n.name); break;
    case kpidIsDir: p.set_bool(n.dir); break;
    case kpidSize:
        p.set_u64(n.dir || !n.hard.empty() ? 0 : n.link ? n.target.size() : (uint64_t)st.st_size);
        break;
    case kpidMTime: p.set_time(st.st_mtim.tv_sec, st.st_mtim.tv_nsec); break;
    case kpidATime: p.set_time(st.st_atim.tv_sec, st.st_atim.tv_nsec); break;
    case kpidCTime: p.set_time(st.st_ctim.tv_sec, st.st_ctim.tv_nsec); break;
    case kpidAttrib: {
        uint32_t a = 0x8000u | ((uint32_t)st.st_mode << 16) | (n.dir ? 0x10u : 0x20u);
        if (!(st.st_mode & S_IWUSR)) a |= 1;
        p.set_u32(a);
        break;
    }
    case kpidPosixAttrib: p.set_u32((uint32_t)st.st_mode); break;
    case kpidIsAnti: p.set_bool(false); break;
    case kpidSymLink:
        if (n.link) p.set_str(n.target);
        break;
    case kpidHardLink:
        if (!n.hard.empty()) p.set_str(n.hard);
        break;
    case kpidUserId:
        if (store_owner) p.set_u32((uint32_t)st.st_uid);
        break;
    case kpidGroupId:
        if (store_owner) p.set_u32((uint32_t)st.st_gid);
        break;
    case kpidUser:
        if (store_owner)
            if (const passwd *pw = getpwuid(st.st_uid)) p.set_str(pw->pw_name);
        break;
    case kpidGroup:
        if (store_owner)
            if (const group *gr = getgrgid(st.st_gid)) p.set_str(gr->gr_name);
        break;
    }
}

class UpdateCallback final : public IArchiveUpdateCallback,
                             public ICryptoGetTextPassword2,
                             public ICryptoGetTextPassword,
                             public ICompressProgressInfo {
    SZ_COM_BEGIN
    SZ_COM_IF(IArchiveUpdateCallback) SZ_COM_IF(IProgress) SZ_COM_IF(ICryptoGetTextPassword2)
    SZ_COM_IF(ICryptoGetTextPassword) SZ_COM_IF(ICompressProgressInfo)
    SZ_COM_END(IArchiveUpdateCallback)

    Ui &ui;
    Result &r;
    const std::vector<UpdItem> &items;
    const std::vector<NewItem> &news;
    Opened *old;  // may be null
    const Options &o;
    bool store_owner = false;
    bool cancelled = false;
    std::vector<char> failed;  // new items whose file couldn't be read

    UpdateCallback(Ui &u, Result &res, const std::vector<UpdItem> &it, const std::vector<NewItem> &n, Opened *ol,
                   const Options &op)
        : ui(u), r(res), items(it), news(n), old(ol), o(op), failed(n.size(), 0) {}

    HRESULT SetTotal(uint64_t total) override
    {
        ui.set_total(total);
        return S_OK;
    }
    HRESULT SetCompleted(const uint64_t *v) override
    {
        if (v) ui.set_completed(*v);
        if (ui.cancelled()) cancelled = true;
        return cancelled ? E_ABORT : S_OK;
    }
    HRESULT SetRatioInfo(const uint64_t *in, const uint64_t *out) override
    {
        if (in && out) ui.set_ratio(*in, *out);
        return S_OK;
    }
    HRESULT GetUpdateItemInfo(uint32_t index, int32_t *new_data, int32_t *new_props, uint32_t *index_in_archive) override
    {
        const UpdItem &u = items[index];
        if (new_data) *new_data = u.new_index >= 0;
        if (new_props) *new_props = u.new_index >= 0 || u.new_props;
        if (index_in_archive) *index_in_archive = u.old_index >= 0 ? (uint32_t)u.old_index : (uint32_t)-1;
        return S_OK;
    }
    HRESULT GetProperty(uint32_t index, PROPID id, PROPVARIANT *value) override
    {
        Prop &p = *static_cast<Prop *>(value);
        p.clear();
        const UpdItem &u = items[index];
        if (u.new_index >= 0) {
            set_file_props(p, id, news[(size_t)u.new_index], store_owner);
        } else if (u.new_props && old) {
            if (id == kpidPath) p.set_str(u.name);
            else if (id == kpidIsAnti) p.set_bool(false);
            else return old->arc()->GetProperty((uint32_t)u.old_index, id, value);
        }
        return S_OK;
    }
    HRESULT GetStream(uint32_t index, ISequentialInStream **stream) override
    {
        *stream = nullptr;
        if (ui.cancelled()) {
            cancelled = true;
            return E_ABORT;
        }
        const UpdItem &u = items[index];
        if (u.new_index < 0) return S_OK;
        const NewItem &n = news[(size_t)u.new_index];
        if (n.dir) return S_OK;
        ui.set_current("Compressing", n.name);
        if (n.link) {
            *stream = new MemIn(n.target);
            return S_OK;
        }
        if (!n.hard.empty()) return S_OK;
        int err = 0;
        InFile *f = InFile::open(n.disk, err);
        if (!f) {
            add_warning(r, ui, n.disk + " : " + errno_text(err));
            failed[(size_t)u.new_index] = 1;
            return S_FALSE;
        }
        *stream = f;
        return S_OK;
    }
    HRESULT SetOperationResult(int32_t result) override
    {
        if (result != 0) add_warning(r, ui, "Operation result " + std::to_string(result));
        ui.file_done();
        return S_OK;
    }
    HRESULT CryptoGetTextPassword2(int32_t *defined, BSTR *password) override
    {
        *defined = o.password_set && !o.password.empty();
        *password = make_bstr(*defined ? o.password : std::string());
        return S_OK;
    }
    HRESULT CryptoGetTextPassword(BSTR *password) override
    {
        *password = nullptr;
        std::string pw = o.password;
        if (old && !o.password_set) {
            pw = ask_password_for(ui, *old, cancelled);
            if (cancelled) return E_ABORT;
        }
        *password = make_bstr(pw);
        return S_OK;
    }
};

// ---------------------------------------------------------------- a u d rn

uint64_t parse_size(const std::string &s, bool &ok)
{
    char *end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    ok = end != s.c_str() && v > 0;
    if (!ok) return 0;
    uint64_t m = 1;
    const std::string u = lower(end);
    if (u.empty() || u == "b") m = 1;
    else if (u == "k") m = 1ull << 10;
    else if (u == "m") m = 1ull << 20;
    else if (u == "g") m = 1ull << 30;
    else ok = false;
    return v * m;
}

HRESULT set_handler_props(IUnknown *handler, const Options &o)
{
    if (o.props.empty()) return S_OK;
    Com<ISetProperties> sp;
    handler->QueryInterface(IID_ISetProperties, sp.pv());
    if (!sp) return S_OK;
    std::vector<std::wstring> names;
    std::vector<Prop> values(o.props.size());
    for (size_t i = 0; i < o.props.size(); i++) {
        std::string name = o.props[i].first;
        const std::string &val = o.props[i].second;
        if (val.empty()) {
            // "-mx9" is name "x9" with no value; "name+" / "name-" are booleans.
            if (!name.empty() && (name.back() == '+' || name.back() == '-')) {
                values[i].set_bool(name.back() == '+');
                name.pop_back();
            }
        } else {
            char *end = nullptr;
            const unsigned long long v = std::strtoull(val.c_str(), &end, 10);
            if (*end == 0 && g_ascii_isdigit(val[0])) {
                if (v <= 0xFFFFFFFFull) values[i].set_u32((uint32_t)v);
                else values[i].set_u64(v);
            } else {
                values[i].set_str(val);
            }
        }
        names.push_back(s2w(name));
    }
    std::vector<const wchar_t *> np;
    for (const auto &n : names) np.push_back(n.c_str());
    return sp->SetProperties(np.data(), values.data(), (uint32_t)np.size());
}

// Moves the finished temporary file over the archive (copying when they are on different file systems).
bool replace_file(const std::string &tmp, const std::string &dst, int &err)
{
    if (rename(tmp.c_str(), dst.c_str()) == 0) return true;
    if (errno != EXDEV) {
        err = errno;
        return false;
    }
    gchar *data = nullptr;
    gsize len = 0;
    GError *e = nullptr;
    const bool ok = g_file_get_contents(tmp.c_str(), &data, &len, &e) && g_file_set_contents(dst.c_str(), data, (gssize)len, &e);
    g_free(data);
    if (e) {
        err = EIO;
        g_error_free(e);
    }
    unlink(tmp.c_str());
    return ok;
}

bool under(const std::string &path, const std::string &dir) { return path == dir || starts_with(path, dir + "/"); }

void update(const Options &o, Ui &ui, Result &r)
{
    if (o.pos.empty()) {
        add_error(r, ui, "No archive name");
        return;
    }
    const std::string archive = absolute(o.pos[0]);
    struct stat ast {};
    const bool exists = stat(archive.c_str(), &ast) == 0;
    const bool adding = o.cmd == "a" || o.cmd == "u";
    if (!exists && !adding) {
        add_error(r, ui, archive + " : " + errno_text(ENOENT));
        return;
    }

    Opened old;
    bool have_old = false;
    int fmt = -1;
    if (exists) {
        std::string error;
        bool cancelled = false;
        if (!impl::open(archive, "", ui, o.password_set ? &o.password : nullptr, old, error, cancelled)) {
            r.cancelled = cancelled;
            if (!cancelled) add_error(r, ui, error);
            return;
        }
        if (old.parser || old.levels.size() > 1 || !old.volumes.empty()) {
            add_error(r, ui, "Updating of multivolume or nested archives is not supported : " + archive);
            return;
        }
        have_old = true;
        fmt = old.fmt();
        if (!o.type.empty() && find_format(o.type) != fmt) {
            add_error(r, ui, archive + " is a " + g_fmt[fmt].name + " archive, not " + o.type);
            return;
        }
        if (!g_fmt[fmt].update) {
            add_error(r, ui, g_fmt[fmt].name + " archives can't be updated : " + archive);
            return;
        }
    } else {
        if (!o.type.empty()) {
            fmt = find_format(o.type);
            if (fmt < 0) {
                add_error(r, ui, "Unsupported archive type: " + o.type);
                return;
            }
        } else {
            const std::string ext = path_ext(archive);
            for (size_t i = 0; i < g_fmt.size() && fmt < 0; i++)
                if (g_fmt[i].update)
                    for (const auto &e : g_fmt[i].exts)
                        if (e == ext) fmt = (int)i;
            if (fmt < 0) fmt = find_format("7z");
        }
        if (fmt < 0 || !g_fmt[fmt].update) {
            add_error(r, ui, "Archives of type " + (fmt < 0 ? o.type : g_fmt[fmt].name) + " can't be created");
            return;
        }
    }
    const Format &F = g_fmt[fmt];

    // New files.
    Scanner scan(ui, r, o, o.snl, o.snh && (F.flags & arc_flags::kHardLinks),
                 exists ? ast.st_dev : 0, exists ? ast.st_ino : 0);
    if (adding)
        for (size_t i = 1; i < o.pos.size(); i++) scan.add_arg(o.pos[i]);
    if (scan.cancelled) {
        r.cancelled = true;
        return;
    }
    std::vector<NewItem> &news = scan.items;

    // Action sets (p q r x y z w): 0 ignore, 1 keep the archived item, 2 pack the file.
    int act[7] = {1, 1, 2, 2, 2, 2, 2};
    if (o.cmd == "u") {
        const int u[7] = {1, 1, 2, 1, 2, 1, 1};
        std::copy(u, u + 7, act);
    }
    for (size_t i = 0; i + 1 < o.update_switches.size(); i += 2) {
        const char *letters = "pqrxyzw";
        const char *at = std::strchr(letters, o.update_switches[i]);
        const int v = o.update_switches[i + 1] - '0';
        if (at && *at && v >= 0 && v <= 3) act[at - letters] = v == 3 ? 0 : v;
    }

    std::vector<UpdItem> items;
    std::vector<char> new_used(news.size(), 0);
    uint32_t time_type = 0;
    Com<IOutArchive> out;
    if (have_old) {
        out = old.levels[0].arc.query<IOutArchive>(IID_IOutArchive);
    } else {
        g_lib.create_object(&F.clsid, &IID_IOutArchive, out.pv());
    }
    if (!out) {
        add_error(r, ui, "Cannot create " + F.name + " archives");
        return;
    }
    out->GetFileTimeType(&time_type);

    std::unordered_map<std::string, size_t> new_by_name;
    for (size_t i = 0; i < news.size(); i++) new_by_name[news[i].name] = i;

    if (have_old) {
        IInArchive *a = old.arc();
        uint32_t n = 0;
        a->GetNumberOfItems(&n);
        const std::string def = old.default_item_name();
        for (uint32_t i = 0; i < n; i++) {
            Entry e;
            read_entry(a, i, def, e);
            UpdItem u;
            u.old_index = (int32_t)i;
            if (o.cmd == "d") {
                bool hit = false;
                for (size_t k = 1; k < o.pos.size() && !hit; k++) {
                    const std::string &pat = o.pos[k];
                    if (o.spd || pat.find_first_of("*?") == std::string::npos) {
                        hit = under(e.path, pat);
                    } else {
                        const bool full = pat.find('/') != std::string::npos;
                        std::string probe = e.path;
                        // a pattern also removes the content of the folders it matches
                        for (;;) {
                            if (g_pattern_match_simple(pat.c_str(), (full ? probe : base_name(probe)).c_str())) {
                                hit = true;
                                break;
                            }
                            const auto sl = probe.rfind('/');
                            if (sl == std::string::npos) break;
                            probe.erase(sl);
                        }
                    }
                }
                if (!hit) items.push_back(u);
                continue;
            }
            if (o.cmd == "rn") {
                for (size_t k = 1; k + 1 < o.pos.size(); k += 2) {
                    const std::string &from = o.pos[k], &to = o.pos[k + 1];
                    if (under(e.path, from)) {
                        u.new_props = true;
                        u.name = to + e.path.substr(from.size());
                        break;
                    }
                }
                items.push_back(u);
                continue;
            }
            // a / u
            const auto f = new_by_name.find(e.path);
            int action;
            if (f != new_by_name.end()) {
                const NewItem &ni = news[f->second];
                new_used[f->second] = 1;
                const int64_t disk = (int64_t)ni.st.st_mtim.tv_sec;
                const int64_t tol = time_type == 2 ? 2 : 0;  // DOS times have 2 s steps
                if (ni.dir || !e.has_mtime) action = act[6];
                else if (e.mtime > disk + tol) action = act[3];  // newer in the archive
                else if (e.mtime + tol < disk) action = act[4];  // newer on disk
                else action = e.size == (uint64_t)ni.st.st_size ? act[5] : act[6];
                if (action == 2) {
                    UpdItem nu;
                    nu.new_index = (int)f->second;
                    items.push_back(nu);
                    continue;
                }
            } else {
                bool in_roots = false;
                for (const auto &root : scan.roots) in_roots = in_roots || under(e.path, root);
                action = in_roots ? act[1] : act[0];
            }
            if (action == 1) items.push_back(u);
        }
    }
    if (adding)
        for (size_t i = 0; i < news.size(); i++)
            if (!new_used[i] && act[2] == 2) {
                UpdItem nu;
                nu.new_index = (int)i;
                items.push_back(nu);
            }

    uint64_t num_new_files = 0;
    for (const auto &u : items)
        if (u.new_index >= 0) {
            const NewItem &n = news[(size_t)u.new_index];
            if (n.dir) r.folders++;
            else {
                r.files++;
                num_new_files++;
                if (!n.link && n.hard.empty()) r.size += (uint64_t)n.st.st_size;
            }
        }
    ui.set_num_files(num_new_files);

    Com<IUnknown> handler = out.query<IUnknown>(IID_IUnknown);
    const HRESULT sp = set_handler_props(handler.get(), o);
    if (sp != S_OK) {
        add_error(r, ui, "Unsupported or wrong archive parameters (" + hr_text(sp) + ")");
        return;
    }

    Com<UpdateCallback> cb(new UpdateCallback(ui, r, items, news, have_old ? &old : nullptr, o));

    // Output: volumes, or a temporary file next to the archive (or in the working folder).
    std::string tmp;
    Com<IOutStream> stream;
    MultiVolOut *vols = nullptr;
    OutFile *file = nullptr;
    if (!o.volume.empty()) {
        bool ok = false;
        const uint64_t vs = parse_size(o.volume, ok);
        if (!ok) {
            add_error(r, ui, "Incorrect volume size: " + o.volume);
            return;
        }
        if (have_old) {
            add_error(r, ui, "Updating of multivolume archives is not supported : " + archive);
            return;
        }
        vols = new MultiVolOut(archive, vs);
        stream = Com<IOutStream>(vols);
    } else {
        const std::string dir = o.work_dir.empty() ? dir_name(archive) : o.work_dir;
        tmp = dir + "/.7zipl-" + base_name(archive) + "-XXXXXX";
        std::vector<char> t(tmp.begin(), tmp.end());
        t.push_back(0);
        const int fd = mkstemp(t.data());
        if (fd < 0) {
            add_error(r, ui, "Cannot create temporary file : " + errno_text(errno) + " : " + dir);
            return;
        }
        tmp = t.data();
        const mode_t um = umask(0);
        umask(um);
        fchmod(fd, exists ? (ast.st_mode & 07777) : (0666 & ~um));
        file = new OutFile(fd);
        stream = Com<IOutStream>(file);
    }

    const HRESULT hr = out->UpdateItems(stream.get(), (uint32_t)items.size(), cb.get());
    int close_err = 0;
    if (file) close_err = file->close_fd();
    if (vols) close_err = vols->close_all();
    if (hr != S_OK || close_err) {
        if (vols) vols->remove_all();
        if (!tmp.empty()) unlink(tmp.c_str());
        if (hr == E_ABORT && cb->cancelled) r.cancelled = true;
        else add_error(r, ui, (hr != S_OK ? hr_text(hr) : errno_text(close_err)) + " : " + archive);
        return;
    }
    stream.reset();
    out.reset();
    handler.reset();
    if (have_old) {
        old.arc()->Close();
        old.levels.clear();
    }
    if (!tmp.empty()) {
        int err = 0;
        if (!replace_file(tmp, archive, err)) {
            unlink(tmp.c_str());
            add_error(r, ui, "Cannot write the archive : " + errno_text(err) + " : " + archive);
            return;
        }
    }
    struct stat fin;
    if (stat(archive.c_str(), &fin) == 0) r.packed = (uint64_t)fin.st_size;

    // -sdel: the added files go away once the archive is written; folders when they are empty.
    if (o.sdel && r.errors == 0) {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (it->new_index < 0 || cb->failed[(size_t)it->new_index]) continue;
            const NewItem &n = news[(size_t)it->new_index];
            if (n.dir) rmdir(n.disk.c_str());
            else if (unlink(n.disk.c_str()) != 0) add_warning(r, ui, n.disk + " : " + errno_text(errno));
        }
    }
}

// ---------------------------------------------------------------- x e t

void extract_cmd(const Options &o, Ui &ui, Result &r)
{
    if (o.pos.empty()) {
        add_error(r, ui, "No archive name");
        return;
    }
    for (const auto &arg : o.pos) {
        if (ui.cancelled()) {
            r.cancelled = true;
            return;
        }
        const std::string path = absolute(arg);
        Opened op;
        std::string error;
        bool cancelled = false;
        if (!impl::open(path, o.type, ui, o.password_set ? &o.password : nullptr, op, error, cancelled)) {
            if (cancelled) {
                r.cancelled = true;
                return;
            }
            add_error(r, ui, error);
            continue;
        }
        if (op.parser) {
            add_error(r, ui, "Cannot extract embedded archives : " + path);
            continue;
        }
        ExtractParams p;
        p.test = o.cmd == "t";
        p.path_mode = o.cmd == "e" ? 1 : o.spf ? 2 : 0;
        p.overwrite = o.overwrite;
        std::string out = o.out_set ? o.out_dir : cwd();
        const auto star = out.find('*');
        if (star != std::string::npos) {
            std::string stem = base_name(path);
            const std::string ext = path_ext(stem);
            if (!ext.empty()) stem.erase(stem.size() - ext.size() - 1);
            out.replace(star, 1, stem);
        }
        p.out_dir = absolute(out);
        if (o.spe && !p.test) {
            // A single root folder named like the output folder isn't repeated inside it.
            IInArchive *a = op.arc();
            uint32_t n = 0;
            a->GetNumberOfItems(&n);
            std::string root;
            bool single = n > 0;
            const std::string def = op.default_item_name();
            for (uint32_t i = 0; i < n && single; i++) {
                Entry e;
                read_entry(a, i, def, e);
                const std::string first = e.path.substr(0, e.path.find('/'));
                if (root.empty()) root = first;
                single = first == root && (e.path != root || e.dir);
            }
            if (single && root == base_name(p.out_dir)) p.strip_prefix = root;
        }
        if (!p.test) {
            int err = 0;
            if (!make_dirs(p.out_dir, err)) {
                add_error(r, ui, "Cannot create folder : " + errno_text(err) + " : " + p.out_dir);
                continue;
            }
        }
        extract(op, {}, p, ui, r);
        if (r.cancelled) return;
    }
}

// ---------------------------------------------------------------- h

std::string hex_digest(const Byte *d, size_t n)
{
    static const char lo[] = "0123456789abcdef", up[] = "0123456789ABCDEF";
    std::string s;
    if (n > 8) {
        for (size_t i = 0; i < n; i++) s += lo[d[i] >> 4], s += lo[d[i] & 15];
    } else {
        // CRC32, CRC64, XXH64: printed as numbers, most significant byte first
        for (size_t i = n; i-- > 0;) s += up[d[i] >> 4], s += up[d[i] & 15];
    }
    return s;
}

void hash_cmd(const Options &o, Ui &ui, Result &r)
{
    Com<IHashers> hs;
    if (!g_lib.get_hashers || g_lib.get_hashers(hs.put()) != S_OK || !hs) {
        add_error(r, ui, "The 7-Zip library has no hash functions");
        return;
    }
    std::vector<std::pair<std::string, uint32_t>> all;
    const uint32_t nh = hs->GetNumHashers();
    for (uint32_t i = 0; i < nh; i++) {
        Prop p;
        std::string s;
        if (hs->GetHasherProp(i, method_prop::kName, &p) == S_OK && p.get_str(s)) all.emplace_back(s, i);
    }
    std::vector<std::pair<std::string, uint32_t>> use;
    std::vector<std::string> want = o.hash_methods;
    if (want.empty() || (want.size() == 1 && want[0].empty())) want = {"CRC32"};
    for (const auto &w : want) {
        if (w == "*") {
            use = all;
            break;
        }
        bool found = false;
        for (const auto &a : all)
            if (lower(a.first) == lower(w)) {
                use.push_back(a);
                found = true;
            }
        if (!found) {
            add_error(r, ui, "Unsupported hash method: " + w);
            return;
        }
    }
    struct H {
        Com<IHasher> h;
        uint32_t size;
        std::vector<Byte> sum;  // digest sum, then 8 carry bytes
    };
    std::vector<H> hashers;
    for (const auto &u : use) {
        H h;
        if (hs->CreateHasher(u.second, h.h.put()) != S_OK || !h.h) continue;
        h.size = h.h->GetDigestSize();
        h.sum.assign(h.size + 8, 0);
        hashers.push_back(std::move(h));
        r.hash.methods.push_back(u.first);
    }

    Options so = o;
    so.spf = 0;
    Scanner scan(ui, r, so, false, false, 0, 0);
    for (const auto &p : o.pos) scan.add_arg(p);
    if (scan.cancelled) {
        r.cancelled = true;
        return;
    }
    uint64_t total = 0;
    for (const auto &n : scan.items)
        if (!n.dir) total += (uint64_t)n.st.st_size;
    ui.set_total(total);
    ui.set_num_files(scan.items.size());

    std::vector<char> buf(1 << 20);
    uint64_t done = 0;
    std::vector<Byte> digest(64);
    for (const auto &n : scan.items) {
        HashLine line;
        line.name = n.name;
        line.dir = n.dir;
        if (n.dir) {
            r.hash.num_dirs++;
            line.digests.assign(hashers.size(), "");
            r.hash.files.push_back(line);
            continue;
        }
        ui.set_current("Hashing", n.name);
        const int fd = ::open(n.disk.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            add_error(r, ui, n.disk + " : " + errno_text(errno));
            r.hash.errors++;
            continue;
        }
        for (auto &h : hashers) h.h->Init();
        uint64_t size = 0;
        bool failed = false;
        for (;;) {
            const ssize_t got = ::read(fd, buf.data(), buf.size());
            if (got < 0) {
                if (errno == EINTR) continue;
                add_error(r, ui, n.disk + " : " + errno_text(errno));
                failed = true;
                break;
            }
            if (got == 0) break;
            for (auto &h : hashers) h.h->Update(buf.data(), (uint32_t)got);
            size += (uint64_t)got;
            done += (uint64_t)got;
            ui.set_completed(done);
            if (ui.cancelled()) {
                ::close(fd);
                r.cancelled = true;
                return;
            }
        }
        ::close(fd);
        if (failed) {
            r.hash.errors++;
            continue;
        }
        line.size = size;
        for (auto &h : hashers) {
            std::fill(digest.begin(), digest.end(), 0);
            if (digest.size() < h.size) digest.resize(h.size);
            h.h->Final(digest.data());
            line.digests.push_back(hex_digest(digest.data(), h.size));
            unsigned carry = 0;
            for (size_t i = 0; i < h.size; i++) {
                carry += (unsigned)h.sum[i] + digest[i];
                h.sum[i] = (Byte)carry;
                carry >>= 8;
            }
            for (size_t i = h.size; i < h.sum.size(); i++) {
                carry += h.sum[i];
                h.sum[i] = (Byte)carry;
                carry >>= 8;
            }
        }
        r.hash.files.push_back(line);
        r.hash.num_files++;
        r.hash.total_size += size;
        ui.file_done();
    }
    for (auto &h : hashers) {
        std::string t = hex_digest(h.sum.data(), h.size);
        if (r.hash.num_files != 1) {
            size_t extra = 0;
            for (size_t i = 0; i < 8; i++)
                if (h.sum[h.size + i]) extra = i + 1;
            t += "-" + hex_digest(h.sum.data() + h.size, extra > 4 ? 8 : 4);
        }
        r.hash.totals.push_back(t);
    }
    r.files = r.hash.num_files;
    r.folders = r.hash.num_dirs;
    r.size = r.hash.total_size;
}

// ---------------------------------------------------------------- benchmark (7-Zip's LZMA rating)

class RandomGen {
    uint32_t a1_ = 362436069, a2_ = 521288629;

public:
    uint32_t next()
    {
        return ((a1_ = 36969 * (a1_ & 0xffff) + (a1_ >> 16)) << 16) + (a2_ = 18000 * (a2_ & 0xffff) + (a2_ >> 16));
    }
};

// 7-Zip's CBenchRandomGenerator::GenerateLz: compressible data with matches up to the dictionary size.
void generate_lz(std::vector<Byte> &buf, unsigned dict_bits)
{
    RandomGen rg;
    auto val = [](uint32_t &r, unsigned bits) {
        const uint32_t v = r & ((1u << bits) - 1);
        r >>= bits;
        return v;
    };
    auto len_of = [&](uint32_t &r) {
        const unsigned l = val(r, 2);
        return val(r, 1 + l);
    };
    size_t pos = 0, rep0 = 1;
    unsigned pos_bits = 1;
    while (pos < buf.size()) {
        uint32_t r = rg.next();
        if (val(r, 1) == 0 || pos < 1024) {
            buf[pos++] = (Byte)(r & 0xFF);
            continue;
        }
        uint32_t len = 1 + len_of(r);
        if (val(r, 3) != 0) {
            len += len_of(r);
            while (((size_t)1 << pos_bits) < pos) pos_bits++;
            const unsigned max_bits = std::min(dict_bits, pos_bits);
            const unsigned add_bits = 6;
            const unsigned log_bits = max_bits <= (1 << 4) - 1 + add_bits ? 4 : 5;
            for (;;) {
                const uint32_t ppp = val(r, log_bits) + add_bits;
                r = rg.next();
                if (ppp > max_bits) continue;
                rep0 = r & (((size_t)1 << ppp) - 1);
                if (rep0 < pos) break;
                r = rg.next();
            }
            rep0++;
        }
        len = (uint32_t)std::min<size_t>(len, buf.size() - pos);
        for (uint32_t i = 0; i < len; i++, pos++) buf[pos] = buf[pos - rep0];
    }
}

class BufIn final : public ISequentialInStream {
    const Byte *d_;
    size_t n_, pos_ = 0;
    SZ_COM_BEGIN SZ_COM_IF(ISequentialInStream) SZ_COM_END(ISequentialInStream)
    BufIn(const Byte *d, size_t n) : d_(d), n_(n) {}
    HRESULT Read(void *data, uint32_t size, uint32_t *processed) override
    {
        const size_t k = std::min<size_t>(size, n_ - pos_);
        std::memcpy(data, d_ + pos_, k);
        pos_ += k;
        if (processed) *processed = (uint32_t)k;
        return S_OK;
    }
};

class BufOut final : public ISequentialOutStream {
    SZ_COM_BEGIN SZ_COM_IF(ISequentialOutStream) SZ_COM_END(ISequentialOutStream)
    std::vector<Byte> data;
    size_t pos = 0;
    bool overflow = false;
    explicit BufOut(size_t cap) : data(cap) {}
    HRESULT Write(const void *d, uint32_t size, uint32_t *processed) override
    {
        const size_t k = std::min<size_t>(size, data.size() - pos);
        std::memcpy(data.data() + pos, d, k);
        pos += k;
        if (k < size) overflow = true;
        if (processed) *processed = size;
        return S_OK;
    }
};

class BenchProgress final : public ICompressProgressInfo {
    SZ_COM_BEGIN SZ_COM_IF(ICompressProgressInfo) SZ_COM_END(ICompressProgressInfo)
    Ui &ui;
    std::atomic<bool> &stop;
    BenchProgress(Ui &u, std::atomic<bool> &s) : ui(u), stop(s) {}
    HRESULT SetRatioInfo(const uint64_t *, const uint64_t *) override
    {
        if (!stop && ui.cancelled()) stop = true;
        return stop ? E_ABORT : S_OK;
    }
};

uint32_t log_sub(uint64_t size)
{
    if (size <= 1) return 0;
    unsigned i = 0;
    for (uint64_t s = size; s > 1; s >>= 1) i++;
    const uint32_t v = i <= 8 ? (uint32_t)(size << (8 - i)) : (uint32_t)(size >> (i - 8));
    return ((uint32_t)i << 8) + (v & 0xFF);
}

double enc_rating(uint64_t dict, double seconds, uint64_t bytes)
{
    if (dict < (1u << 18)) dict = 1u << 18;
    const uint32_t t = log_sub(dict) - (18u << 8);
    const uint64_t complexity = 870 + ((uint64_t)t * t * 5 >> 16);
    return (double)bytes * (double)complexity / seconds;
}

double dec_rating(double seconds, uint64_t unpacked, uint64_t packed) { return ((double)packed * 190 + (double)unpacked * 4) / seconds; }

double cpu_seconds()
{
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + (double)u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

struct Row {
    double speed = 0, usage = 0, rpu = 0, rating = 0;
};

std::string fmt_row(const char *label, const Row &c, const Row &d, bool speed)
{
    char b[160];
    if (speed)
        std::snprintf(b, sizeof b, "%-4s %8.0f %5.0f %6.0f %6.0f  | %10.0f %5.0f %6.0f %6.0f\n", label, c.speed, c.usage,
                      c.rpu, c.rating, d.speed, d.usage, d.rpu, d.rating);
    else
        std::snprintf(b, sizeof b, "%-4s %8s %5.0f %6.0f %6.0f  | %10s %5.0f %6.0f %6.0f\n", label, "", c.usage, c.rpu,
                      c.rating, "", d.usage, d.rpu, d.rating);
    return b;
}

}  // namespace
}  // namespace impl

using namespace impl;

Result benchmark(unsigned threads, unsigned dict_log, unsigned passes, Ui &ui,
                 const std::function<void(const std::string &)> &print)
{
    Result r;
    if (!available()) {
        add_error(r, ui, "7-Zip's library (7z.so) was not found");
        finish(r);
        return r;
    }
    int lzma = -1;
    uint32_t nm = 0;
    if (g_lib.num_methods) g_lib.num_methods(&nm);
    for (uint32_t i = 0; i < nm && lzma < 0; i++) {
        Prop p;
        std::string s;
        if (g_lib.method_prop(i, method_prop::kName, &p) == S_OK && p.get_str(s) && s == "LZMA") lzma = (int)i;
    }
    if (lzma < 0 || !g_lib.create_encoder || !g_lib.create_decoder) {
        add_error(r, ui, "The 7-Zip library has no LZMA codec");
        finish(r);
        return r;
    }
    if (threads == 0) threads = g_get_num_processors();
    if (dict_log < 18) dict_log = 22;
    if (dict_log > 30) dict_log = 30;
    if (passes == 0) passes = 1;
    const unsigned first = std::min(22u, dict_log);
    const unsigned enc_jobs = threads >= 2 ? threads / 2 : 1;
    const unsigned enc_threads = threads >= 2 ? 2 : 1;
    const unsigned dec_jobs = threads;

    char head[512];
    std::snprintf(head, sizeof head,
                  "7-Zip %s (%s)\nThreads: %u   Dictionary: 2^%u = %u MB   Passes: %u\n\n"
                  "         Compressing                  |           Decompressing\n"
                  "Dict    Speed Usage    R/U Rating  |      Speed Usage    R/U Rating\n"
                  "        KiB/s     %%   MIPS   MIPS  |      KiB/s     %%   MIPS   MIPS\n\n",
                  version().c_str(), g_lib.path.c_str(), threads, dict_log, (1u << dict_log) >> 20, passes);
    print(head);

    std::atomic<bool> stop{false};
    Row sum_c, sum_d;
    unsigned rows = 0;
    for (unsigned pass = 0; pass < passes && !stop; pass++) {
        for (unsigned dl = first; dl <= dict_log && !stop; dl++) {
            const uint64_t dict = 1ull << dl;
            std::vector<Byte> data((size_t)dict + (1 << 16));
            generate_lz(data, dl);

            // Compression: encoder jobs in parallel, repeated until about a second has passed.
            std::vector<std::vector<Byte>> packed(enc_jobs);
            std::vector<std::string> coder_props(enc_jobs);
            std::vector<uint64_t> iters(enc_jobs, 0);
            std::atomic<bool> failed{false};
            const auto t0 = std::chrono::steady_clock::now();
            const double c0 = cpu_seconds();
            auto encode = [&](unsigned j) {
                Com<ICompressCoder> enc;
                if (g_lib.create_encoder((uint32_t)lzma, &IID_ICompressCoder, enc.pv()) != S_OK || !enc) {
                    failed = true;
                    return;
                }
                if (auto sp = enc.query<ICompressSetCoderProperties>(IID_ICompressSetCoderProperties)) {
                    const PROPID ids[] = {coder_prop::kDictionarySize, coder_prop::kNumThreads, coder_prop::kLevel};
                    Prop v[3];
                    v[0].set_u32((uint32_t)dict);
                    v[1].set_u32(enc_threads);
                    v[2].set_u32(5);
                    sp->SetCoderProperties(ids, v, 3);
                }
                Com<MemOut> pr(new MemOut);
                if (auto wp = enc.query<ICompressWriteCoderProperties>(IID_ICompressWriteCoderProperties))
                    wp->WriteCoderProperties(pr.get());
                coder_props[j] = pr->data;
                Com<BenchProgress> prog(new BenchProgress(ui, stop));
                do {
                    Com<BufIn> in(new BufIn(data.data(), data.size()));
                    Com<BufOut> out(new BufOut(data.size() + data.size() / 16 + (1 << 16)));
                    const HRESULT hr = enc->Code(in.get(), out.get(), nullptr, nullptr, prog.get());
                    if (hr != S_OK) {
                        if (hr != E_ABORT) failed = true;
                        return;
                    }
                    if (iters[j] == 0) packed[j].assign(out->data.begin(), out->data.begin() + (long)out->pos);
                    iters[j]++;
                } while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 1.0 && !stop);
            };
            {
                std::vector<std::thread> ts;
                for (unsigned j = 0; j < enc_jobs; j++) ts.emplace_back(encode, j);
                for (auto &t : ts) t.join();
            }
            const double ce = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double cc = cpu_seconds() - c0;
            if (stop) break;
            if (failed) {
                add_error(r, ui, "LZMA encoding failed");
                break;
            }
            uint64_t enc_bytes = 0;
            for (auto n : iters) enc_bytes += n * data.size();

            // Decompression of what job 0 produced, checked against the input once.
            std::vector<uint64_t> diters(dec_jobs, 0);
            const auto t1 = std::chrono::steady_clock::now();
            const double c1 = cpu_seconds();
            auto decode = [&](unsigned j) {
                Com<ICompressCoder> dec;
                if (g_lib.create_decoder((uint32_t)lzma, &IID_ICompressCoder, dec.pv()) != S_OK || !dec) {
                    failed = true;
                    return;
                }
                auto dp = dec.query<ICompressSetDecoderProperties2>(IID_ICompressSetDecoderProperties2);
                if (!dp || dp->SetDecoderProperties2(reinterpret_cast<const Byte *>(coder_props[0].data()),
                                                     (uint32_t)coder_props[0].size()) != S_OK) {
                    failed = true;
                    return;
                }
                Com<BenchProgress> prog(new BenchProgress(ui, stop));
                const uint64_t out_size = data.size();
                do {
                    Com<BufIn> in(new BufIn(packed[0].data(), packed[0].size()));
                    Com<BufOut> out(new BufOut(data.size()));
                    const HRESULT hr = dec->Code(in.get(), out.get(), nullptr, &out_size, prog.get());
                    if (hr != S_OK) {
                        if (hr != E_ABORT) failed = true;
                        return;
                    }
                    if (diters[j] == 0 && (out->pos != data.size() || out->data != data)) {
                        failed = true;
                        return;
                    }
                    diters[j]++;
                } while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() < 1.0 && !stop);
            };
            {
                std::vector<std::thread> ts;
                for (unsigned j = 0; j < dec_jobs; j++) ts.emplace_back(decode, j);
                for (auto &t : ts) t.join();
            }
            const double de = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            const double dc = cpu_seconds() - c1;
            if (stop) break;
            if (failed) {
                add_error(r, ui, "LZMA decoding failed: the data doesn't match");
                break;
            }
            uint64_t dn = 0;
            for (auto n : diters) dn += n;

            Row c, d;
            c.speed = (double)enc_bytes / ce / 1024;
            c.usage = cc / ce * 100;
            c.rating = enc_rating(dict, ce, enc_bytes) / 1e6;
            c.rpu = cc > 0 ? c.rating * ce / cc : 0;
            d.speed = (double)(dn * data.size()) / de / 1024;
            d.usage = dc / de * 100;
            d.rating = dec_rating(de, dn * data.size(), dn * packed[0].size()) / 1e6;
            d.rpu = dc > 0 ? d.rating * de / dc : 0;
            const std::string label = std::to_string(dl) + ":";
            print(fmt_row(label.c_str(), c, d, true));
            sum_c.usage += c.usage, sum_c.rpu += c.rpu, sum_c.rating += c.rating;
            sum_d.usage += d.usage, sum_d.rpu += d.rpu, sum_d.rating += d.rating;
            rows++;
        }
    }
    if (stop) r.cancelled = true;
    if (rows && !r.cancelled && r.errors == 0) {
        auto avg = [&](Row s) {
            s.usage /= rows, s.rpu /= rows, s.rating /= rows;
            return s;
        };
        const Row ac = avg(sum_c), ad = avg(sum_d);
        print("----------------------------------|------------------------------\n");
        print(fmt_row("Avr:", ac, ad, false));
        char b[128];
        std::snprintf(b, sizeof b, "Tot: %8s %5.0f %6.0f %6.0f\n", "", (ac.usage + ad.usage) / 2, (ac.rpu + ad.rpu) / 2,
                      (ac.rating + ad.rating) / 2);
        print(b);
    }
    finish(r);
    return r;
}

Result run(const std::vector<std::string> &args, Ui &ui)
{
    Result r;
    Options o;
    std::string error;
    if (!available()) {
        add_error(r, ui, "7-Zip's library (7z.so) was not found: install the 7zip package");
    } else if (!parse(args, o, error)) {
        add_error(r, ui, error);
    } else if (o.cmd == "a" || o.cmd == "u" || o.cmd == "d" || o.cmd == "rn") {
        update(o, ui, r);
    } else if (o.cmd == "x" || o.cmd == "e" || o.cmd == "t") {
        extract_cmd(o, ui, r);
    } else if (o.cmd == "h") {
        hash_cmd(o, ui, r);
    } else {
        add_error(r, ui, "Unsupported command: " + o.cmd);
    }
    finish(r);
    return r;
}

}  // namespace szl
