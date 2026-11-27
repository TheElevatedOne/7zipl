// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the header for 7-Zip's library 7z.so (7-Zip 23 and newer on Linux), which the distributions ship
// without one. Written from 7-Zip's interface headers (IArchive.h, IStream.h, IPassword.h, IProgress.h,
// ICoder.h, PropID.h, MyWindows.h) with the same layout, so it matches the library's ABI. Only what 7zipl
// uses is declared; the library itself comes from the distribution's 7zip package.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sz {

using HRESULT = int32_t;
using ULONG = uint32_t;
using PROPID = uint32_t;
using VARTYPE = uint16_t;
using BSTR = wchar_t *;
using Byte = unsigned char;

constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT E_NOTIMPL = (HRESULT)0x80004001u;
constexpr HRESULT E_NOINTERFACE = (HRESULT)0x80004002u;
constexpr HRESULT E_ABORT = (HRESULT)0x80004004u;
constexpr HRESULT E_FAIL = (HRESULT)0x80004005u;
constexpr HRESULT E_OUTOFMEMORY = (HRESULT)0x8007000Eu;
constexpr HRESULT E_INVALIDARG = (HRESULT)0x80070057u;

struct GUID {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
};
inline bool operator==(const GUID &a, const GUID &b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

// {23170F69-40C1-278A-0000-00gg00ii0000}
constexpr GUID iid7(uint8_t group, uint8_t sub) { return {0x23170F69, 0x40C1, 0x278A, {0, 0, 0, group, 0, sub, 0, 0}}; }
constexpr GUID IID_IUnknown = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

struct FILETIME {
    uint32_t lo, hi;
};

enum : VARTYPE {
    VT_EMPTY = 0, VT_I2 = 2, VT_I4 = 3, VT_BSTR = 8, VT_BOOL = 11, VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18,
    VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21, VT_INT = 22, VT_UINT = 23, VT_FILETIME = 64
};

struct PROPVARIANT {
    VARTYPE vt;
    uint16_t r1, r2, r3;  // wReserved1 carries the FILETIME precision
    union {
        int8_t cVal;
        uint8_t bVal;
        int16_t iVal;
        uint16_t uiVal;
        int32_t lVal;
        uint32_t ulVal;
        int32_t intVal;
        uint32_t uintVal;
        int64_t hVal;
        uint64_t uhVal;
        int16_t boolVal;  // VARIANT_TRUE = -1
        int32_t scode;
        FILETIME filetime;
        BSTR bstrVal;
    };
};
static_assert(sizeof(PROPVARIANT) == 16, "PROPVARIANT layout");

// 7-Zip 23+ on Linux: IUnknown has no virtual destructor (GetModuleProp kInterfaceType == 0).
struct IUnknown {
    virtual HRESULT QueryInterface(const GUID &iid, void **out) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
};

#define SZ_IFACE(name, base, group, sub)                       \
    constexpr GUID IID_##name = iid7(group, sub);              \
    struct name : public base

SZ_IFACE(ISequentialInStream, IUnknown, 3, 0x01) { virtual HRESULT Read(void *data, uint32_t size, uint32_t *processed) = 0; };
SZ_IFACE(ISequentialOutStream, IUnknown, 3, 0x02) { virtual HRESULT Write(const void *data, uint32_t size, uint32_t *processed) = 0; };
SZ_IFACE(IInStream, ISequentialInStream, 3, 0x03) { virtual HRESULT Seek(int64_t offset, uint32_t origin, uint64_t *newpos) = 0; };
SZ_IFACE(IOutStream, ISequentialOutStream, 3, 0x04)
{
    virtual HRESULT Seek(int64_t offset, uint32_t origin, uint64_t *newpos) = 0;
    virtual HRESULT SetSize(uint64_t size) = 0;
};
SZ_IFACE(IStreamGetSize, IUnknown, 3, 0x06) { virtual HRESULT GetSize(uint64_t *size) = 0; };

SZ_IFACE(IProgress, IUnknown, 0, 0x05)
{
    virtual HRESULT SetTotal(uint64_t total) = 0;
    virtual HRESULT SetCompleted(const uint64_t *value) = 0;
};

SZ_IFACE(ICryptoGetTextPassword, IUnknown, 5, 0x10) { virtual HRESULT CryptoGetTextPassword(BSTR *password) = 0; };
SZ_IFACE(ICryptoGetTextPassword2, IUnknown, 5, 0x11) { virtual HRESULT CryptoGetTextPassword2(int32_t *defined, BSTR *password) = 0; };

SZ_IFACE(ISetProperties, IUnknown, 6, 0x03)
{
    virtual HRESULT SetProperties(const wchar_t *const *names, const PROPVARIANT *values, uint32_t num) = 0;
};
SZ_IFACE(IArchiveRequestMemoryUseCallback, IUnknown, 6, 0x09)
{
    virtual HRESULT RequestMemoryUse(uint32_t flags, uint32_t index_type, uint32_t index, const wchar_t *path,
                                     uint64_t required, uint64_t *allowed, uint32_t *answer) = 0;
};
SZ_IFACE(IArchiveOpenCallback, IUnknown, 6, 0x10)
{
    virtual HRESULT SetTotal(const uint64_t *files, const uint64_t *bytes) = 0;
    virtual HRESULT SetCompleted(const uint64_t *files, const uint64_t *bytes) = 0;
};
SZ_IFACE(IArchiveExtractCallback, IProgress, 6, 0x20)
{
    virtual HRESULT GetStream(uint32_t index, ISequentialOutStream **out, int32_t mode) = 0;
    virtual HRESULT PrepareOperation(int32_t mode) = 0;
    virtual HRESULT SetOperationResult(int32_t result) = 0;
};
SZ_IFACE(IArchiveExtractCallbackMessage2, IUnknown, 6, 0x22)
{
    virtual HRESULT ReportExtractResult(uint32_t index_type, uint32_t index, int32_t result) = 0;
};
SZ_IFACE(IArchiveOpenVolumeCallback, IUnknown, 6, 0x30)
{
    virtual HRESULT GetProperty(PROPID id, PROPVARIANT *value) = 0;
    virtual HRESULT GetStream(const wchar_t *name, IInStream **stream) = 0;
};
SZ_IFACE(IInArchiveGetStream, IUnknown, 6, 0x40) { virtual HRESULT GetStream(uint32_t index, ISequentialInStream **stream) = 0; };
SZ_IFACE(IArchiveOpenSetSubArchiveName, IUnknown, 6, 0x50) { virtual HRESULT SetSubArchiveName(const wchar_t *name) = 0; };
SZ_IFACE(IInArchive, IUnknown, 6, 0x60)
{
    virtual HRESULT Open(IInStream *stream, const uint64_t *max_check_start, IArchiveOpenCallback *cb) = 0;
    virtual HRESULT Close() = 0;
    virtual HRESULT GetNumberOfItems(uint32_t *num) = 0;
    virtual HRESULT GetProperty(uint32_t index, PROPID id, PROPVARIANT *value) = 0;
    virtual HRESULT Extract(const uint32_t *indices, uint32_t num, int32_t test, IArchiveExtractCallback *cb) = 0;
    virtual HRESULT GetArchiveProperty(PROPID id, PROPVARIANT *value) = 0;
    virtual HRESULT GetNumberOfProperties(uint32_t *num) = 0;
    virtual HRESULT GetPropertyInfo(uint32_t index, BSTR *name, PROPID *id, VARTYPE *type) = 0;
    virtual HRESULT GetNumberOfArchiveProperties(uint32_t *num) = 0;
    virtual HRESULT GetArchivePropertyInfo(uint32_t index, BSTR *name, PROPID *id, VARTYPE *type) = 0;
};
SZ_IFACE(IArchiveUpdateCallback, IProgress, 6, 0x80)
{
    virtual HRESULT GetUpdateItemInfo(uint32_t index, int32_t *new_data, int32_t *new_props, uint32_t *index_in_archive) = 0;
    virtual HRESULT GetProperty(uint32_t index, PROPID id, PROPVARIANT *value) = 0;
    virtual HRESULT GetStream(uint32_t index, ISequentialInStream **stream) = 0;
    virtual HRESULT SetOperationResult(int32_t result) = 0;
};
SZ_IFACE(IOutArchive, IUnknown, 6, 0xA0)
{
    virtual HRESULT UpdateItems(ISequentialOutStream *out, uint32_t num, IArchiveUpdateCallback *cb) = 0;
    virtual HRESULT GetFileTimeType(uint32_t *type) = 0;
};

SZ_IFACE(ICompressProgressInfo, IUnknown, 4, 0x04) { virtual HRESULT SetRatioInfo(const uint64_t *in, const uint64_t *out) = 0; };
SZ_IFACE(ICompressCoder, IUnknown, 4, 0x05)
{
    virtual HRESULT Code(ISequentialInStream *in, ISequentialOutStream *out, const uint64_t *in_size,
                         const uint64_t *out_size, ICompressProgressInfo *progress) = 0;
};
SZ_IFACE(ICompressSetCoderProperties, IUnknown, 4, 0x20)
{
    virtual HRESULT SetCoderProperties(const PROPID *ids, const PROPVARIANT *props, uint32_t num) = 0;
};
SZ_IFACE(ICompressSetDecoderProperties2, IUnknown, 4, 0x22) { virtual HRESULT SetDecoderProperties2(const Byte *data, uint32_t size) = 0; };
SZ_IFACE(ICompressWriteCoderProperties, IUnknown, 4, 0x23) { virtual HRESULT WriteCoderProperties(ISequentialOutStream *out) = 0; };
SZ_IFACE(IHasher, IUnknown, 4, 0xC0)
{
    virtual void Init() = 0;
    virtual void Update(const void *data, uint32_t size) = 0;
    virtual void Final(Byte *digest) = 0;
    virtual uint32_t GetDigestSize() = 0;
};
SZ_IFACE(IHashers, IUnknown, 4, 0xC1)
{
    virtual uint32_t GetNumHashers() = 0;
    virtual HRESULT GetHasherProp(uint32_t index, PROPID id, PROPVARIANT *value) = 0;
    virtual HRESULT CreateHasher(uint32_t index, IHasher **hasher) = 0;
};

#undef SZ_IFACE

using Func_IsArc = uint32_t (*)(const Byte *p, size_t size);

}  // namespace sz

// Functions exported by 7z.so (C linkage, in 7-Zip's DllExports2.cpp, ArchiveExports.cpp, CodecExports.cpp
// and MyWindows.cpp). 7zipl links against the library, so these are called directly.
extern "C" {
sz::HRESULT CreateObject(const sz::GUID *clsid, const sz::GUID *iid, void **out);
sz::HRESULT GetNumberOfFormats(uint32_t *num);
sz::HRESULT GetHandlerProperty2(uint32_t index, sz::PROPID id, sz::PROPVARIANT *value);
sz::HRESULT GetIsArc(uint32_t index, sz::Func_IsArc *is_arc);
sz::HRESULT GetNumberOfMethods(uint32_t *num);
sz::HRESULT GetMethodProperty(uint32_t index, sz::PROPID id, sz::PROPVARIANT *value);
sz::HRESULT CreateEncoder(uint32_t index, const sz::GUID *iid, void **out);
sz::HRESULT CreateDecoder(uint32_t index, const sz::GUID *iid, void **out);
sz::HRESULT GetHashers(sz::IHashers **hashers);
sz::HRESULT GetModuleProp(sz::PROPID id, sz::PROPVARIANT *value);
sz::BSTR SysAllocStringLen(const wchar_t *s, uint32_t len);
void SysFreeString(sz::BSTR s);
uint32_t SysStringByteLen(sz::BSTR s);
}

namespace sz {

namespace handler_prop { enum { kName = 0, kClassID, kExtension, kAddExtension, kUpdate, kKeepName, kSignature,
                                kMultiSignature, kSignatureOffset, kAltStreams, kNtSecure, kFlags, kTimeFlags }; }
namespace method_prop { enum { kID = 0, kName, kDecoder, kEncoder }; }
namespace module_prop { enum { kInterfaceType = 0, kVersion }; }
namespace coder_prop { enum { kDictionarySize = 1, kNumThreads = 13, kLevel = 15 }; }

namespace arc_flags {
constexpr uint32_t kFindSignature = 1 << 3, kMultiSignature = 1 << 4, kPreArc = 1 << 9, kSymLinks = 1 << 10,
                   kHardLinks = 1 << 11, kByExtOnlyOpen = 1 << 12;
}

namespace ask_mode { enum { kExtract = 0, kTest, kSkip, kReadExternal }; }
namespace op_result {
enum { kOK = 0, kUnsupportedMethod, kDataError, kCRCError, kUnavailable, kUnexpectedEnd, kDataAfterEnd, kIsNotArc,
       kHeadersError, kWrongPassword };
}
namespace mem_answer { constexpr uint32_t k_Allow = 1 << 0, k_Stop = 1 << 1, k_SkipArc = 1 << 2, k_Limit_Exceeded = 1 << 10; }
namespace mem_flags { constexpr uint32_t k_SkipArc_IsExpected = 1 << 12; }

namespace error_flags {
constexpr uint32_t IsNotArc = 1 << 0, HeadersError = 1 << 1, EncryptedHeadersError = 1 << 2,
                   UnavailableStart = 1 << 3, UnconfirmedStart = 1 << 4, UnexpectedEnd = 1 << 5,
                   DataAfterEnd = 1 << 6, UnsupportedMethod = 1 << 7, UnsupportedFeature = 1 << 8,
                   DataError = 1 << 9, CrcError = 1 << 10;
}

enum : PROPID {
    kpidNoProperty = 0, kpidMainSubfile, kpidHandlerItemIndex, kpidPath, kpidName, kpidExtension, kpidIsDir, kpidSize,
    kpidPackSize, kpidAttrib, kpidCTime, kpidATime, kpidMTime, kpidSolid, kpidCommented, kpidEncrypted,
    kpidSplitBefore, kpidSplitAfter, kpidDictionarySize, kpidCRC, kpidType, kpidIsAnti, kpidMethod, kpidHostOS,
    kpidFileSystem, kpidUser, kpidGroup, kpidBlock, kpidComment, kpidPosition, kpidPrefix, kpidNumSubDirs,
    kpidNumSubFiles, kpidUnpackVer, kpidVolume, kpidIsVolume, kpidOffset, kpidLinks, kpidNumBlocks, kpidNumVolumes,
    kpidTimeType, kpidBit64, kpidBigEndian, kpidCpu, kpidPhySize, kpidHeadersSize, kpidChecksum, kpidCharacts,
    kpidVa, kpidId, kpidShortName, kpidCreatorApp, kpidSectorSize, kpidPosixAttrib, kpidSymLink, kpidError,
    kpidTotalSize, kpidFreeSpace, kpidClusterSize, kpidVolumeName, kpidLocalName, kpidProvider, kpidNtSecure,
    kpidIsAltStream, kpidIsAux, kpidIsDeleted, kpidIsTree, kpidSha1, kpidSha256, kpidErrorType, kpidNumErrors,
    kpidErrorFlags, kpidWarningFlags, kpidWarning, kpidNumStreams, kpidNumAltStreams, kpidAltStreamsSize,
    kpidVirtualSize, kpidUnpackSize, kpidTotalPhySize, kpidVolumeIndex, kpidSubType, kpidShortComment,
    kpidCodePage, kpidIsNotArcType, kpidPhySizeCantBeDetected, kpidZerosTailIsAllowed, kpidTailSize,
    kpidEmbeddedStubSize, kpidNtReparse, kpidHardLink, kpidINode, kpidStreamId, kpidReadOnly, kpidOutName,
    kpidCopyLink, kpidArcFileName, kpidIsHash, kpidChangeTime, kpidUserId, kpidGroupId, kpidDeviceMajor,
    kpidDeviceMinor, kpidDevMajor, kpidDevMinor
};

}  // namespace sz
