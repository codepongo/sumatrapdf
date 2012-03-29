/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
License: Simplified BSD (see COPYING.BSD) */

/*
memtrace.dll enables tracing memory allocations in arbitrary programs.
It hooks RtlFreeHeap etc. APIs in the process and sends collected information
(allocation address/size, address of freed memory, callstacks if possible etc.)
to an external collection and visualization process via named pipe.

The dll can either be injected into arbitrary processes or an app can load it
by itself (easier to integrate than injecting dll).

If the collection process doesn't run when memtrace.dll is initialized, we do
nothing.
*/

#include <stddef.h> // for offsetof
#include "BaseUtil.h"
#include "MemTraceDll.h"

#include "nsWindowsDllInterceptor.h"
#include "StrUtil.h"
#include "Vec.h"

#define NOLOG 0  // always log
#include "DebugLog.h"

static HANDLE gModule;
static HANDLE gPipe;

enum SerializeMsgId {
    AllocDataMsgId  = 1,
    FreeDataMsgId   = 2
};

// TODO: since we use 32 bits, we don't support 64 builds
struct AllocData {
    uint32    size;
    uint32    addr;
};

struct FreeData {
    uint32    addr;
};

struct MemberSerializeInfo {
    enum Type { UInt16, Int32, UInt32, Int64, UInt64, Sentinel };
    Type    type;
    int     offset;

    bool IsSentinel() const { return Sentinel == type; };
};

struct TypeSerializeInfo {
    SerializeMsgId          msgId;
    MemberSerializeInfo *   members;
};

#define SERIALIZEINFO_SENTINEL { MemberSerializeInfo::Sentinel, 0 }

// TODO: manually defining serialization info is fine for few types
// but if we get more, this should be auto-generated by e.g. defining
// the types in C# and generating the C descriptions in C# by reflecting
// C# information.
MemberSerializeInfo allocDataSerMemberInfo[] = {
    { MemberSerializeInfo::UInt32, offsetof(AllocData, size) },
    { MemberSerializeInfo::UInt32, offsetof(AllocData, addr) },
    SERIALIZEINFO_SENTINEL
};

TypeSerializeInfo allocDataTypeInfo = {
    AllocDataMsgId,
    allocDataSerMemberInfo
};

MemberSerializeInfo freeDataSerMemberInfo[] = {
    { MemberSerializeInfo::UInt32, offsetof(FreeData, addr) },
    SERIALIZEINFO_SENTINEL
};

TypeSerializeInfo freeDataTypeInfo = {
    FreeDataMsgId,
    freeDataSerMemberInfo
};

// TODO: we should serialize all numbers using variable-length encoding
// like e.g. in snappy.
// Assumes that data points to either int16 or uint16 value. Appends
// serialized data to serOut and returns number of bytes needed to serialize
static inline int SerNum16(byte *data, Vec<byte>& serOut)
{
    byte *dataOut = serOut.AppendBlanks(2);
    *dataOut++ = *data++; *dataOut++ = *data++;
    return 2;
}

static inline int SerNum32(byte *data, Vec<byte>& serOut)
{
    byte *dataOut = serOut.AppendBlanks(4);
    *dataOut++ = *data++; *dataOut++ = *data++;
    *dataOut++ = *data++; *dataOut++ = *data++;
    return 4;
}

static inline int SerNum64(byte *data, Vec<byte>& serOut)
{
    byte *dataOut = serOut.AppendBlanks(8);
    *dataOut++ = *data++; *dataOut++ = *data++;
    *dataOut++ = *data++; *dataOut++ = *data++;
    *dataOut++ = *data++; *dataOut++ = *data++;
    *dataOut++ = *data++; *dataOut++ = *data++;
    return 8;
}

// data is a pointer to a struct being serialized and typeInfo describes
// the struct. res is a result as a stream of bytes
static void SerializeType(byte *data, TypeSerializeInfo *typeInfo, Vec<byte>& msg)
{
    msg.Reset();
    // reserve space for the size of the message, which we only know
    // after serializeing the data. We're making an assumption here
    // that serialized data will be smaller than 65k
    // note: we can easily relax that by using uint32 for len
    uint16 *msgLenPtr = (uint16*)msg.AppendBlanks(2);
    uint16 msgId = (uint16)typeInfo->msgId;
    int msgLen = 2 + SerNum16((byte*)&msgId, msg);

    for (MemberSerializeInfo *member = typeInfo->members; !member->IsSentinel(); ++ member) {
        switch (member->type) {
        case MemberSerializeInfo::UInt16:
            msgLen += SerNum16(data + member->offset, msg);
            break;
        case MemberSerializeInfo::Int32:
        case MemberSerializeInfo::UInt32:
            msgLen += SerNum32(data + member->offset, msg);
            break;
        case MemberSerializeInfo::Int64:
        case MemberSerializeInfo::UInt64:
            msgLen += SerNum32(data + member->offset, msg);
            break;
        }
    }
    *msgLenPtr = (uint16)msgLen;
}

WindowsDllInterceptor gNtdllIntercept;

//http://msdn.microsoft.com/en-us/library/windows/hardware/ff552108(v=vs.85).aspx
PVOID (WINAPI *gRtlAllocateHeapOrig)(PVOID heapHandle, ULONG flags, SIZE_T size);
// http://msdn.microsoft.com/en-us/library/windows/hardware/ff552276(v=vs.85).aspx
BOOLEAN (WINAPI *gRtlFreeHeapOrig)(PVOID heapHandle, ULONG flags, PVOID heapBase);

#define PIPE_NAME "\\\\.\\pipe\\MemTraceCollectorPipe"

// note: must be careful to not allocate memory in this function to avoid
// infinite recursion
PVOID WINAPI RtlAllocateHeapHook(PVOID heapHandle, ULONG flags, SIZE_T size)
{
    PVOID res = gRtlAllocateHeapOrig(heapHandle, flags, size);
    AllocData d = { (uint32)size, (uint32)res };
    Vec<byte> msg;
    SerializeType((byte*)&d, &allocDataTypeInfo, msg);

    return res;
}

BOOLEAN WINAPI RtlFreeHeapHook(PVOID heapHandle, ULONG flags, PVOID heapBase)
{
    BOOLEAN res = gRtlFreeHeapOrig(heapHandle, flags, heapBase);
    FreeData d = { (uint32)heapBase };
    Vec<byte> msg;
    SerializeType((byte*)&d, &freeDataTypeInfo, msg);
    return res;
}

static bool WriteToPipe(const char *s)
{
    DWORD size;
    if (!gPipe)
        return false;
    DWORD sLen = str::Len(s);
    BOOL ok = WriteFile(gPipe, s, (DWORD)sLen, &size, NULL);
    if (!ok || (size != sLen))
        return false;
    return true;
}

static bool TryOpenPipe()
{
    gPipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | FILE_FLAG_OVERLAPPED, NULL);
    if (INVALID_HANDLE_VALUE == gPipe) {
        gPipe = NULL;
        return false;
    }
    WriteToPipe("hello, sailor");
    return true;
}

static void ClosePipe()
{
    if (gPipe && (INVALID_HANDLE_VALUE != gPipe))
        CloseHandle(gPipe);
    gPipe = NULL;
}

static void InstallHooks()
{
    gNtdllIntercept.Init("ntdll.dll");
    bool ok = gNtdllIntercept.AddHook("RtlAllocateHeap", reinterpret_cast<intptr_t>(RtlAllocateHeapHook), (void**) &gRtlAllocateHeapOrig);
    if (ok)
        lf("Hooked RtlAllocateHeap");
    else
        lf("failed to hook RtlAllocateHeap");

    ok = gNtdllIntercept.AddHook("RtlFreeHeap", reinterpret_cast<intptr_t>(RtlFreeHeapHook), (void**) &gRtlFreeHeapOrig);
    if (ok)
        lf("Hooked RtlFreeHeap");
    else
        lf("failed to hook RtlFreeHeap");
}

static BOOL ProcessAttach()
{
    lf("ProcessAttach()");
    if (!TryOpenPipe()) {
        lf("couldn't open pipe");
        return FALSE;
    } else {
        lf("opened pipe");
    }
    InstallHooks();
    return TRUE;
}

static BOOL ProcessDetach()
{
    lf("ProcessDetach()");
    ClosePipe();
    return TRUE;
}

static BOOL ThreadAttach()
{
    return TRUE;
}

static BOOL ThreadDetach()
{
    return TRUE;
}

BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved)
{
    gModule = hModule;
    if (DLL_PROCESS_ATTACH == dwReason)
        return ProcessAttach();
    if (DLL_PROCESS_DETACH == dwReason)
        return ProcessDetach();
    if (DLL_THREAD_ATTACH == dwReason)
        return ThreadAttach();
    if (DLL_THREAD_DETACH == dwReason)
        return ThreadDetach();

    return TRUE;
}
