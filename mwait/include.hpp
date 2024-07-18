#pragma once

#include <ntifs.h>
#include <intrin.h>

#define logmsg(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_MASK | DPFLTR_INFO_LEVEL, "[" __FUNCTION__ "] " ##__VA_ARGS__)

namespace mw
{
    inline UNICODE_STRING DEVICE_NAME = RTL_CONSTANT_STRING( L"\\Device\\Mwait" );
    inline UNICODE_STRING SYMLINK_NAME = RTL_CONSTANT_STRING( L"\\??\\Mwait" );

    inline LARGE_INTEGER Sleep = { .QuadPart = -( 1 * 100 * 1000 ) };
    inline LARGE_INTEGER NoSleep = { .QuadPart = 0 };

    inline ULONG64 TestVariable = 0llu;

    constexpr ULONG MONITOR_THREAD_CPU_AFFINITY = 1;
    constexpr ULONG WORKER_THREAD_CPU_AFFINITY = 4;

    constexpr ULONG64 MAGIC = 0xEEFFEEFFEEFFEEFF;
    constexpr ULONG THREAD_COUNT = 2lu;

    struct MONITOR_CONTEXT
    {
        ULONG_PTR MonitoredAddress;
        KEVENT MonitorExit;
    };

    struct MWDEVICE_EXTENSION
    {
        HANDLE WorkerHandle;
        CLIENT_ID WorkerCid;

        HANDLE MonitorThreadHandle;
        CLIENT_ID MonitorCid;

        PDEVICE_OBJECT Self;
        KEVENT Unload;

        MONITOR_CONTEXT MonitorContext;
    };
}
