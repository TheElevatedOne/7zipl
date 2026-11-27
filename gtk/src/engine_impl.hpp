// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: internals shared by engine.cpp (library, opening, listing, extracting) and engine_ops.cpp
// (updating, hashing, benchmark, the command runner). Everything goes through 7-Zip's 7z.so.
#pragma once

#include "engine.hpp"
#include "sevenzip.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace szl::impl {

using namespace sz;

struct Lib {
    bool ok = false;  // 7z.so has a compatible interface
    std::string path;
    unsigned version = 0;  // (major << 16) | minor
};
extern Lib g_lib;
extern uint64_t g_memory_limit;

struct Format {
    std::string name;
    GUID clsid{};
    std::vector<std::string> exts, add_exts;
    bool update = false;
    uint32_t flags = 0;
    std::vector<std::string> sigs;
    uint32_t sig_offset = 0;
    Func_IsArc is_arc = nullptr;
};
extern std::vector<Format> g_fmt;
int find_format(const std::string &name);  // case-insensitive, -1 if unknown

// ---- strings and properties ----
std::string w2s(const wchar_t *w, size_t n = (size_t)-1);
std::wstring s2w(const std::string &s);
BSTR make_bstr(const std::string &s);
std::string lower(const std::string &s);
std::string base_name(const std::string &p);
std::string dir_name(const std::string &p);
std::string path_ext(const std::string &name);  // lower-case extension without the dot
bool starts_with(const std::string &s, const std::string &p);
std::string errno_text(int e);
std::string hr_text(HRESULT hr);
std::string op_result_text(int32_t r, bool encrypted);

struct Prop : PROPVARIANT {
    Prop() { std::memset(static_cast<PROPVARIANT *>(this), 0, sizeof(PROPVARIANT)); }
    ~Prop() { clear(); }
    Prop(const Prop &) = delete;
    Prop &operator=(const Prop &) = delete;
    void clear();
    bool empty() const { return vt == VT_EMPTY; }
    bool get_str(std::string &out) const;
    bool get_u64(uint64_t &out) const;
    bool get_bool() const { return vt == VT_BOOL && boolVal != 0; }
    bool get_time(int64_t &unix_seconds) const;
    void set_str(const std::string &s);
    void set_u32(uint32_t v);
    void set_u64(uint64_t v);
    void set_bool(bool v);
    void set_time(int64_t sec, long nsec);
};
std::string prop_display(PROPID id, const Prop &p);
std::string prop_name(PROPID id);

// Owning pointer to a COM interface.
template <class T> class Com {
    T *p_ = nullptr;

public:
    Com() = default;
    explicit Com(T *p) : p_(p) {}  // takes over a reference
    Com(const Com &o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com &operator=(Com o) noexcept { std::swap(p_, o.p_); return *this; }
    ~Com() { reset(); }
    void reset(T *n = nullptr) { if (p_) p_->Release(); p_ = n; }
    T **put() { reset(); return &p_; }
    void **pv() { reset(); return reinterpret_cast<void **>(&p_); }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T *detach() { T *p = p_; p_ = nullptr; return p; }
    template <class U> Com<U> query(const GUID &iid) const
    {
        Com<U> r;
        if (p_) p_->QueryInterface(iid, r.pv());
        return r;
    }
};

// IUnknown for our own objects: SZ_COM_BEGIN, one SZ_COM_IF per interface, SZ_COM_END(first interface).
#define SZ_COM_BEGIN                                                         \
    std::atomic<ULONG> refs_{1};                                             \
                                                                             \
public:                                                                      \
    ULONG AddRef() override { return ++refs_; }                              \
    ULONG Release() override                                                 \
    {                                                                        \
        const ULONG r = --refs_;                                             \
        if (!r) delete this;                                                 \
        return r;                                                            \
    }                                                                        \
    HRESULT QueryInterface(const GUID &iid, void **out) override             \
    {                                                                        \
        *out = nullptr;
#define SZ_COM_IF(I)                                                         \
        if (iid == IID_##I) {                                                \
            *out = static_cast<I *>(this);                                   \
            AddRef();                                                        \
            return S_OK;                                                     \
        }
#define SZ_COM_END(First)                                                    \
        if (iid == IID_IUnknown) {                                           \
            *out = static_cast<IUnknown *>(static_cast<First *>(this));      \
            AddRef();                                                        \
            return S_OK;                                                     \
        }                                                                    \
        return E_NOINTERFACE;                                                \
    }

HRESULT hr_from_errno(int e);

// ---- streams ----
class InFile final : public IInStream, public IStreamGetSize {
    int fd_;
    uint64_t pos_ = 0, size_ = 0;
    SZ_COM_BEGIN SZ_COM_IF(IInStream) SZ_COM_IF(ISequentialInStream) SZ_COM_IF(IStreamGetSize) SZ_COM_END(IInStream)
    explicit InFile(int fd, uint64_t size) : fd_(fd), size_(size) {}
    ~InFile();
    static InFile *open(const std::string &path, int &err);
    HRESULT Read(void *data, uint32_t size, uint32_t *processed) override;
    HRESULT Seek(int64_t offset, uint32_t origin, uint64_t *newpos) override;
    HRESULT GetSize(uint64_t *size) override { *size = size_; return S_OK; }
    uint64_t size() const { return size_; }
};

// A window [offset, offset + size) of another stream.
class SubStream final : public IInStream {
    Com<IInStream> base_;
    uint64_t off_, size_, pos_ = 0;
    SZ_COM_BEGIN SZ_COM_IF(IInStream) SZ_COM_IF(ISequentialInStream) SZ_COM_END(IInStream)
    SubStream(Com<IInStream> base, uint64_t off, uint64_t size) : base_(std::move(base)), off_(off), size_(size) {}
    HRESULT Read(void *data, uint32_t size, uint32_t *processed) override;
    HRESULT Seek(int64_t offset, uint32_t origin, uint64_t *newpos) override;
};

class OutFile final : public IOutStream {
    int fd_;
    SZ_COM_BEGIN SZ_COM_IF(IOutStream) SZ_COM_IF(ISequentialOutStream) SZ_COM_END(IOutStream)
    explicit OutFile(int fd) : fd_(fd) {}
    ~OutFile();
    HRESULT Write(const void *data, uint32_t size, uint32_t *processed) override;
    HRESULT Seek(int64_t offset, uint32_t origin, uint64_t *newpos) override;
    HRESULT SetSize(uint64_t size) override;
    int close_fd();  // closes now; returns 0 or errno
};

class MemOut final : public ISequentialOutStream {
    SZ_COM_BEGIN SZ_COM_IF(ISequentialOutStream) SZ_COM_END(ISequentialOutStream)
    std::string data;
    HRESULT Write(const void *d, uint32_t size, uint32_t *processed) override
    {
        data.append(static_cast<const char *>(d), size);
        if (processed) *processed = size;
        return S_OK;
    }
};

// ---- opened archives ----
struct Level {
    Com<IInArchive> arc;
    Com<IInStream> stream;
    int fmt = -1;
    std::string name;  // file name of this level (the archive, or the sub-archive item)
};

struct Opened {
    std::string path;
    std::vector<Level> levels;
    std::string password;
    bool password_given = false, password_asked = false;
    std::vector<std::string> volumes;  // extra volume files opened through the volume callback
    std::vector<std::string> errors, warnings;
    bool parser = false;  // Open Inside #: items are embedded archives
    struct Embedded {
        uint64_t offset = 0, size = 0;
        int fmt = -1;
    };
    std::vector<Embedded> embedded;

    IInArchive *arc() const { return levels.back().arc.get(); }
    int fmt() const { return levels.back().fmt; }
    std::string default_item_name() const;  // name for items without a path (a .gz stream)
};

// Opens `path`. type: "" detect, a format name, "*" try all formats, "#" find embedded archives.
// Returns false with `error` set (or `cancelled`).
bool open(const std::string &path, const std::string &type, Ui &ui, const std::string *password, Opened &out,
          std::string &error, bool &cancelled);
bool read_entry(IInArchive *arc, uint32_t index, const std::string &default_name, Entry &e);
std::string ask_password_for(Ui &ui, Opened &o, bool &cancelled);

// ---- extracting ----
struct ExtractParams {
    std::string out_dir;
    std::string strip_prefix;
    int path_mode = 0;   // 0 full, 1 none, 2 absolute
    int overwrite = 0;   // 0 ask, 1 overwrite, 2 skip, 3 rename new, 4 rename existing
    bool test = false;
};
void extract(Opened &o, const std::vector<uint32_t> &indices, const ExtractParams &p, Ui &ui, Result &r);

void add_error(Result &r, Ui &ui, const std::string &msg);
void add_warning(Result &r, Ui &ui, const std::string &msg);
void finish(Result &r);
bool make_dirs(const std::string &path, int &err);

}  // namespace szl::impl
