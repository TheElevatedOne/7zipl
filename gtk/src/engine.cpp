// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: glue between the GTK front-end and 7-Zip's UI/Common code. It plays the part that
// FileManager/ExtractCallback.cpp, GUI/UpdateGUI.cpp and Console/Main.cpp play in upstream 7-Zip.

#include "CPP/Common/Common.h"
#include "CPP/Common/MyWindows.h"

#include "C/7zVersion.h"
#include "C/CpuArch.h"

#include "CPP/Common/MyInitGuid.h"

#include "CPP/Common/IntToString.h"
#include "CPP/Common/MyException.h"
#include "CPP/Common/StringConvert.h"
#include "CPP/Common/Wildcard.h"

#include "CPP/Windows/ErrorMsg.h"
#include "CPP/Windows/FileDir.h"
#include "CPP/Windows/FileName.h"
#include "CPP/Windows/PropVariant.h"
#include "CPP/Windows/PropVariantConv.h"
#include "CPP/Windows/TimeUtils.h"

#include "CPP/7zip/Common/RegisterCodec.h"
#include "CPP/7zip/UI/Common/ArchiveCommandLine.h"
#include "CPP/7zip/UI/Common/Bench.h"
#include "CPP/7zip/UI/Common/ArchiveExtractCallback.h"
#include "CPP/7zip/UI/Common/ArchiveOpenCallback.h"
#include "CPP/7zip/UI/Common/Extract.h"
#include "CPP/7zip/UI/Common/HashCalc.h"
#include "CPP/7zip/UI/Common/LoadCodecs.h"
#include "CPP/7zip/UI/Common/OpenArchive.h"
#include "CPP/7zip/UI/Common/PropIDUtils.h"
#include "CPP/7zip/UI/Common/SetProperties.h"
#include "CPP/7zip/UI/Common/Update.h"

#include "engine.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

extern unsigned g_NumHashers;
extern const CHasherInfo *g_Hashers[];
void Set_ModuleDirPrefix_From_ProgArg0(const char *s);

using namespace NWindows;

namespace szl {

namespace {

CCodecs *g_codecs = nullptr;
CMyComPtr<IUnknown> g_codecs_ref;

std::string u2s(const UString &u)
{
    AString a;
    ConvertUnicodeToUTF8(u, a);
    return std::string(a.Ptr(), a.Len());
}
std::string u2s(const wchar_t *u) { return u ? u2s(UString(u)) : std::string(); }
UString s2u(const std::string &s)
{
    UString u;
    ConvertUTF8ToUnicode(AString(s.c_str()), u);
    return u;
}
std::string f2s(const FString &f) { return std::string(f.Ptr(), f.Len()); }  // FString is UTF-8 on Linux

std::string hresult_text(HRESULT r)
{
    if (r == E_OUTOFMEMORY) return "Can't allocate required memory";
    if (r == E_ABORT) return "Operation was cancelled";
    if (r == E_NOTIMPL) return "Not implemented";
    if (r == S_FALSE) return "Error";
    return u2s(NError::MyFormatMessage(r));
}

int64_t ft_to_unix(const FILETIME &ft) { return NTime::FileTime_To_UnixTime64(ft); }

const char *extract_error_text(Int32 opRes, Int32 encrypted)
{
    using namespace NArchive::NExtract::NOperationResult;
    switch (opRes) {
    case kUnsupportedMethod: return "Unsupported compression method";
    case kCRCError: return encrypted ? "CRC Failed in encrypted file. Wrong password?" : "CRC Failed";
    case kDataError: return encrypted ? "Data Error in encrypted file. Wrong password?" : "Data Error";
    case kUnavailable: return "Unavailable data";
    case kUnexpectedEnd: return "Unexpected end of data";
    case kDataAfterEnd: return "There are some data after the end of the payload data";
    case kIsNotArc: return "Is not archive";
    case kHeadersError: return "Headers Error";
    case kWrongPassword: return "Wrong password";
    default: return "Error";
    }
}

std::string open_error_text(const CArchiveLink &link, const std::string &name, HRESULT result)
{
    if (result == E_ABORT) return "Operation was cancelled";
    if (result == S_FALSE) {
        if (link.PasswordWasAsked) return "Cannot open encrypted archive '" + name + "'. Wrong password?";
        return "Cannot open the file '" + name + "' as archive";
    }
    return "Cannot open '" + name + "': " + hresult_text(result);
}

// Shared state of all callbacks of one operation.
struct Ctx {
    Ui &ui;
    Result &res;
    bool password_defined = false;
    UString password;
    bool password_asked = false;
    std::string archive;  // the archive being processed, for prompts and messages
    UString current;
    std::mutex mt;        // 7-Zip may call some callbacks from several threads

    Ctx(Ui &u, Result &r) : ui(u), res(r) {}

    HRESULT check() { return ui.cancelled() ? E_ABORT : S_OK; }
    void error(const std::string &s)
    {
        res.errors++;
        res.messages.push_back(s);
        ui.message(Ui::Level::Error, s);
    }
    void warning(const std::string &s)
    {
        res.warnings++;
        res.messages.push_back(s);
        ui.message(Ui::Level::Warning, s);
    }
    HRESULT get_password(BSTR *out)
    {
        *out = nullptr;
        RINOK(check())
        if (!password_defined) {
            std::string pw;
            password_asked = true;
            if (!ui.ask_password(archive, pw)) return E_ABORT;
            password = s2u(pw);
            password_defined = true;
        }
        return StringToBstr(password, out);
    }
};

// ---------- Opening archives (IOpenCallbackUI) ----------

class OpenCallback : public IOpenCallbackUI {
public:
    explicit OpenCallback(Ctx &c) : ctx(c) {}
    Z7_IFACE_IMP(IOpenCallbackUI)
    Ctx &ctx;
};

HRESULT OpenCallback::Open_CheckBreak() { return ctx.check(); }
HRESULT OpenCallback::Open_SetTotal(const UInt64 *, const UInt64 *bytes)
{
    if (bytes) ctx.ui.set_total(*bytes);
    return ctx.check();
}
HRESULT OpenCallback::Open_SetCompleted(const UInt64 *, const UInt64 *bytes)
{
    if (bytes) ctx.ui.set_completed(*bytes);
    return ctx.check();
}
HRESULT OpenCallback::Open_Finished() { return S_OK; }
HRESULT OpenCallback::Open_CryptoGetTextPassword(BSTR *password) { return ctx.get_password(password); }

// ---------- Extracting and testing ----------

class ExtractCallback Z7_final : public IFolderArchiveExtractCallback,
                                 public IExtractCallbackUI,
                                 public IFolderArchiveExtractCallback2,
                                 public ICryptoGetTextPassword,
                                 public IArchiveRequestMemoryUseCallback,
                                 public OpenCallback,
                                 public CMyUnknownImp {
    Z7_COM_QI_BEGIN2(IFolderArchiveExtractCallback)
    Z7_COM_QI_ENTRY(IFolderArchiveExtractCallback2)
    Z7_COM_QI_ENTRY(ICryptoGetTextPassword)
    Z7_COM_QI_ENTRY(IArchiveRequestMemoryUseCallback)
    Z7_COM_QI_END
    Z7_COM_ADDREF_RELEASE

    Z7_IFACE_COM7_IMP(IProgress)
    Z7_IFACE_COM7_IMP(IFolderArchiveExtractCallback)
    Z7_IFACE_IMP(IExtractCallbackUI)
    Z7_IFACE_COM7_IMP(IFolderArchiveExtractCallback2)
    Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
    Z7_IFACE_COM7_IMP(IArchiveRequestMemoryUseCallback)

public:
    explicit ExtractCallback(Ctx &c) : OpenCallback(c) {}
    bool test = false;
};

Z7_COM7F_IMF(ExtractCallback::SetTotal(UInt64 size))
{
    ctx.ui.set_total(size);
    return ctx.check();
}
Z7_COM7F_IMF(ExtractCallback::SetCompleted(const UInt64 *value))
{
    if (value) ctx.ui.set_completed(*value);
    return ctx.check();
}

Z7_COM7F_IMF(ExtractCallback::AskOverwrite(const wchar_t *existName, const FILETIME *existTime, const UInt64 *existSize,
                                           const wchar_t *newName, const FILETIME *newTime, const UInt64 *newSize,
                                           Int32 *answer))
{
    std::lock_guard<std::mutex> lock(ctx.mt);
    RINOK(ctx.check())
    const Ui::Overwrite a = ctx.ui.ask_overwrite(u2s(existName), existSize ? *existSize : 0,
                                                 existTime ? ft_to_unix(*existTime) : 0, u2s(newName),
                                                 newSize ? *newSize : 0, newTime ? ft_to_unix(*newTime) : 0);
    switch (a) {
    case Ui::Overwrite::Yes: *answer = NOverwriteAnswer::kYes; break;
    case Ui::Overwrite::YesAll: *answer = NOverwriteAnswer::kYesToAll; break;
    case Ui::Overwrite::No: *answer = NOverwriteAnswer::kNo; break;
    case Ui::Overwrite::NoAll: *answer = NOverwriteAnswer::kNoToAll; break;
    case Ui::Overwrite::Rename: *answer = NOverwriteAnswer::kAutoRename; break;
    case Ui::Overwrite::Cancel: *answer = NOverwriteAnswer::kCancel; return E_ABORT;
    }
    return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::PrepareOperation(const wchar_t *name, Int32 isFolder, Int32 askExtractMode,
                                               const UInt64 *))
{
    ctx.current = name;
    const char *what = "Extracting";
    switch (askExtractMode) {
    case NArchive::NExtract::NAskMode::kTest: what = "Testing"; break;
    case NArchive::NExtract::NAskMode::kSkip: what = "Skipping"; break;
    case NArchive::NExtract::NAskMode::kReadExternal: what = "Reading"; break;
    default: break;
    }
    (void)isFolder;
    ctx.ui.set_current(what, u2s(name));
    return ctx.check();
}

Z7_COM7F_IMF(ExtractCallback::MessageError(const wchar_t *message))
{
    std::lock_guard<std::mutex> lock(ctx.mt);
    ctx.error(u2s(message));
    return ctx.check();
}

Z7_COM7F_IMF(ExtractCallback::SetOperationResult(Int32 opRes, Int32 encrypted))
{
    if (opRes == NArchive::NExtract::NOperationResult::kOK) {
        ctx.ui.file_done();
    } else {
        std::lock_guard<std::mutex> lock(ctx.mt);
        std::string s = extract_error_text(opRes, encrypted);
        if (!ctx.current.IsEmpty()) s += " : " + u2s(ctx.current);
        ctx.error(s);
    }
    return ctx.check();
}

Z7_COM7F_IMF(ExtractCallback::ReportExtractResult(Int32 opRes, Int32 encrypted, const wchar_t *name))
{
    if (opRes != NArchive::NExtract::NOperationResult::kOK) {
        ctx.current = name;
        return SetOperationResult(opRes, encrypted);
    }
    return ctx.check();
}

Z7_COM7F_IMF(ExtractCallback::CryptoGetTextPassword(BSTR *password))
{
    std::lock_guard<std::mutex> lock(ctx.mt);
    return ctx.get_password(password);
}

std::atomic<uint64_t> g_memory_limit{0};

Z7_COM7F_IMF(ExtractCallback::RequestMemoryUse(UInt32 flags, UInt32, UInt32, const wchar_t *path, UInt64 requiredSize,
                                               UInt64 *allowedSize, UInt32 *answerFlags))
{
    // Like 7-Zip FM: allow up to the limit from Options > Settings, and ask above it.
    if ((flags & NRequestMemoryUseFlags::k_IsReport) == 0) {
        const uint64_t limit = g_memory_limit;
        if (limit && requiredSize > limit) {
            std::lock_guard<std::mutex> lock(ctx.mt);
            if (!ctx.ui.ask_memory(requiredSize, limit, path ? u2s(path) : ctx.archive)) {
                *answerFlags = NRequestMemoryAnswerFlags::k_SkipArc;
                ctx.error("The archive needs " + std::to_string(requiredSize >> 20) + " MB of memory to unpack; the limit is " +
                          std::to_string(limit >> 20) + " MB (Tools > Options > Settings)");
                return ctx.check();
            }
        }
        *allowedSize = requiredSize;
        *answerFlags = NRequestMemoryAnswerFlags::k_Allow;
        return ctx.check();
    }
    std::lock_guard<std::mutex> lock(ctx.mt);
    ctx.error("Not enough memory: the archive needs " + std::to_string(requiredSize >> 20) + " MB" +
              (path ? " : " + u2s(path) : std::string()));
    *answerFlags = NRequestMemoryAnswerFlags::k_SkipArc;
    return ctx.check();
}

HRESULT ExtractCallback::BeforeOpen(const wchar_t *name, bool)
{
    ctx.archive = u2s(name);
    ctx.ui.set_current("Opening", ctx.archive);
    return ctx.check();
}

HRESULT ExtractCallback::OpenResult(const CCodecs *, const CArchiveLink &arcLink, const wchar_t *name, HRESULT result)
{
    if (result != S_OK) {
        if (result == E_ABORT) return result;
        ctx.error(open_error_text(arcLink, u2s(name), result));
        return S_OK;
    }
    // Warnings about the archive itself (data after the end, unconfirmed start, ...).
    for (unsigned i = 0; i < arcLink.Arcs.Size(); i++) {
        const CArc &arc = arcLink.Arcs[i];
        const CArcErrorInfo &er = arc.ErrorInfo;
        if (er.ThereIsTail) ctx.warning("There are some data after the end of the payload data : " + u2s(arc.Path));
        if (!er.WarningMessage.IsEmpty()) ctx.warning(u2s(er.WarningMessage) + " : " + u2s(arc.Path));
        if (!er.ErrorMessage.IsEmpty()) ctx.error(u2s(er.ErrorMessage) + " : " + u2s(arc.Path));
    }
    return S_OK;
}

HRESULT ExtractCallback::ThereAreNoFiles() { return S_OK; }

HRESULT ExtractCallback::ExtractResult(HRESULT result)
{
    if (result == S_OK || result == E_ABORT) return result;
    ctx.error(hresult_text(result) + (ctx.archive.empty() ? "" : " : " + ctx.archive));
    return S_OK;
}

HRESULT ExtractCallback::SetPassword(const UString &password)
{
    ctx.password_defined = true;
    ctx.password = password;
    return S_OK;
}

// ---------- Adding, updating, deleting ----------

class UpdateCallback Z7_final : public IUpdateCallbackUI2 {
public:
    explicit UpdateCallback(Ctx &c) : ctx(c) {}
    Z7_IFACE_IMP(IUpdateCallbackUI)
    Z7_IFACE_IMP(IDirItemsCallback)
    Z7_IFACE_IMP(IUpdateCallbackUI2)
    Ctx &ctx;
    bool ask_password = false;  // -p without a value: ask for a new password
};

HRESULT UpdateCallback::ScanError(const FString &path, DWORD systemError)
{
    std::lock_guard<std::mutex> lock(ctx.mt);
    ctx.warning(f2s(path) + " : " + hresult_text(HRESULT_FROM_WIN32(systemError)));
    return ctx.check();
}
HRESULT UpdateCallback::ScanProgress(const CDirItemsStat &, const FString &path, bool)
{
    ctx.ui.set_current("Scanning", f2s(path));
    return ctx.check();
}
HRESULT UpdateCallback::OpenResult(const CCodecs *, const CArchiveLink &arcLink, const wchar_t *name, HRESULT result)
{
    if (result != S_OK && result != E_ABORT) ctx.error(open_error_text(arcLink, u2s(name), result));
    return S_OK;
}
HRESULT UpdateCallback::StartScanning()
{
    ctx.ui.set_current("Scanning", "");
    return ctx.check();
}
HRESULT UpdateCallback::FinishScanning(const CDirItemsStat &st)
{
    (void)st;  // scan errors were reported by ScanError
    return ctx.check();
}
HRESULT UpdateCallback::StartOpenArchive(const wchar_t *name)
{
    ctx.archive = u2s(name);
    ctx.ui.set_current("Opening", ctx.archive);
    return ctx.check();
}
HRESULT UpdateCallback::StartArchive(const wchar_t *name, bool updating)
{
    if (name) ctx.archive = u2s(name);
    ctx.ui.set_current(updating ? "Updating" : "Creating", ctx.archive);
    return ctx.check();
}
HRESULT UpdateCallback::FinishArchive(const CFinishArchiveStat &st)
{
    ctx.res.packed = st.OutArcFileSize;
    return S_OK;
}
HRESULT UpdateCallback::DeletingAfterArchiving(const FString &path, bool)
{
    ctx.ui.set_current("Deleting", f2s(path));
    return ctx.check();
}
HRESULT UpdateCallback::FinishDeletingAfterArchiving() { return S_OK; }
HRESULT UpdateCallback::MoveArc_Start(const wchar_t *, const wchar_t *dest, UInt64 size, Int32)
{
    ctx.ui.set_current("Moving", u2s(dest));
    ctx.ui.set_total(size);
    ctx.ui.set_completed(0);
    return ctx.check();
}
HRESULT UpdateCallback::MoveArc_Progress(UInt64 total, UInt64 current)
{
    ctx.ui.set_total(total);
    ctx.ui.set_completed(current);
    return ctx.check();
}
HRESULT UpdateCallback::MoveArc_Finish() { return ctx.check(); }

HRESULT UpdateCallback::WriteSfx(const wchar_t *name, UInt64)
{
    ctx.ui.set_current("Writing SFX module", u2s(name));
    return ctx.check();
}
HRESULT UpdateCallback::SetTotal(UInt64 size)
{
    ctx.ui.set_total(size);
    return ctx.check();
}
HRESULT UpdateCallback::SetCompleted(const UInt64 *value)
{
    if (value) ctx.ui.set_completed(*value);
    return ctx.check();
}
HRESULT UpdateCallback::SetRatioInfo(const UInt64 *inSize, const UInt64 *outSize)
{
    if (inSize && outSize) ctx.ui.set_ratio(*inSize, *outSize);
    return ctx.check();
}
HRESULT UpdateCallback::CheckBreak() { return ctx.check(); }
HRESULT UpdateCallback::SetNumItems(const CArcToDoStat &stat)
{
    ctx.ui.set_num_files(stat.Get_NumDataItems_Total());
    ctx.res.files = stat.NewData.NumFiles + stat.OldData.NumFiles;
    ctx.res.folders = stat.NewData.NumDirs + stat.OldData.NumDirs;
    ctx.res.size = stat.NewData.FilesSize + stat.OldData.FilesSize;
    return ctx.check();
}
HRESULT UpdateCallback::GetStream(const wchar_t *name, bool isDir, bool, UInt32 mode)
{
    const char *what = "Adding";
    if (mode == NUpdateNotifyOp::kUpdate) what = "Updating";
    else if (mode == NUpdateNotifyOp::kAnalyze) what = "Analyzing";
    else if (mode == NUpdateNotifyOp::kReplicate) what = "Replicating";
    else if (mode == NUpdateNotifyOp::kRepack) what = "Repacking";
    else if (mode == NUpdateNotifyOp::kSkip) what = "Skipping";
    else if (mode == NUpdateNotifyOp::kDelete) what = "Deleting";
    else if (mode == NUpdateNotifyOp::kHeader) what = "Writing headers";
    ctx.current = name ? name : L"";
    (void)isDir;
    ctx.ui.set_current(what, u2s(name));
    return ctx.check();
}
HRESULT UpdateCallback::OpenFileError(const FString &path, DWORD systemError)
{
    std::lock_guard<std::mutex> lock(ctx.mt);
    ctx.warning("Cannot open file " + f2s(path) + " : " + hresult_text(HRESULT_FROM_WIN32(systemError)));
    return ctx.check();
}
HRESULT UpdateCallback::ReadingFileError(const FString &path, DWORD systemError)
{
    std::lock_guard<std::mutex> lock(ctx.mt);
    ctx.error("Cannot read file " + f2s(path) + " : " + hresult_text(HRESULT_FROM_WIN32(systemError)));
    return ctx.check();
}
HRESULT UpdateCallback::SetOperationResult(Int32)
{
    ctx.ui.file_done();
    return ctx.check();
}
HRESULT UpdateCallback::ReportExtractResult(Int32 opRes, Int32 isEncrypted, const wchar_t *name)
{
    if (opRes != NArchive::NExtract::NOperationResult::kOK) {
        std::lock_guard<std::mutex> lock(ctx.mt);
        ctx.error(std::string(extract_error_text(opRes, isEncrypted)) + " : " + u2s(name));
    }
    return ctx.check();
}
HRESULT UpdateCallback::ReportUpdateOperation(UInt32 op, const wchar_t *name, bool isDir)
{
    return GetStream(name, isDir, false, op);
}
HRESULT UpdateCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *password)
{
    *password = nullptr;
    if (!ctx.password_defined && ask_password) {
        std::string pw;
        if (!ctx.ui.ask_password(std::string(), pw)) return E_ABORT;
        ctx.password = s2u(pw);
        ctx.password_defined = true;
    }
    *passwordIsDefined = ctx.password_defined ? 1 : 0;
    return StringToBstr(ctx.password, password);
}
HRESULT UpdateCallback::CryptoGetTextPassword(BSTR *password)
{
    // Asked when an encrypted archive is updated and its old items must be read.
    return ctx.get_password(password);
}
HRESULT UpdateCallback::ShowDeleteFile(const wchar_t *name, bool)
{
    ctx.ui.set_current("Deleting", u2s(name));
    return ctx.check();
}

// ---------- Hashing ----------

class HashCallback Z7_final : public IHashCallbackUI {
public:
    explicit HashCallback(Ctx &c) : ctx(c) {}
    Z7_IFACE_IMP(IDirItemsCallback)
    Z7_IFACE_IMP(IHashCallbackUI)
    Ctx &ctx;
    UString name;
};

HRESULT HashCallback::ScanError(const FString &path, DWORD systemError)
{
    ctx.warning(f2s(path) + " : " + hresult_text(HRESULT_FROM_WIN32(systemError)));
    return ctx.check();
}
HRESULT HashCallback::ScanProgress(const CDirItemsStat &, const FString &path, bool)
{
    ctx.ui.set_current("Scanning", f2s(path));
    return ctx.check();
}
HRESULT HashCallback::StartScanning() { return ctx.check(); }
HRESULT HashCallback::FinishScanning(const CDirItemsStat &) { return ctx.check(); }
HRESULT HashCallback::SetNumFiles(UInt64 n)
{
    ctx.ui.set_num_files(n);
    return ctx.check();
}
HRESULT HashCallback::SetTotal(UInt64 size)
{
    ctx.ui.set_total(size);
    return ctx.check();
}
HRESULT HashCallback::SetCompleted(const UInt64 *v)
{
    if (v) ctx.ui.set_completed(*v);
    return ctx.check();
}
HRESULT HashCallback::CheckBreak() { return ctx.check(); }
HRESULT HashCallback::BeforeFirstFile(const CHashBundle &hb)
{
    ctx.res.hash.methods.clear();
    FOR_VECTOR (i, hb.Hashers)
        ctx.res.hash.methods.emplace_back(hb.Hashers[i].Name.Ptr());
    return ctx.check();
}
HRESULT HashCallback::GetStream(const wchar_t *n, bool)
{
    name = n;
    ctx.ui.set_current("Calculating", u2s(n));
    return ctx.check();
}
HRESULT HashCallback::OpenFileError(const FString &path, DWORD systemError)
{
    ctx.error("Cannot open file " + f2s(path) + " : " + hresult_text(HRESULT_FROM_WIN32(systemError)));
    return ctx.check();
}
HRESULT HashCallback::SetOperationResult(UInt64 fileSize, const CHashBundle &hb, bool showHash)
{
    HashLine line;
    line.name = u2s(name);
    line.size = fileSize;
    line.dir = !showHash;
    if (showHash) {
        FOR_VECTOR (i, hb.Hashers) {
            char s[k_HashCalc_DigestSize_Max * 2 + 64];
            hb.Hashers[i].WriteToString(k_HashCalc_Index_Current, s);
            line.digests.emplace_back(s);
        }
    }
    ctx.res.hash.files.push_back(std::move(line));
    ctx.ui.file_done();
    return ctx.check();
}
HRESULT HashCallback::AfterLastFile(CHashBundle &hb)
{
    HashResult &h = ctx.res.hash;
    h.num_files = hb.NumFiles;
    h.num_dirs = hb.NumDirs;
    h.total_size = hb.FilesSize;
    h.errors = hb.NumErrors;
    h.totals.clear();
    FOR_VECTOR (i, hb.Hashers) {
        char s[k_HashCalc_DigestSize_Max * 2 + 64];
        hb.Hashers[i].WriteToString(k_HashCalc_Index_DataSum, s);
        h.totals.emplace_back(s);
    }
    return S_OK;
}

const char *const kPropNames[] = {
    "0", "1", "2", "Path", "Name", "Extension", "Folder", "Size", "Packed Size", "Attributes", "Created", "Accessed",
    "Modified", "Solid", "Commented", "Encrypted", "Split Before", "Split After", "Dictionary Size", "CRC", "Type",
    "Anti", "Method", "Host OS", "File System", "User", "Group", "Block", "Comment", "Position", "Path Prefix",
    "Folders", "Files", "Version", "Volume", "Multivolume", "Offset", "Links", "Blocks", "Volumes", "Time Type",
    "64-bit", "Big-endian", "CPU", "Physical Size", "Headers Size", "Checksum", "Characteristics", "Virtual Address",
    "ID", "Short Name", "Creator Application", "Sector Size", "Mode", "Symbolic Link", "Error", "Total Size",
    "Free Space", "Cluster Size", "Label", "Local Name", "Provider", "NT Security", "Alternate Stream", "Aux",
    "Deleted", "Tree", "SHA-1", "SHA-256", "Error Type", "Errors", "Errors", "Warnings", "Warning", "Streams",
    "Alternate Streams", "Alternate Streams Size", "Virtual Size", "Unpack Size", "Total Physical Size",
    "Volume Index", "SubType", "Short Comment", "Code Page", "Is not archive type", "Physical Size can't be detected",
    "Zeros Tail Is Allowed", "Tail Size", "Embedded Stub Size", "Link", "Hard Link", "iNode", "Stream ID",
    "Read-only", "Out Name", "Copy Link", "ArcFileName", "IsHash", "Metadata Changed", "User ID", "Group ID",
    "Device Major", "Device Minor", "Dev Major", "Dev Minor"};

std::string prop_name(PROPID id, const wchar_t *name)
{
    if (name && *name) return u2s(name);
    if (id < sizeof(kPropNames) / sizeof(kPropNames[0])) return kPropNames[id];
    return "Property " + std::to_string(id);
}

}  // namespace

// ---------- public API ----------

void set_memory_limit(uint64_t bytes) { g_memory_limit = bytes; }

void init(const char *argv0)
{
    if (g_codecs) return;
    if (argv0) Set_ModuleDirPrefix_From_ProgArg0(argv0);
    g_codecs = new CCodecs;
    g_codecs_ref = g_codecs;
    g_codecs->Load();
    Codecs_AddHashArcHandler(g_codecs);
}

std::string version() { return MY_VERSION_NUMBERS; }

std::vector<FormatInfo> formats()
{
    std::vector<FormatInfo> out;
    if (!g_codecs) return out;
    FOR_VECTOR (i, g_codecs->Formats) {
        const CArcInfoEx &f = g_codecs->Formats[i];
        FormatInfo fi;
        fi.name = u2s(f.Name);
        fi.can_update = f.UpdateEnabled;
        FOR_VECTOR (k, f.Exts)
            fi.extensions.push_back(u2s(f.Exts[k].Ext));
        out.push_back(std::move(fi));
    }
    return out;
}

std::vector<std::string> update_formats()
{
    // The Add to Archive dialog's list (7-Zip FM shows the formats that can create archives).
    static const char *const order[] = {"7z", "zip", "tar", "wim", "gzip", "bzip2", "xz", "zstd"};
    std::vector<std::string> out;
    const auto all = formats();
    for (const char *n : order)
        for (const auto &f : all)
            if (f.can_update && f.name == n) out.push_back(f.name);
    return out;
}

std::vector<std::string> hash_methods()
{
    std::vector<std::string> out;
    for (unsigned i = 0; i < g_NumHashers; i++) out.emplace_back(g_Hashers[i]->Name);
    return out;
}

bool looks_like_archive(const std::string &path)
{
    if (!g_codecs) return false;
    const auto slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const UString ext = s2u(name.substr(dot + 1));
    if (g_codecs->FindFormatForExtension(ext) >= 0) return true;
    // Multi-volume archives: name.7z.001
    if (ext.Len() == 3 && ext[0] >= L'0' && ext[0] <= L'9') return name.size() > 4;
    return false;
}

ArchiveListing open_archive(const std::string &path, Ui &ui, const std::string &password, const std::string &type)
{
    ArchiveListing out;
    out.path = path;
    Result dummy;
    Ctx ctx(ui, dummy);
    ctx.archive = path;
    if (!password.empty()) {
        ctx.password_defined = true;
        ctx.password = s2u(password);
    }
    try {
        CObjectVector<COpenType> types;
        if (!type.empty() && !ParseOpenTypes(*g_codecs, s2u(type), types)) types.Clear();
        CIntVector excluded;
        CArchiveLink link;
        COpenOptions op;
        op.codecs = g_codecs;
        op.types = &types;
        op.excludedFormats = &excluded;
        CObjectVector<CProperty> no_props;
        op.props = &no_props;  // COpenOptions leaves it uninitialised
        op.stdInMode = false;
        op.filePath = s2u(path);
        OpenCallback cb(ctx);
        ui.set_current("Opening", path);
        HRESULT res = link.Open3(op, &cb);
        if (res != S_OK) {
            out.cancelled = res == E_ABORT;
            out.error = open_error_text(link, path, res);
            return out;
        }
        const CArc &arc = link.Arcs.Back();
        IInArchive *archive = arc.Archive;
        if (arc.FormatIndex >= 0) {
            const CArcInfoEx &f = g_codecs->Formats[(unsigned)arc.FormatIndex];
            out.format = u2s(f.Name);
            out.can_update = f.UpdateEnabled && link.Arcs.Size() == 1 && link.VolumePaths.Size() <= 1;
        }
        if (ctx.password_defined) out.password = u2s(ctx.password);

        // Archive properties (Info dialog), for every level of nested archives.
        for (unsigned level = 0; level < link.Arcs.Size(); level++) {
            const CArc &a = link.Arcs[level];
            if (link.Arcs.Size() > 1) out.props.emplace_back("--", u2s(a.Path));
            if (a.FormatIndex >= 0) out.props.emplace_back("Type", u2s(g_codecs->Formats[(unsigned)a.FormatIndex].Name));
            UInt32 n = 0;
            a.Archive->GetNumberOfArchiveProperties(&n);
            for (UInt32 i = 0; i < n; i++) {
                CMyComBSTR name;
                PROPID id;
                VARTYPE vt;
                if (a.Archive->GetArchivePropertyInfo(i, &name, &id, &vt) != S_OK) continue;
                NCOM::CPropVariant prop;
                if (a.Archive->GetArchiveProperty(id, &prop) != S_OK || prop.vt == VT_EMPTY) continue;
                UString s;
                ConvertPropertyToString2(s, prop, id);
                if (!s.IsEmpty()) out.props.emplace_back(prop_name(id, name), u2s(s));
            }
            const CArcErrorInfo &er = a.ErrorInfo;
            if (!er.WarningMessage.IsEmpty()) out.props.emplace_back("Warning", u2s(er.WarningMessage));
            if (!er.ErrorMessage.IsEmpty()) out.props.emplace_back("Error", u2s(er.ErrorMessage));
            if (er.ThereIsTail) out.props.emplace_back("Warning", "There are some data after the end of the payload data");
        }

        UInt32 num = 0;
        if (archive->GetNumberOfItems(&num) != S_OK) throw CSystemException(E_FAIL);
        out.entries.reserve(num);
        CReadArcItem item;
        for (UInt32 i = 0; i < num; i++) {
            if ((i & 0xFFF) == 0 && ui.cancelled()) {
                out.cancelled = true;
                out.error = "Operation was cancelled";
                return out;
            }
            if (arc.GetItem(i, item) != S_OK) continue;
            Entry e;
            e.index = i;
            UString p = item.Path;
            p.Replace(WCHAR_PATH_SEPARATOR, L'/');
            e.path = u2s(p);
            e.dir = item.IsDir;
            {
                NCOM::CPropVariant prop;
                if (archive->GetProperty(i, kpidSize, &prop) == S_OK && prop.vt != VT_EMPTY)
                    e.has_size = ConvertPropVariantToUInt64(prop, e.size);
            }
            {
                NCOM::CPropVariant prop;
                if (archive->GetProperty(i, kpidPackSize, &prop) == S_OK && prop.vt != VT_EMPTY)
                    e.has_packed = ConvertPropVariantToUInt64(prop, e.packed);
            }
            auto get_time = [&](PROPID id, int64_t &t, bool &has) {
                NCOM::CPropVariant prop;
                if (archive->GetProperty(i, id, &prop) == S_OK && prop.vt == VT_FILETIME) {
                    t = ft_to_unix(prop.filetime);
                    has = true;
                }
            };
            get_time(kpidMTime, e.mtime, e.has_mtime);
            get_time(kpidCTime, e.ctime, e.has_ctime);
            get_time(kpidATime, e.atime, e.has_atime);
            {
                NCOM::CPropVariant prop;
                if (archive->GetProperty(i, kpidCRC, &prop) == S_OK && prop.vt == VT_UI4) {
                    e.crc = prop.ulVal;
                    e.has_crc = true;
                }
            }
            {
                NCOM::CPropVariant prop;
                if (archive->GetProperty(i, kpidEncrypted, &prop) == S_OK && prop.vt == VT_BOOL) e.encrypted = VARIANT_BOOLToBool(prop.boolVal);
            }
            auto get_str = [&](PROPID id, std::string &s) {
                NCOM::CPropVariant prop;
                if (archive->GetProperty(i, id, &prop) == S_OK && prop.vt != VT_EMPTY) {
                    UString u;
                    ConvertPropertyToString2(u, prop, id);
                    s = u2s(u);
                }
            };
            get_str(kpidAttrib, e.attrib);
            get_str(kpidMethod, e.method);
            get_str(kpidComment, e.comment);
            get_str(kpidUser, e.user);
            get_str(kpidGroup, e.group);
            get_str(kpidSymLink, e.link);
            out.entries.push_back(std::move(e));
        }
        out.ok = true;
    } catch (const CSystemException &e) {
        out.error = hresult_text(e.ErrorCode);
    } catch (const UString &s) {
        out.error = u2s(s);
    } catch (const char *s) {
        out.error = s;
    } catch (const CNewException &) {
        out.error = "Can't allocate required memory";
    } catch (...) {
        out.error = "Unknown error";
    }
    return out;
}

static void finish(Result &r, HRESULT hr)
{
    if (hr == E_ABORT) {
        r.cancelled = true;
        r.ok = false;
        r.summary = "Operation was cancelled";
        return;
    }
    if (hr != S_OK && hr != S_FALSE) {
        r.errors++;
        r.messages.push_back(hresult_text(hr));
    }
    r.ok = r.errors == 0;
    if (r.ok)
        r.summary = r.warnings ? "Finished with warnings" : "Everything is Ok";
    else
        r.summary = "There are some errors";
}

Result run(const std::vector<std::string> &args, Ui &ui)
{
    Result r;
    Ctx ctx(ui, r);
    try {
        UStringVector cmd;
        for (const auto &a : args) cmd.Add(s2u(a));
        CArcCmdLineOptions options;
        CArcCmdLineParser parser;
        parser.Parse1(cmd, options);
        parser.Parse2(options);

        CObjectVector<COpenType> types;
        if (!ParseOpenTypes(*g_codecs, options.ArcType, types)) throw UString(L"Unsupported archive type");
        CIntVector excluded;
        FOR_VECTOR (k, options.ExcludedArcTypes) {
            CIntVector idx;
            if (!g_codecs->FindFormatForArchiveType(options.ExcludedArcTypes[k], idx) || idx.Size() != 1)
                throw UString(L"Unsupported archive type");
            excluded.AddToUniqueSorted(idx[0]);
        }

        ctx.password_defined = options.PasswordEnabled && !options.Password.IsEmpty();
        ctx.password = options.Password;

        HRESULT hr = S_OK;
        const auto type = options.Command.CommandType;
        if (options.Command.IsFromExtractGroup()) {
            UStringVector paths, pathsFull;
            CDirItemsStat st;
            hr = EnumerateDirItemsAndSort(options.arcCensor, NWildcard::k_RelatPath, UString(), paths, pathsFull, st, nullptr);
            if (hr == S_OK) {
                ExtractCallback *ecs = new ExtractCallback(ctx);
                CMyComPtr<IFolderArchiveExtractCallback> ref(ecs);
                ecs->test = options.Command.IsTestCommand();
                CExtractOptions eo;
                (CExtractOptionsBase &)eo = options.ExtractOptions;
                eo.YesToAll = options.YesToAll;
                eo.TestMode = options.Command.IsTestCommand();
                eo.Properties = options.Properties;
                UString err;
                CDecompressStat stat;
                stat.Clear();
                hr = Extract(g_codecs, types, excluded, paths, pathsFull, options.Censor.Pairs.Front().Head, eo, ecs, ecs,
                             ecs, nullptr, err, stat);
                if (!err.IsEmpty()) ctx.error(u2s(err));
                r.files = stat.NumFiles;
                r.folders = stat.NumFolders;
                r.size = stat.UnpackSize;
                r.packed = stat.PackSize;
                if (paths.IsEmpty()) ctx.error("Cannot find archive");
            }
        } else if (options.Command.IsFromUpdateGroup()) {
            CUpdateOptions &uo = options.UpdateOptions;
            OpenCallback ocb(ctx);
            UpdateCallback ucb(ctx);
            ucb.ask_password = options.PasswordEnabled && options.Password.IsEmpty();
            CUpdateErrorInfo ei;
            ctx.archive = u2s(options.ArchiveName);
            hr = UpdateArchive(g_codecs, types, options.ArchiveName, options.Censor, uo, ei, &ocb, &ucb, true);
            if (ei.ThereIsError()) {
                std::string s(ei.Message.Ptr(), ei.Message.Len());
                FOR_VECTOR (i, ei.FileNames) s += "\n" + f2s(ei.FileNames[i]);
                if (ei.SystemError) s += "\n" + hresult_text(HRESULT_FROM_WIN32(ei.SystemError));
                ctx.error(s);
                if (hr == S_OK) hr = E_FAIL;
                else if (hr != E_ABORT) hr = S_OK;  // already reported
            }
        } else if (type == NCommandType::kHash) {
            HashCallback hcb(ctx);
            AString err;
            hr = HashCalc(EXTERNAL_CODECS_LOC_VARS options.Censor, options.HashOptions, err, &hcb);
            if (!err.IsEmpty()) ctx.error(std::string(err.Ptr(), err.Len()));
            r.files = r.hash.num_files;
            r.folders = r.hash.num_dirs;
            r.size = r.hash.total_size;
        } else {
            throw UString(L"This command is not supported by the GUI");
        }
        finish(r, hr);
    } catch (const CArcCmdLineException &e) {
        ctx.error("Command Line Error: " + u2s(e));
        finish(r, S_OK);
    } catch (const CSystemException &e) {
        finish(r, e.ErrorCode);
    } catch (const UString &s) {
        ctx.error(u2s(s));
        finish(r, S_OK);
    } catch (const AString &s) {
        ctx.error(std::string(s.Ptr(), s.Len()));
        finish(r, S_OK);
    } catch (const char *s) {
        ctx.error(s);
        finish(r, S_OK);
    } catch (const CNewException &) {
        finish(r, E_OUTOFMEMORY);
    } catch (...) {
        ctx.error("Unknown error");
        finish(r, S_OK);
    }
    return r;
}

Result extract_items(const ExtractRequest &req, Ui &ui)
{
    Result r;
    Ctx ctx(ui, r);
    ctx.archive = req.archive;
    if (!req.password.empty()) {
        ctx.password_defined = true;
        ctx.password = s2u(req.password);
    }
    HRESULT hr = S_OK;
    try {
        ExtractCallback *ecs = new ExtractCallback(ctx);
        CMyComPtr<IFolderArchiveExtractCallback> ref(ecs);
        ecs->test = req.test;

        CObjectVector<COpenType> types;
        CIntVector excluded;
        CArchiveLink link;
        COpenOptions op;
        op.codecs = g_codecs;
        op.types = &types;
        op.excludedFormats = &excluded;
        CObjectVector<CProperty> no_props;
        op.props = &no_props;  // COpenOptions leaves it uninitialised
        op.filePath = s2u(req.archive);
        ui.set_current("Opening", req.archive);
        hr = link.Open3(op, ecs);
        if (hr != S_OK) {
            if (hr != E_ABORT) ctx.error(open_error_text(link, req.archive, hr));
            finish(r, hr == E_ABORT ? hr : S_OK);
            return r;
        }
        const CArc &arc = link.Arcs.Back();
        IInArchive *archive = arc.Archive;

        // Indices of the selected items and everything below them.
        CRecordVector<UInt32> indices;
        UInt32 num = 0;
        archive->GetNumberOfItems(&num);
        std::vector<UString> want;
        for (const auto &s : req.items) want.push_back(s2u(s));
        CReadArcItem item;
        for (UInt32 i = 0; i < num; i++) {
            if (want.empty()) {
                indices.Add(i);
                continue;
            }
            if (arc.GetItem(i, item) != S_OK) continue;
            UString p = item.Path;
            p.Replace(WCHAR_PATH_SEPARATOR, L'/');
            for (const UString &w : want) {
                if (CompareFileNames(p, w) == 0 ||
                    (p.Len() > w.Len() && p[w.Len()] == L'/' && CompareFileNames(p.Left(w.Len()), w) == 0)) {
                    indices.Add(i);
                    break;
                }
            }
        }
        if (indices.IsEmpty()) {
            finish(r, S_OK);
            return r;
        }

        UStringVector remove;
        if (!req.strip_prefix.empty()) SplitPathToParts(s2u(req.strip_prefix), remove);
        while (!remove.IsEmpty() && remove.Back().IsEmpty()) remove.DeleteBack();

        CArchiveExtractCallback *aec = new CArchiveExtractCallback;
        CMyComPtr<IArchiveExtractCallback> aecRef(aec);
        NExtract::NPathMode::EEnum pm = NExtract::NPathMode::kFullPaths;
        if (req.path_mode == 1) pm = NExtract::NPathMode::kNoPaths;
        else if (req.path_mode == 2) pm = NExtract::NPathMode::kAbsPaths;
        NExtract::NOverwriteMode::EEnum om = NExtract::NOverwriteMode::kAsk;
        switch (req.overwrite) {
        case 1: om = NExtract::NOverwriteMode::kOverwrite; break;
        case 2: om = NExtract::NOverwriteMode::kSkip; break;
        case 3: om = NExtract::NOverwriteMode::kRename; break;
        case 4: om = NExtract::NOverwriteMode::kRenameExisting; break;
        default: break;
        }
        aec->InitForMulti(false, pm, om, NExtract::NZoneIdMode::kNone, false);

        FString outDir;
        if (!req.test) {
            outDir = us2fs(s2u(req.out_dir));
            NFile::NName::NormalizeDirPathPrefix(outDir);
            if (!NFile::NDir::CreateComplexDir(outDir)) {
                ctx.error("Cannot create output directory " + req.out_dir);
                finish(r, S_OK);
                return r;
            }
        }
        CExtractNtOptions nt;
        nt.HardLinks.Val = true;
        nt.SymLinks.Val = true;
        UInt64 packSize = 0;
        {
            NCOM::CPropVariant prop;
            if (archive->GetArchiveProperty(kpidPhySize, &prop) == S_OK) ConvertPropVariantToUInt64(prop, packSize);
        }
        aec->Init(nt, nullptr, &arc, ecs, false, req.test, outDir, remove, false, packSize);
        if (!req.test) aec->PrepareHardLinks(&indices);

        {
            CArchiveExtractCallback_Closer closer(aec);
            const UInt64 zero = 0;
            IArchiveExtractCallback *iaec = aec;
            iaec->SetCompleted(&zero);
            hr = archive->Extract(indices.ConstData(), indices.Size(), req.test ? 1 : 0, iaec);
            const HRESULT hr2 = closer.Close();
            if (hr == S_OK) hr = hr2;
        }
        if (hr != S_OK && hr != E_ABORT) {
            ctx.error(hresult_text(hr));
            hr = S_OK;
        }
    } catch (const CSystemException &e) {
        hr = e.ErrorCode;
    } catch (const UString &s) {
        ctx.error(u2s(s));
    } catch (const char *s) {
        ctx.error(s);
    } catch (const CNewException &) {
        hr = E_OUTOFMEMORY;
    } catch (...) {
        ctx.error("Unknown error");
    }
    finish(r, hr);
    return r;
}

namespace {
class BenchPrint final : public IBenchPrintCallback {
public:
    BenchPrint(Ui &u, const std::function<void(const std::string &)> &p) : ui(u), print(p) {}
    void Print(const char *s) override { print(s); }
    void NewLine() override { print("\n"); }
    HRESULT CheckBreak() override { return ui.cancelled() ? E_ABORT : S_OK; }
    Ui &ui;
    const std::function<void(const std::string &)> &print;
};
}  // namespace

Result benchmark(unsigned threads, unsigned dict_log, unsigned passes, Ui &ui,
                 const std::function<void(const std::string &)> &print)
{
    Result r;
    try {
        CObjectVector<CProperty> props;
        if (threads) {
            CProperty p;
            p.Name = L"mt";
            p.Value.Add_UInt32(threads);
            props.Add(p);
        }
        if (dict_log) {
            CProperty p;
            p.Name = L"d";
            p.Value.Add_UInt32(dict_log);
            props.Add(p);
        }
        BenchPrint cb(ui, print);
        const HRESULT hr = Bench(EXTERNAL_CODECS_LOC_VARS & cb, nullptr, props, passes ? passes : 1, true);
        finish(r, hr);
    } catch (...) {
        r.errors++;
        r.messages.push_back("Benchmark error");
        finish(r, S_OK);
    }
    return r;
}

}  // namespace szl
