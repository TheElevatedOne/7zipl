// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: uses the archive handlers of 7-Zip's library 7z.so (the distribution's 7zip package, linked at
// build time, declared in sevenzip.hpp) directly: format detection, opening (volumes, nested and embedded archives), listing and
// extracting. Updating, hashing and the benchmark are in engine_ops.cpp.
#include "engine_impl.hpp"

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace szl {
namespace impl {

Lib g_lib;
uint64_t g_memory_limit = 0;
std::vector<Format> g_fmt;

namespace {
std::string g_version;
std::vector<std::string> g_hashers;
std::string g_load_error;
bool g_inited = false;

constexpr int64_t kUnixToFiletime = 11644473600LL;
}  // namespace

// ---------------------------------------------------------------- strings

std::string w2s(const wchar_t *w, size_t n)
{
    std::string out;
    if (!w) return out;
    for (size_t i = 0; i < n && w[i]; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) c = 0xFFFD;
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring s2w(const std::string &s)
{
    std::wstring out;
    out.reserve(s.size());
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned c = p[i];
        unsigned len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        uint32_t v = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        bool ok = len != 0 && i + len <= n;
        for (unsigned k = 1; ok && k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) ok = false;
            else v = (v << 6) | (p[i + k] & 0x3F);
        }
        if (!ok) {
            out += (wchar_t)0xFFFD;
            i++;
            continue;
        }
        out += (wchar_t)v;
        i += len;
    }
    return out;
}

BSTR make_bstr(const std::string &s)
{
    const std::wstring w = s2w(s);
    return SysAllocStringLen(w.c_str(), (uint32_t)w.size());
}

std::string lower(const std::string &s)
{
    std::string r = s;
    for (auto &c : r) c = (char)g_ascii_tolower(c);
    return r;
}

std::string base_name(const std::string &p)
{
    const auto k = p.rfind('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}

std::string dir_name(const std::string &p)
{
    const auto k = p.rfind('/');
    if (k == std::string::npos) return ".";
    if (k == 0) return "/";
    return p.substr(0, k);
}

std::string path_ext(const std::string &name)
{
    const std::string b = base_name(name);
    const auto dot = b.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    return lower(b.substr(dot + 1));
}

bool starts_with(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }

std::string errno_text(int e) { return g_strerror(e); }

HRESULT hr_from_errno(int e) { return (HRESULT)(((uint32_t)e & 0xFFFF) | 0x88000000u); }

std::string hr_text(HRESULT hr)
{
    const uint32_t u = (uint32_t)hr;
    if ((u & 0xFFFF0000u) == 0x88000000u) return errno_text((int)(u & 0xFFFF));  // errno
    if ((u & 0xFFFF0000u) == 0x80070000u) return errno_text((int)(u & 0xFFFF));  // HRESULT_FROM_WIN32
    switch (u) {
    case 0x80004001u: return "Not implemented";
    case 0x80004004u: return "Operation was cancelled";
    case 0x80004005u: return "Unspecified error";
    case 0x8007000Eu: return "Not enough memory";
    case 0x80070057u: return "Invalid argument";
    case 0x80030001u: return "Invalid function";
    case 1: return "Unsupported or damaged data";
    }
    char b[64];
    std::snprintf(b, sizeof b, "Error 0x%08X", u);
    return b;
}

std::string op_result_text(int32_t r, bool encrypted)
{
    switch (r) {
    case op_result::kUnsupportedMethod: return "Unsupported Method";
    case op_result::kDataError: return encrypted ? "Data Error in encrypted file. Wrong password?" : "Data Error";
    case op_result::kCRCError: return encrypted ? "CRC Failed in encrypted file. Wrong password?" : "CRC Failed";
    case op_result::kUnavailable: return "Unavailable data";
    case op_result::kUnexpectedEnd: return "Unexpected end of data";
    case op_result::kDataAfterEnd: return "There are some data after the end of the payload data";
    case op_result::kIsNotArc: return "Is not archive";
    case op_result::kHeadersError: return "Headers Error";
    case op_result::kWrongPassword: return "Wrong password";
    }
    return "Error #" + std::to_string(r);
}

// ---------------------------------------------------------------- properties

void Prop::clear()
{
    if (vt == VT_BSTR && bstrVal) SysFreeString(bstrVal);
    std::memset(static_cast<PROPVARIANT *>(this), 0, sizeof(PROPVARIANT));
}

bool Prop::get_str(std::string &out) const
{
    if (vt != VT_BSTR || !bstrVal) return false;
    out = w2s(bstrVal);
    return true;
}

bool Prop::get_u64(uint64_t &out) const
{
    switch (vt) {
    case VT_UI1: out = bVal; return true;
    case VT_UI2: out = uiVal; return true;
    case VT_UI4: case VT_UINT: out = ulVal; return true;
    case VT_UI8: out = uhVal; return true;
    case VT_I2: out = (uint64_t)(int64_t)iVal; return true;
    case VT_I4: case VT_INT: out = (uint64_t)(int64_t)lVal; return true;
    case VT_I8: out = (uint64_t)hVal; return true;
    }
    return false;
}

bool Prop::get_time(int64_t &unix_seconds) const
{
    if (vt != VT_FILETIME) return false;
    const uint64_t ft = ((uint64_t)filetime.hi << 32) | filetime.lo;
    if (ft == 0) return false;
    unix_seconds = (int64_t)(ft / 10000000) - kUnixToFiletime;
    return true;
}

void Prop::set_str(const std::string &s)
{
    clear();
    vt = VT_BSTR;
    bstrVal = make_bstr(s);
}
void Prop::set_u32(uint32_t v)
{
    clear();
    vt = VT_UI4;
    ulVal = v;
}
void Prop::set_u64(uint64_t v)
{
    clear();
    vt = VT_UI8;
    uhVal = v;
}
void Prop::set_bool(bool v)
{
    clear();
    vt = VT_BOOL;
    boolVal = v ? -1 : 0;
}
void Prop::set_time(int64_t sec, long nsec)
{
    clear();
    vt = VT_FILETIME;
    const uint64_t ft = (uint64_t)(sec + kUnixToFiletime) * 10000000u + (uint64_t)(nsec / 100);
    filetime.lo = (uint32_t)ft;
    filetime.hi = (uint32_t)(ft >> 32);
}

std::string prop_name(PROPID id)
{
    switch (id) {
    case kpidPath: return "Path";
    case kpidName: return "Name";
    case kpidType: return "Type";
    case kpidSize: return "Size";
    case kpidPackSize: return "Packed Size";
    case kpidPhySize: return "Physical Size";
    case kpidHeadersSize: return "Headers Size";
    case kpidMethod: return "Method";
    case kpidSolid: return "Solid";
    case kpidNumBlocks: return "Blocks";
    case kpidNumVolumes: return "Volumes";
    case kpidIsVolume: return "Volume";
    case kpidVolumeIndex: return "Volume Index";
    case kpidComment: return "Comment";
    case kpidShortComment: return "Short Comment";
    case kpidOffset: return "Offset";
    case kpidCharacts: return "Characteristics";
    case kpidCTime: return "Created";
    case kpidMTime: return "Modified";
    case kpidATime: return "Accessed";
    case kpidCreatorApp: return "Creator Application";
    case kpidCodePage: return "Code Page";
    case kpidTotalPhySize: return "Total Physical Size";
    case kpidEmbeddedStubSize: return "Embedded Stub Size";
    case kpidTailSize: return "Tail Size";
    case kpidUnpackVer: return "Version";
    case kpidHostOS: return "Host OS";
    case kpidSectorSize: return "Sector Size";
    case kpidClusterSize: return "Cluster Size";
    case kpidFileSystem: return "File System";
    case kpidVolumeName: return "Label";
    case kpidBit64: return "64-bit";
    case kpidBigEndian: return "Big-endian";
    case kpidCpu: return "CPU";
    case kpidId: return "ID";
    case kpidUnpackSize: return "Unpack Size";
    case kpidChecksum: return "Checksum";
    case kpidDictionarySize: return "Dictionary Size";
    case kpidError: return "Error";
    case kpidWarning: return "Warning";
    case kpidNumSubDirs: return "Folders";
    case kpidNumSubFiles: return "Files";
    case kpidEncrypted: return "Encrypted";
    case kpidSubType: return "Subtype";
    case kpidVirtualSize: return "Virtual Size";
    case kpidTotalSize: return "Total Size";
    case kpidFreeSpace: return "Free Space";
    case kpidReadOnly: return "Read-only";
    case kpidCRC: return "CRC";
    case kpidZerosTailIsAllowed: return "Zeros Tail Is Allowed";
    }
    return {};
}

std::string prop_display(PROPID id, const Prop &p)
{
    std::string s;
    uint64_t v;
    int64_t t;
    if (p.get_str(s)) return s;
    if (p.vt == VT_BOOL) return p.get_bool() ? "+" : "-";
    if (p.get_time(t)) {
        const time_t tt = (time_t)t;
        struct tm tm;
        localtime_r(&tt, &tm);
        char b[32];
        std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
        return b;
    }
    if (p.get_u64(v)) {
        char b[32];
        if (id == kpidCRC || id == kpidChecksum) std::snprintf(b, sizeof b, "%08llX", (unsigned long long)v);
        else std::snprintf(b, sizeof b, "%llu", (unsigned long long)v);
        return b;
    }
    return {};
}

int find_format(const std::string &name)
{
    const std::string l = lower(name);
    for (size_t i = 0; i < g_fmt.size(); i++)
        if (lower(g_fmt[i].name) == l) return (int)i;
    // Aliases 7-Zip accepts for -t.
    static const std::pair<const char *, const char *> alias[] = {{"gz", "gzip"}, {"bz2", "bzip2"}, {"zst", "zstd"}};
    for (const auto &a : alias)
        if (l == a.first) return find_format(a.second);
    return -1;
}

// ---------------------------------------------------------------- streams

InFile::~InFile() { ::close(fd_); }

InFile *InFile::open(const std::string &path, int &err)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = errno;
        return nullptr;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        err = S_ISDIR(st.st_mode) ? EISDIR : errno;
        ::close(fd);
        return nullptr;
    }
    return new InFile(fd, (uint64_t)st.st_size);
}

HRESULT InFile::Read(void *data, uint32_t size, uint32_t *processed)
{
    if (processed) *processed = 0;
    if (size == 0) return S_OK;
    ssize_t n;
    do n = pread(fd_, data, size, (off_t)pos_);
    while (n < 0 && errno == EINTR);
    if (n < 0) return hr_from_errno(errno);
    pos_ += (uint64_t)n;
    if (processed) *processed = (uint32_t)n;
    return S_OK;
}

static HRESULT do_seek(uint64_t &pos, uint64_t size, int64_t offset, uint32_t origin, uint64_t *newpos)
{
    if (origin > 2) return E_INVALIDARG;
    const int64_t base = origin == 0 ? 0 : origin == 1 ? (int64_t)pos : (int64_t)size;
    const int64_t np = base + offset;
    if (np < 0) return (HRESULT)0x80070083u;  // ERROR_NEGATIVE_SEEK
    pos = (uint64_t)np;
    if (newpos) *newpos = pos;
    return S_OK;
}

HRESULT InFile::Seek(int64_t offset, uint32_t origin, uint64_t *newpos) { return do_seek(pos_, size_, offset, origin, newpos); }

HRESULT SubStream::Read(void *data, uint32_t size, uint32_t *processed)
{
    if (processed) *processed = 0;
    if (pos_ >= size_) return S_OK;
    if (size > size_ - pos_) size = (uint32_t)(size_ - pos_);
    HRESULT hr = base_->Seek((int64_t)(off_ + pos_), 0, nullptr);
    if (hr != S_OK) return hr;
    uint32_t got = 0;
    hr = base_->Read(data, size, &got);
    pos_ += got;
    if (processed) *processed = got;
    return hr;
}

HRESULT SubStream::Seek(int64_t offset, uint32_t origin, uint64_t *newpos) { return do_seek(pos_, size_, offset, origin, newpos); }

OutFile::~OutFile() { close_fd(); }

int OutFile::close_fd()
{
    if (fd_ < 0) return 0;
    const int r = ::close(fd_);
    fd_ = -1;
    return r == 0 ? 0 : errno;
}

HRESULT OutFile::Write(const void *data, uint32_t size, uint32_t *processed)
{
    if (processed) *processed = 0;
    const char *p = static_cast<const char *>(data);
    uint32_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd_, p + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (processed) *processed = done;
            return hr_from_errno(errno);
        }
        done += (uint32_t)n;
    }
    if (processed) *processed = done;
    return S_OK;
}

HRESULT OutFile::Seek(int64_t offset, uint32_t origin, uint64_t *newpos)
{
    const off_t r = lseek(fd_, (off_t)offset, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) return hr_from_errno(errno);
    if (newpos) *newpos = (uint64_t)r;
    return S_OK;
}

HRESULT OutFile::SetSize(uint64_t size) { return ftruncate(fd_, (off_t)size) == 0 ? S_OK : hr_from_errno(errno); }

// ---------------------------------------------------------------- results

void add_error(Result &r, Ui &ui, const std::string &msg)
{
    r.errors++;
    if (std::find(r.messages.begin(), r.messages.end(), msg) == r.messages.end()) r.messages.push_back(msg);
    ui.message(Ui::Level::Error, msg);
}

void add_warning(Result &r, Ui &ui, const std::string &msg)
{
    r.warnings++;
    if (std::find(r.messages.begin(), r.messages.end(), msg) == r.messages.end()) r.messages.push_back(msg);
    ui.message(Ui::Level::Warning, msg);
}

void finish(Result &r)
{
    if (r.cancelled) {
        r.ok = false;
        r.summary = "Operation was cancelled";
        return;
    }
    r.ok = r.errors == 0;
    if (r.ok) r.summary = r.warnings ? "Finished with warnings" : "Everything is Ok";
    else r.summary = "There are some errors";
}

bool make_dirs(const std::string &path, int &err)
{
    if (g_mkdir_with_parents(path.c_str(), 0777) == 0) return true;
    err = errno;
    return false;
}

// ---------------------------------------------------------------- opening

std::string Opened::default_item_name() const
{
    const std::string name = levels.back().name;
    const std::string ext = path_ext(name);
    std::string base = base_name(name);
    const Format *f = levels.back().fmt >= 0 ? &g_fmt[levels.back().fmt] : nullptr;
    if (!ext.empty() && f)
        for (size_t i = 0; i < f->exts.size(); i++)
            if (f->exts[i] == ext) {
                base.erase(base.size() - ext.size() - 1);
                if (i < f->add_exts.size() && f->add_exts[i] != "*") base += f->add_exts[i];
                return base;
            }
    if (!ext.empty()) base.erase(base.size() - ext.size() - 1);
    return base.empty() ? "[Content]" : base;
}

std::string ask_password_for(Ui &ui, Opened &o, bool &cancelled)
{
    if (o.password_given || o.password_asked) return o.password;
    std::string pw;
    if (!ui.ask_password(o.path, pw)) {
        cancelled = true;
        return {};
    }
    o.password = pw;
    o.password_asked = true;
    return pw;
}

namespace {

class OpenCallback final : public IArchiveOpenCallback, public IArchiveOpenVolumeCallback, public ICryptoGetTextPassword {
    SZ_COM_BEGIN
    SZ_COM_IF(IArchiveOpenCallback) SZ_COM_IF(IArchiveOpenVolumeCallback) SZ_COM_IF(ICryptoGetTextPassword)
    SZ_COM_END(IArchiveOpenCallback)

    Ui &ui;
    Opened &o;
    std::string dir, name;   // the first volume
    std::string sub_name;    // set when opening an item of another archive
    bool cancelled = false;

    OpenCallback(Ui &u, Opened &op, std::string d, std::string n) : ui(u), o(op), dir(std::move(d)), name(std::move(n)) {}

    HRESULT SetTotal(const uint64_t *, const uint64_t *) override { return S_OK; }
    HRESULT SetCompleted(const uint64_t *, const uint64_t *) override
    {
        if (ui.cancelled()) cancelled = true;
        return cancelled ? E_ABORT : S_OK;
    }
    HRESULT GetProperty(PROPID id, PROPVARIANT *value) override
    {
        Prop *p = static_cast<Prop *>(value);
        p->clear();
        if (id == kpidName) p->set_str(sub_name.empty() ? name : sub_name);
        else if (id == kpidIsDir) p->set_bool(false);
        return S_OK;
    }
    HRESULT GetStream(const wchar_t *wname, IInStream **stream) override
    {
        *stream = nullptr;
        if (!sub_name.empty()) return S_FALSE;
        const std::string n = w2s(wname);
        if (n.find('/') != std::string::npos) return S_FALSE;
        const std::string p = dir + "/" + n;
        int err = 0;
        InFile *f = InFile::open(p, err);
        if (!f) return S_FALSE;
        if (std::find(o.volumes.begin(), o.volumes.end(), p) == o.volumes.end()) o.volumes.push_back(p);
        *stream = f;
        return S_OK;
    }
    HRESULT CryptoGetTextPassword(BSTR *password) override
    {
        *password = nullptr;
        const std::string pw = ask_password_for(ui, o, cancelled);
        if (cancelled) return E_ABORT;
        *password = make_bstr(pw);
        return S_OK;
    }
};

bool sig_at(const std::string &buf, size_t off, const std::string &sig)
{
    return off + sig.size() <= buf.size() && std::memcmp(buf.data() + off, sig.data(), sig.size()) == 0;
}

std::string read_head(IInStream *s, size_t max)
{
    std::string buf(max, '\0');
    s->Seek(0, 0, nullptr);
    size_t got = 0;
    while (got < max) {
        uint32_t n = 0;
        if (s->Read(&buf[got], (uint32_t)std::min<size_t>(max - got, 1 << 20), &n) != S_OK || n == 0) break;
        got += n;
    }
    buf.resize(got);
    s->Seek(0, 0, nullptr);
    return buf;
}

// Formats to try, most likely first: by extension, then by signature or IsArc.
std::vector<int> candidates(const std::string &name, const std::string &head, bool all)
{
    std::vector<int> out;
    auto add = [&](int i) {
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    };
    const std::string ext = path_ext(name);
    if (!ext.empty())
        for (size_t i = 0; i < g_fmt.size(); i++)
            for (const auto &e : g_fmt[i].exts)
                if (e == ext) add((int)i);
    for (size_t i = 0; i < g_fmt.size(); i++) {
        const Format &f = g_fmt[i];
        if (f.flags & arc_flags::kByExtOnlyOpen) continue;
        for (const auto &s : f.sigs)
            if (sig_at(head, f.sig_offset, s)) add((int)i);
    }
    for (size_t i = 0; i < g_fmt.size(); i++) {
        const Format &f = g_fmt[i];
        if ((f.flags & arc_flags::kByExtOnlyOpen) || !f.is_arc || !f.sigs.empty()) continue;
        if (!head.empty() && f.is_arc(reinterpret_cast<const Byte *>(head.data()), head.size()) == 1) add((int)i);
    }
    if (all)
        for (size_t i = 0; i < g_fmt.size(); i++) add((int)i);
    return out;
}

std::string flags_text(uint32_t f);

// `why` gets the handler's error flags when it recognised the format but couldn't open it (a damaged archive).
HRESULT try_open(int fi, IInStream *stream, OpenCallback *cb, uint64_t max_check, Com<IInArchive> &out,
                 std::string *why = nullptr)
{
    Com<IInArchive> a;
    HRESULT hr = CreateObject(&g_fmt[fi].clsid, &IID_IInArchive, a.pv());
    if (hr != S_OK || !a) return hr == S_OK ? E_FAIL : hr;
    stream->Seek(0, 0, nullptr);
    hr = a->Open(stream, &max_check, cb);
    if (hr == S_OK) {
        out = a;
        return hr;
    }
    if (why && why->empty()) {
        Prop p;
        uint64_t v;
        if (a->GetArchiveProperty(kpidErrorFlags, &p) == S_OK && p.get_u64(v) && (v & ~(uint64_t)error_flags::IsNotArc))
            *why = "Cannot open the file as " + g_fmt[fi].name + " archive: " + flags_text((uint32_t)(v & ~1u));
    }
    a->Close();
    return hr;
}

uint64_t arc_u64(IInArchive *a, PROPID id, uint64_t def)
{
    Prop p;
    uint64_t v;
    if (a->GetArchiveProperty(id, &p) == S_OK && p.get_u64(v)) return v;
    return def;
}

std::string flags_text(uint32_t f)
{
    std::string s;
    auto add = [&](uint32_t bit, const char *t) {
        if (f & bit) s += (s.empty() ? "" : ", ") + std::string(t);
    };
    add(error_flags::IsNotArc, "Is not archive");
    add(error_flags::HeadersError, "Headers Error");
    add(error_flags::EncryptedHeadersError, "Headers Error in encrypted archive. Wrong password?");
    add(error_flags::UnavailableStart, "Unavailable start of archive");
    add(error_flags::UnconfirmedStart, "Unconfirmed start of archive");
    add(error_flags::UnexpectedEnd, "Unexpected end of archive");
    add(error_flags::DataAfterEnd, "There are data after the end of archive");
    add(error_flags::UnsupportedMethod, "Unsupported method");
    add(error_flags::UnsupportedFeature, "Unsupported feature");
    add(error_flags::DataError, "Data Error");
    add(error_flags::CrcError, "CRC Error");
    return s;
}

void collect_level_messages(Opened &o, IInArchive *a)
{
    Prop p;
    uint64_t v;
    std::string s;
    if (a->GetArchiveProperty(kpidErrorFlags, &p) == S_OK && p.get_u64(v) && v) o.errors.push_back(flags_text((uint32_t)v));
    p.clear();
    if (a->GetArchiveProperty(kpidError, &p) == S_OK && p.get_str(s) && !s.empty()) o.errors.push_back(s);
    p.clear();
    if (a->GetArchiveProperty(kpidWarningFlags, &p) == S_OK && p.get_u64(v) && v) o.warnings.push_back(flags_text((uint32_t)v));
    p.clear();
    if (a->GetArchiveProperty(kpidWarning, &p) == S_OK && p.get_str(s) && !s.empty()) o.warnings.push_back(s);
}

// Open Inside #: finds archives by their signatures anywhere in the file.
bool open_parser(Opened &o, Com<IInStream> file, uint64_t file_size, OpenCallback *cb, Ui &ui, bool &cancelled)
{
    std::vector<std::vector<std::pair<int, const std::string *>>> by_byte(256);
    for (size_t i = 0; i < g_fmt.size(); i++)
        for (const auto &s : g_fmt[i].sigs)
            if (s.size() >= 2 && !(g_fmt[i].flags & arc_flags::kByExtOnlyOpen))
                by_byte[(unsigned char)s[0]].emplace_back((int)i, &s);
    const size_t chunk = 1 << 20, overlap = 64;
    uint64_t base = 0, skip_to = 0;
    std::string buf;
    while (base < file_size) {
        if (ui.cancelled()) {
            cancelled = true;
            return false;
        }
        buf.assign((size_t)std::min<uint64_t>(chunk + overlap, file_size - base), '\0');
        file->Seek((int64_t)base, 0, nullptr);
        size_t got = 0;
        while (got < buf.size()) {
            uint32_t n = 0;
            if (file->Read(&buf[got], (uint32_t)(buf.size() - got), &n) != S_OK || n == 0) break;
            got += n;
        }
        buf.resize(got);
        if (got == 0) break;
        const size_t limit = std::min(buf.size(), chunk);
        for (size_t i = 0; i < limit; i++) {
            const uint64_t pos = base + i;
            if (pos < skip_to) continue;
            for (const auto &c : by_byte[(unsigned char)buf[i]]) {
                const Format &f = g_fmt[c.first];
                if (!sig_at(buf, i, *c.second) || pos < f.sig_offset) continue;
                const uint64_t start = pos - f.sig_offset;
                Com<IInStream> sub(new SubStream(file, start, file_size - start));
                Com<IInArchive> a;
                if (try_open(c.first, sub.get(), cb, 0, a) != S_OK) continue;
                const uint64_t phy = arc_u64(a.get(), kpidPhySize, file_size - start);
                a->Close();
                o.embedded.push_back({start, phy ? phy : file_size - start, c.first});
                skip_to = start + std::max<uint64_t>(phy, 1);
                break;
            }
        }
        base += limit;
    }
    return !o.embedded.empty();
}

std::string attrib_text(uint32_t attrib, bool has_attrib, uint32_t posix, bool has_posix, bool dir)
{
    std::string s;
    if (has_attrib) {
        static const char letters[] = "RHS_DA";  // bits 0..5 (bit 3 unused)
        for (int b = 0; b < 6; b++)
            if (b != 3 && (attrib & (1u << b))) s += letters[b];
        if (attrib & 0x8000) {
            posix = attrib >> 16;
            has_posix = true;
        }
    }
    if (has_posix) {
        if (!s.empty()) s += ' ';
        const uint32_t t = posix & 0170000;
        s += t == 0040000 ? 'd' : t == 0120000 ? 'l' : t == 0060000 ? 'b' : t == 0020000 ? 'c' : t == 0010000 ? 'p'
           : t == 0140000 ? 's' : t == 0 && dir ? 'd' : '-';
        const char *rwx = "rwxrwxrwx";
        for (int i = 0; i < 9; i++) s += (posix & (0400u >> i)) ? rwx[i] : '-';
        if (posix & 04000) s[s.size() - 7] = (posix & 0100) ? 's' : 'S';
        if (posix & 02000) s[s.size() - 4] = (posix & 010) ? 's' : 'S';
        if (posix & 01000) s[s.size() - 1] = (posix & 01) ? 't' : 'T';
    }
    return s;
}

std::string clean_path(std::string p, bool &dir)
{
    while (starts_with(p, "./")) p.erase(0, 2);
    for (size_t k; (k = p.find("/./")) != std::string::npos;) p.erase(k, 2);
    for (size_t k; (k = p.find("//")) != std::string::npos;) p.erase(k, 1);
    while (!p.empty() && p.front() == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
        dir = true;
    }
    if (p == ".") p.clear();
    return p;
}

}  // namespace

bool open(const std::string &path, const std::string &type, Ui &ui, const std::string *password, Opened &o,
          std::string &error, bool &cancelled)
{
    cancelled = false;
    o.path = path;
    if (password) {
        o.password = *password;
        o.password_given = !password->empty();
    }
    if (!g_lib.ok) {
        error = "7-Zip's library (7z.so) is not usable: " + g_load_error;
        return false;
    }
    int err = 0;
    InFile *raw = InFile::open(path, err);
    if (!raw) {
        error = path + " : " + errno_text(err);
        return false;
    }
    Com<IInStream> file(raw);
    const uint64_t file_size = raw->size();
    Com<OpenCallback> cb(new OpenCallback(ui, o, dir_name(path), base_name(path)));

    if (type == "#") {
        o.parser = true;
        if (open_parser(o, file, file_size, cb.get(), ui, cancelled)) {
            o.levels.push_back({{}, file, -1, base_name(path)});
            return true;
        }
        if (!cancelled) error = "Cannot open the file '" + path + "' as archive";
        return false;
    }

    std::vector<int> cand;
    if (!type.empty() && type != "*") {
        const int fi = find_format(type);
        if (fi < 0) {
            error = "Unsupported archive type: " + type;
            return false;
        }
        cand.push_back(fi);
    } else {
        cand = candidates(path, read_head(file.get(), 1 << 20), type == "*");
    }

    Com<IInArchive> arc;
    int fmt = -1;
    HRESULT last = S_FALSE;
    std::string why;
    for (int fi : cand) {
        last = try_open(fi, file.get(), cb.get(), 0, arc, &why);
        if (last == S_OK) {
            fmt = fi;
            break;
        }
        if (last == E_ABORT || cb->cancelled) {
            cancelled = true;
            return false;
        }
        if (o.password_asked) break;  // encrypted, and the password didn't open it
    }
    // A self-extracting stub (PE/ELF) found first: look for an archive after it.
    if (fmt >= 0 && (g_fmt[fmt].flags & arc_flags::kPreArc) && type.empty()) {
        for (size_t i = 0; i < g_fmt.size(); i++) {
            if ((int)i == fmt || !(g_fmt[i].flags & arc_flags::kFindSignature)) continue;
            Com<IInArchive> a;
            if (try_open((int)i, file.get(), cb.get(), (uint64_t)1 << 22, a) == S_OK) {
                arc->Close();
                arc = a;
                fmt = (int)i;
                break;
            }
        }
    }
    if (fmt < 0) {
        if (o.password_asked || o.password_given) error = "Cannot open encrypted archive '" + path + "'. Wrong password?";
        else if (last != S_FALSE && last != S_OK) error = path + " : " + hr_text(last);
        else error = "Cannot open the file '" + path + "' as archive";
        if (!why.empty() && !o.password_asked) error += "\n" + why;
        return false;
    }
    o.levels.push_back({arc, file, fmt, base_name(path)});
    collect_level_messages(o, arc.get());

    // Archives whose content is one archive that can be read randomly (split volumes) open through to it.
    for (int depth = 0; depth < 4; depth++) {
        IInArchive *a = o.arc();
        Prop p;
        uint64_t sub_index;
        if (a->GetArchiveProperty(kpidMainSubfile, &p) != S_OK || !p.get_u64(sub_index)) break;
        Com<IInArchiveGetStream> gs = o.levels.back().arc.query<IInArchiveGetStream>(IID_IInArchiveGetStream);
        if (!gs) break;
        Com<ISequentialInStream> seq;
        if (gs->GetStream((uint32_t)sub_index, seq.put()) != S_OK || !seq) break;
        Com<IInStream> sub = seq.query<IInStream>(IID_IInStream);
        if (!sub) break;
        Entry e;
        read_entry(a, (uint32_t)sub_index, o.default_item_name(), e);
        const std::string sub_name = base_name(e.path);
        Com<OpenCallback> scb(new OpenCallback(ui, o, dir_name(path), base_name(path)));
        scb->sub_name = sub_name;
        Com<IInArchive> inner;
        int ifmt = -1;
        for (int fi : candidates(sub_name, read_head(sub.get(), 1 << 16), false)) {
            const HRESULT hr = try_open(fi, sub.get(), scb.get(), 0, inner);
            if (hr == S_OK) {
                ifmt = fi;
                break;
            }
            if (hr == E_ABORT) {
                cancelled = true;
                return false;
            }
        }
        if (ifmt < 0) break;
        o.levels.push_back({inner, sub, ifmt, sub_name});
        collect_level_messages(o, inner.get());
    }
    return true;
}

bool read_entry(IInArchive *a, uint32_t i, const std::string &default_name, Entry &e)
{
    e = Entry();
    e.index = i;
    Prop p;
    std::string s;
    uint64_t v;
    int64_t t;
    if (a->GetProperty(i, kpidIsDir, &p) == S_OK) e.dir = p.get_bool();
    p.clear();
    if (a->GetProperty(i, kpidPath, &p) == S_OK && p.get_str(s)) e.path = clean_path(s, e.dir);
    if (e.path.empty() && !e.dir) e.path = default_name;
    p.clear();
    if (a->GetProperty(i, kpidSize, &p) == S_OK && p.get_u64(v)) e.size = v, e.has_size = true;
    p.clear();
    if (a->GetProperty(i, kpidPackSize, &p) == S_OK && p.get_u64(v)) e.packed = v, e.has_packed = true;
    p.clear();
    if (a->GetProperty(i, kpidMTime, &p) == S_OK && p.get_time(t)) e.mtime = t, e.has_mtime = true;
    p.clear();
    if (a->GetProperty(i, kpidCTime, &p) == S_OK && p.get_time(t)) e.ctime = t, e.has_ctime = true;
    p.clear();
    if (a->GetProperty(i, kpidATime, &p) == S_OK && p.get_time(t)) e.atime = t, e.has_atime = true;
    p.clear();
    uint64_t attrib = 0, posix = 0;
    bool has_attrib = false, has_posix = false;
    if (a->GetProperty(i, kpidAttrib, &p) == S_OK && p.get_u64(attrib)) has_attrib = true;
    p.clear();
    if (a->GetProperty(i, kpidPosixAttrib, &p) == S_OK && p.get_u64(posix)) has_posix = true;
    p.clear();
    if (has_attrib && (attrib & 0x10)) e.dir = true;
    if (has_posix && (posix & 0170000) == 0040000) e.dir = true;
    e.attrib = attrib_text((uint32_t)attrib, has_attrib, (uint32_t)posix, has_posix, e.dir);
    if (a->GetProperty(i, kpidCRC, &p) == S_OK && p.get_u64(v)) e.crc = (uint32_t)v, e.has_crc = true;
    p.clear();
    if (a->GetProperty(i, kpidEncrypted, &p) == S_OK) e.encrypted = p.get_bool();
    p.clear();
    if (a->GetProperty(i, kpidMethod, &p) == S_OK) p.get_str(e.method);
    p.clear();
    if (a->GetProperty(i, kpidComment, &p) == S_OK) p.get_str(e.comment);
    p.clear();
    if (a->GetProperty(i, kpidUser, &p) == S_OK) p.get_str(e.user);
    p.clear();
    if (a->GetProperty(i, kpidGroup, &p) == S_OK) p.get_str(e.group);
    p.clear();
    if (a->GetProperty(i, kpidSymLink, &p) == S_OK) p.get_str(e.link);
    p.clear();
    if (e.link.empty() && a->GetProperty(i, kpidHardLink, &p) == S_OK) p.get_str(e.link);
    return !e.path.empty();
}

// ---------------------------------------------------------------- extracting

namespace {

struct ItemMode {
    bool has = false;
    uint32_t mode = 0;  // POSIX st_mode
    bool readonly = false;
};

ItemMode item_mode(IInArchive *a, uint32_t i)
{
    ItemMode m;
    Prop p;
    uint64_t v;
    if (a->GetProperty(i, kpidPosixAttrib, &p) == S_OK && p.get_u64(v)) {
        m.has = true;
        m.mode = (uint32_t)v;
    }
    p.clear();
    if (a->GetProperty(i, kpidAttrib, &p) == S_OK && p.get_u64(v)) {
        if (!m.has && (v & 0x8000)) {
            m.has = true;
            m.mode = (uint32_t)(v >> 16);
        }
        m.readonly = (v & 1) != 0;
    }
    return m;
}

std::string unique_name(const std::string &path)
{
    const std::string dir = dir_name(path), base = base_name(path);
    const auto dot = base.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
    const std::string ext = dot == std::string::npos || dot == 0 ? "" : base.substr(dot);
    for (unsigned n = 1;; n++) {
        const std::string c = dir + "/" + stem + "_" + std::to_string(n) + ext;
        struct stat st;
        if (lstat(c.c_str(), &st) != 0) return c;
    }
}

// Removes ".", ".." and empty components so that nothing is written outside the output folder.
std::string safe_rel(const std::string &p)
{
    std::string out;
    gchar **v = g_strsplit(p.c_str(), "/", -1);
    for (gchar **c = v; *c; c++) {
        if (!**c || std::strcmp(*c, ".") == 0 || std::strcmp(*c, "..") == 0) continue;
        if (!out.empty()) out += '/';
        out += *c;
    }
    g_strfreev(v);
    return out;
}

// A link may only point inside the output folder (like 7-Zip without -snld): otherwise later items could be
// written through it to anywhere.
bool link_is_safe(const std::string &rel, const std::string &target)
{
    if (target.empty() || target[0] == '/') return false;
    long depth = 0;
    for (size_t k = rel.find('/'); k != std::string::npos; k = rel.find('/', k + 1)) depth++;
    gchar **v = g_strsplit(target.c_str(), "/", -1);
    bool ok = true;
    for (gchar **c = v; *c && ok; c++) {
        if (!**c || std::strcmp(*c, ".") == 0) continue;
        if (std::strcmp(*c, "..") == 0) ok = --depth >= 0;
        else depth++;
    }
    g_strfreev(v);
    return ok;
}

void set_times(const std::string &path, const Entry &e, bool nofollow)
{
    if (!e.has_mtime) return;
    struct timespec ts[2];
    ts[0].tv_sec = e.has_atime ? (time_t)e.atime : (time_t)e.mtime;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = (time_t)e.mtime;
    ts[1].tv_nsec = 0;
    utimensat(AT_FDCWD, path.c_str(), ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
}

class ExtractCallback final : public IArchiveExtractCallback,
                              public IArchiveExtractCallbackMessage2,
                              public ICryptoGetTextPassword,
                              public IArchiveRequestMemoryUseCallback {
    SZ_COM_BEGIN
    SZ_COM_IF(IArchiveExtractCallback) SZ_COM_IF(IProgress) SZ_COM_IF(IArchiveExtractCallbackMessage2)
    SZ_COM_IF(ICryptoGetTextPassword) SZ_COM_IF(IArchiveRequestMemoryUseCallback)
    SZ_COM_END(IArchiveExtractCallback)

    enum class Kind { None, File, SymlinkData };

    Ui &ui;
    Opened &o;
    const ExtractParams &p;
    Result &r;
    IInArchive *arc;
    std::string default_name;
    bool cancelled = false, memory_refused = false;
    int overwrite;

    Entry cur;
    ItemMode cur_mode;
    std::string dest, rel;
    Kind kind = Kind::None;
    bool counting = false;
    Com<MemOut> link_buf;
    std::vector<std::pair<std::string, Entry>> dirs;
    std::vector<std::pair<std::string, ItemMode>> dir_modes;

    ExtractCallback(Ui &u, Opened &op, const ExtractParams &pr, Result &res)
        : ui(u), o(op), p(pr), r(res), arc(op.arc()), default_name(op.default_item_name()), overwrite(pr.overwrite) {}

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

    void error(const std::string &m) { add_error(r, ui, m); }

    bool is_absolute_in_archive(uint32_t index)
    {
        Prop pp;
        std::string s;
        return arc->GetProperty(index, kpidPath, &pp) == S_OK && pp.get_str(s) && !s.empty() && s[0] == '/';
    }

    HRESULT GetStream(uint32_t index, ISequentialOutStream **out, int32_t mode) override
    {
        *out = nullptr;
        kind = Kind::None;
        counting = false;
        dest.clear();
        if (ui.cancelled()) {
            cancelled = true;
            return E_ABORT;
        }
        read_entry(arc, index, default_name, cur);
        if (mode == ask_mode::kTest) {
            counting = !cur.dir;
            ui.set_current("Testing", cur.path);
            return S_OK;
        }
        if (mode != ask_mode::kExtract) return S_OK;

        rel = cur.path;
        if (!p.strip_prefix.empty()) {
            if (rel == p.strip_prefix) return S_OK;
            if (starts_with(rel, p.strip_prefix + "/")) rel = rel.substr(p.strip_prefix.size() + 1);
        }
        if (p.path_mode == 1) {
            if (cur.dir) return S_OK;
            rel = base_name(rel);
        }
        rel = safe_rel(rel);
        if (rel.empty()) return S_OK;
        dest = (p.path_mode == 2 && p.strip_prefix.empty() && is_absolute_in_archive(index)) ? "/" + rel
                                                                                              : p.out_dir + "/" + rel;
        int err = 0;
        if (cur.dir) {
            if (!make_dirs(dest, err)) {
                error("Cannot create folder : " + errno_text(err) + " : " + dest);
            } else {
                dirs.emplace_back(dest, cur);
                dir_modes.emplace_back(dest, item_mode(arc, index));
            }
            return S_OK;
        }
        if (!make_dirs(dir_name(dest), err)) {
            error("Cannot create folder : " + errno_text(err) + " : " + dir_name(dest));
            return S_OK;
        }
        struct stat st;
        if (lstat(dest.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                error("Cannot replace the folder with a file : " + dest);
                return S_OK;
            }
            int action = overwrite;  // 1 overwrite, 2 skip, 3 rename new, 4 rename existing
            if (action == 0) {
                switch (ui.ask_overwrite(dest, (uint64_t)st.st_size, (int64_t)st.st_mtime, cur.path, cur.size,
                                         cur.has_mtime ? cur.mtime : 0)) {
                case Ui::Overwrite::Yes: action = 1; break;
                case Ui::Overwrite::YesAll: action = overwrite = 1; break;
                case Ui::Overwrite::No: action = 2; break;
                case Ui::Overwrite::NoAll: action = overwrite = 2; break;
                case Ui::Overwrite::Rename: action = overwrite = 3; break;
                case Ui::Overwrite::Cancel: cancelled = true; return E_ABORT;
                }
            }
            if (action == 2) return S_OK;
            if (action == 3) {
                dest = unique_name(dest);
            } else if (action == 4) {
                const std::string moved = unique_name(dest);
                if (rename(dest.c_str(), moved.c_str()) != 0) {
                    error("Cannot rename existing file : " + errno_text(errno) + " : " + dest);
                    return S_OK;
                }
            } else if (unlink(dest.c_str()) != 0) {
                error("Cannot delete output file : " + errno_text(errno) + " : " + dest);
                return S_OK;
            }
        }
        ui.set_current("Extracting", cur.path);
        counting = true;

        cur_mode = item_mode(arc, index);
        Prop pl;
        std::string hard, sym;
        if (arc->GetProperty(index, kpidHardLink, &pl) == S_OK) pl.get_str(hard);
        pl.clear();
        if (arc->GetProperty(index, kpidSymLink, &pl) == S_OK) pl.get_str(sym);
        if (!hard.empty()) {
            bool d = false;
            std::string target = safe_rel(clean_path(hard, d));
            if (!p.strip_prefix.empty() && starts_with(target, p.strip_prefix + "/"))
                target = target.substr(p.strip_prefix.size() + 1);
            const std::string tp = p.out_dir + "/" + target;
            if (link(tp.c_str(), dest.c_str()) != 0) error("Cannot create hard link : " + errno_text(errno) + " : " + dest);
            return S_OK;
        }
        const bool is_link = cur_mode.has && (cur_mode.mode & 0170000) == 0120000;
        if (!sym.empty() && (is_link || !cur_mode.has)) {
            if (!link_is_safe(rel, sym)) error("Dangerous link path was ignored : " + cur.path + " : " + sym);
            else if (symlink(sym.c_str(), dest.c_str()) != 0) error("Cannot create symbolic link : " + errno_text(errno) + " : " + dest);
            else set_times(dest, cur, true);
            return S_OK;
        }
        if (is_link) {
            link_buf = Com<MemOut>(new MemOut);
            kind = Kind::SymlinkData;
            link_buf->AddRef();
            *out = link_buf.get();
            return S_OK;
        }
        const int fd = ::open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) {
            error("Cannot open output file : " + errno_text(errno) + " : " + dest);
            return S_OK;
        }
        kind = Kind::File;
        *out = new OutFile(fd);
        return S_OK;
    }

    HRESULT PrepareOperation(int32_t) override { return S_OK; }

    HRESULT SetOperationResult(int32_t result) override
    {
        if (kind == Kind::SymlinkData && link_buf) {
            if (!link_is_safe(rel, link_buf->data))
                error("Dangerous link path was ignored : " + cur.path + " : " + link_buf->data);
            else if (symlink(link_buf->data.c_str(), dest.c_str()) != 0)
                error("Cannot create symbolic link : " + errno_text(errno) + " : " + dest);
            else set_times(dest, cur, true);
            link_buf.reset();
        } else if (kind == Kind::File) {
            set_times(dest, cur, false);
            if (cur_mode.has) {
                chmod(dest.c_str(), cur_mode.mode & 0777);
            } else if (cur_mode.readonly) {
                struct stat st;
                if (stat(dest.c_str(), &st) == 0) chmod(dest.c_str(), st.st_mode & 0555);
            }
        }
        if (result != op_result::kOK) error(op_result_text(result, cur.encrypted) + " : " + cur.path);
        if (counting) {
            r.files++;
            r.size += cur.size;
            ui.file_done();
        }
        kind = Kind::None;
        counting = false;
        return S_OK;
    }

    HRESULT ReportExtractResult(uint32_t index_type, uint32_t index, int32_t result) override
    {
        if (result == op_result::kOK) return S_OK;
        std::string name;
        if (index_type == 1) {
            Entry e;
            read_entry(arc, index, default_name, e);
            name = e.path;
        }
        error(op_result_text(result, false) + (name.empty() ? "" : " : " + name));
        return S_OK;
    }

    HRESULT CryptoGetTextPassword(BSTR *password) override
    {
        *password = nullptr;
        const std::string pw = ask_password_for(ui, o, cancelled);
        if (cancelled) return E_ABORT;
        *password = make_bstr(pw);
        return S_OK;
    }

    HRESULT RequestMemoryUse(uint32_t flags, uint32_t, uint32_t, const wchar_t *, uint64_t required, uint64_t *allowed,
                             uint32_t *answer) override
    {
        if (g_memory_limit == 0 || required <= g_memory_limit || ui.ask_memory(required, g_memory_limit, o.path)) {
            if (allowed) *allowed = required;
            *answer = mem_answer::k_Allow;
            return S_OK;
        }
        memory_refused = true;
        *answer = mem_answer::k_Limit_Exceeded |
                  ((flags & mem_flags::k_SkipArc_IsExpected) ? mem_answer::k_SkipArc : mem_answer::k_Stop);
        error("Memory usage limit was exceeded: " + std::to_string((required + (1u << 30) - 1) >> 30) +
              " GB is required to unpack " + base_name(o.path));
        return S_OK;
    }
};

}  // namespace

void extract(Opened &o, const std::vector<uint32_t> &indices, const ExtractParams &p, Ui &ui, Result &r)
{
    // Problems found while opening (a damaged or truncated archive) belong to the result too.
    for (const auto &m : o.errors) add_error(r, ui, base_name(o.path) + " : " + m);
    for (const auto &m : o.warnings) add_warning(r, ui, base_name(o.path) + " : " + m);
    Com<ExtractCallback> cb(new ExtractCallback(ui, o, p, r));
    const HRESULT hr = o.arc()->Extract(indices.empty() ? nullptr : indices.data(),
                                        indices.empty() ? (uint32_t)-1 : (uint32_t)indices.size(), p.test ? 1 : 0, cb.get());
    if (hr == E_ABORT && !cb->memory_refused) r.cancelled = true;
    else if (hr != S_OK && hr != E_ABORT) add_error(r, ui, hr_text(hr) + " : " + o.path);
    // Folder times and modes last, deepest first, once their content is written.
    for (auto it = cb->dirs.rbegin(); it != cb->dirs.rend(); ++it) set_times(it->first, it->second, false);
    for (auto it = cb->dir_modes.rbegin(); it != cb->dir_modes.rend(); ++it)
        if (it->second.has) chmod(it->first.c_str(), (it->second.mode & 0777) | 0700);
    r.folders += cb->dirs.size();
}

}  // namespace impl

// ================================================================ public API

using namespace impl;

void set_memory_limit(uint64_t bytes) { g_memory_limit = bytes; }

namespace {

// The file 7z.so was loaded from (the dynamic loader found it through the RUNPATH set at build time).
std::string library_path()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&CreateObject), &info) && info.dli_fname) return info.dli_fname;
    return "7z.so";
}

std::string handler_str(uint32_t i, PROPID id)
{
    Prop p;
    std::string s;
    if (GetHandlerProperty2(i, id, &p) == S_OK) p.get_str(s);
    return s;
}

std::string handler_bin(uint32_t i, PROPID id)
{
    Prop p;
    if (GetHandlerProperty2(i, id, &p) != S_OK || p.vt != VT_BSTR || !p.bstrVal) return {};
    const uint32_t n = SysStringByteLen(p.bstrVal);
    return std::string(reinterpret_cast<const char *>(p.bstrVal), n);
}

std::vector<std::string> words(const std::string &s)
{
    std::vector<std::string> out;
    gchar **v = g_strsplit(s.c_str(), " ", -1);
    for (gchar **p = v; *p; p++)
        if (**p) out.emplace_back(*p);
    g_strfreev(v);
    return out;
}

}  // namespace

void init(const char *)
{
    if (g_inited) return;
    g_inited = true;
    g_lib.path = library_path();
    {
        Prop p;
        uint64_t v = 1;
        // Interface type 0: IUnknown without a virtual destructor (7-Zip 23+). p7zip-style libraries differ.
        if (GetModuleProp(module_prop::kInterfaceType, &p) == S_OK) p.get_u64(v);
        if (v != 0) {
            g_load_error = g_lib.path + " uses an incompatible interface (p7zip?)";
            return;
        }
        p.clear();
        if (GetModuleProp(module_prop::kVersion, &p) == S_OK && p.get_u64(v)) g_lib.version = (unsigned)v;
    }
    g_lib.ok = true;
    char vb[32];
    std::snprintf(vb, sizeof vb, "%u.%02u", g_lib.version >> 16, g_lib.version & 0xFFFF);
    g_version = vb;

    uint32_t n = 0;
    GetNumberOfFormats(&n);
    for (uint32_t i = 0; i < n; i++) {
        Format f;
        f.name = handler_str(i, handler_prop::kName);
        const std::string clsid = handler_bin(i, handler_prop::kClassID);
        if (f.name.empty() || clsid.size() < sizeof(GUID)) continue;
        std::memcpy(&f.clsid, clsid.data(), sizeof(GUID));
        f.exts = words(lower(handler_str(i, handler_prop::kExtension)));
        f.add_exts = words(handler_str(i, handler_prop::kAddExtension));
        Prop p;
        uint64_t v;
        if (GetHandlerProperty2(i, handler_prop::kUpdate, &p) == S_OK) f.update = p.get_bool();
        p.clear();
        if (GetHandlerProperty2(i, handler_prop::kFlags, &p) == S_OK && p.get_u64(v)) f.flags = (uint32_t)v;
        p.clear();
        if (GetHandlerProperty2(i, handler_prop::kSignatureOffset, &p) == S_OK && p.get_u64(v)) f.sig_offset = (uint32_t)v;
        const std::string sig = handler_bin(i, handler_prop::kSignature);
        if (!sig.empty()) f.sigs.push_back(sig);
        const std::string multi = handler_bin(i, handler_prop::kMultiSignature);
        for (size_t k = 0; k < multi.size();) {
            const size_t len = (unsigned char)multi[k];
            if (k + 1 + len > multi.size()) break;
            f.sigs.push_back(multi.substr(k + 1, len));
            k += 1 + len;
        }
        GetIsArc(i, &f.is_arc);
        g_fmt.push_back(std::move(f));
    }

    {
        Com<IHashers> hs;
        if (GetHashers(hs.put()) == S_OK && hs) {
            const uint32_t nh = hs->GetNumHashers();
            for (uint32_t i = 0; i < nh; i++) {
                Prop p;
                std::string s;
                if (hs->GetHasherProp(i, method_prop::kName, &p) == S_OK && p.get_str(s) && !s.empty())
                    g_hashers.push_back(s);
            }
        }
    }
}

bool available() { return g_lib.ok; }
std::string program() { return g_lib.ok ? g_lib.path : g_load_error; }
std::string version() { return g_version; }

std::vector<FormatInfo> formats()
{
    std::vector<FormatInfo> out;
    for (const auto &f : g_fmt) out.push_back({f.name, f.exts, f.update});
    return out;
}

std::vector<std::string> hash_methods() { return g_hashers; }

std::vector<std::string> update_formats()
{
    // The Add to Archive dialog's list (7-Zip FM shows the formats that can create archives).
    static const char *const order[] = {"7z", "zip", "tar", "wim", "gzip", "bzip2", "xz", "zstd"};
    std::vector<std::string> out;
    for (const char *n : order)
        for (const auto &f : g_fmt)
            if (f.update && f.name == n) out.push_back(f.name);
    return out;
}

bool looks_like_archive(const std::string &path)
{
    const std::string name = base_name(path);
    const std::string ext = path_ext(name);
    if (ext.empty()) return false;
    for (const auto &f : g_fmt)
        for (const auto &e : f.exts)
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
    Opened o;
    std::string error;
    bool cancelled = false;
    if (!impl::open(path, type, ui, password.empty() ? nullptr : &password, o, error, cancelled)) {
        out.cancelled = cancelled;
        out.error = cancelled ? "Operation was cancelled" : error;
        return out;
    }
    if (o.parser) {
        out.format = "#";
        uint32_t k = 0;
        for (const auto &e : o.embedded) {
            Entry en;
            en.index = k;
            const Format &f = g_fmt[e.fmt];
            en.path = std::to_string(k++) + "." + (f.exts.empty() ? lower(f.name) : f.exts[0]);
            en.size = en.packed = e.size;
            en.has_size = en.has_packed = true;
            en.method = f.name;
            en.comment = "Offset " + std::to_string(e.offset);
            out.entries.push_back(std::move(en));
        }
        out.props.emplace_back("Path", path);
        out.props.emplace_back("Type", "Parser");
        out.ok = true;
        return out;
    }

    for (size_t lv = 0; lv < o.levels.size(); lv++) {
        IInArchive *a = o.levels[lv].arc.get();
        if (o.levels.size() > 1) out.props.emplace_back("--", o.levels[lv].name);
        out.props.emplace_back("Type", g_fmt[o.levels[lv].fmt].name);
        uint32_t np = 0;
        a->GetNumberOfArchiveProperties(&np);
        for (uint32_t i = 0; i < np; i++) {
            BSTR name = nullptr;
            PROPID id = 0;
            VARTYPE vt = 0;
            if (a->GetArchivePropertyInfo(i, &name, &id, &vt) != S_OK) continue;
            std::string label = prop_name(id);
            if (label.empty() && name) label = w2s(name);
            if (name) SysFreeString(name);
            if (id == kpidErrorFlags || id == kpidWarningFlags || id == kpidError || id == kpidWarning) continue;
            Prop p;
            if (a->GetArchiveProperty(id, &p) != S_OK || p.empty()) continue;
            const std::string v = prop_display(id, p);
            if (label.empty()) label = "Property " + std::to_string(id);
            if (!v.empty()) out.props.emplace_back(label, v);
        }
    }
    if (!o.volumes.empty()) out.props.emplace_back("Volumes", std::to_string(o.volumes.size() + 1));
    for (const auto &m : o.errors) out.props.emplace_back("Error", m);
    for (const auto &m : o.warnings) out.props.emplace_back("Warning", m);

    IInArchive *a = o.arc();
    uint32_t n = 0;
    a->GetNumberOfItems(&n);
    const std::string def = o.default_item_name();
    out.entries.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Entry e;
        if (read_entry(a, i, def, e)) out.entries.push_back(std::move(e));
    }
    out.format = g_fmt[o.fmt()].name;
    out.can_update = g_fmt[o.fmt()].update && o.levels.size() == 1 && o.volumes.empty();
    if (o.password_asked || o.password_given) out.password = o.password;
    out.ok = true;
    return out;
}

Result extract_items(const ExtractRequest &req, Ui &ui)
{
    Result r;
    Opened o;
    std::string error;
    bool cancelled = false;
    if (!impl::open(req.archive, "", ui, req.password.empty() ? nullptr : &req.password, o, error, cancelled)) {
        r.cancelled = cancelled;
        if (!cancelled) add_error(r, ui, error);
        finish(r);
        return r;
    }
    if (o.parser) {
        add_error(r, ui, "Cannot extract from this view");
        finish(r);
        return r;
    }
    std::vector<uint32_t> indices;
    if (!req.items.empty()) {
        const std::set<std::string> want(req.items.begin(), req.items.end());
        IInArchive *a = o.arc();
        uint32_t n = 0;
        a->GetNumberOfItems(&n);
        const std::string def = o.default_item_name();
        for (uint32_t i = 0; i < n; i++) {
            Entry e;
            if (!read_entry(a, i, def, e)) continue;
            // The item itself or one of its parent folders is selected.
            bool hit = want.count(e.path) > 0;
            for (size_t k = e.path.find('/'); !hit && k != std::string::npos; k = e.path.find('/', k + 1))
                hit = want.count(e.path.substr(0, k)) > 0;
            if (hit) indices.push_back(i);
        }
        if (indices.empty()) {
            finish(r);
            return r;
        }
    }
    ExtractParams p;
    p.out_dir = req.out_dir;
    p.strip_prefix = req.strip_prefix;
    p.path_mode = req.path_mode;
    p.overwrite = req.overwrite;
    p.test = req.test;
    if (!p.test) {
        int err = 0;
        if (!make_dirs(p.out_dir, err)) {
            add_error(r, ui, "Cannot create folder : " + errno_text(err) + " : " + p.out_dir);
            finish(r);
            return r;
        }
    }
    extract(o, indices, p, ui, r);
    finish(r);
    return r;
}

}  // namespace szl
