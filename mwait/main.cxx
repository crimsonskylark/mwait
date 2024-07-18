#include "include.hpp"

struct INTERRUPT_GUARD
{
    INTERRUPT_GUARD( )
    {
        _disable ( );
    }

    ~INTERRUPT_GUARD( )
    {
        _enable ( );
    }
};

VOID Monitor( _In_ VOID *Context )
{
    const auto Address = static_cast< mw::MONITOR_CONTEXT* >(
        Context
    )->MonitoredAddress;

    logmsg( "Monitoring address %p for writes.\n", Address );

    ULONG64 MostRecentRead = 0llu;

    /*
     * `_mm_mwait` will halt execution, therefore we don't want this thread running on the same CPU as the 'manager'.
     *  Ideally, we should check for IRQL before calling this, as it is only guaranteed the thread will migrate
     *  to (one) of the target CPUs if at <= APC_LEVEL.
     */
    KeSetSystemAffinityThread( mw::MONITOR_THREAD_CPU_AFFINITY );

    NT_ASSERT( KeGetCurrentProcessorNumber( ) != mw::MONITOR_THREAD_CPU_AFFINITY );

    for ( ;; )
    {
        volatile INTERRUPT_GUARD _ { };

        const auto Start = __rdtsc ( );

        /*
        * According to the manual: "MONITOR performs the same segmentation and paging checks as a 1-byte read."
        * Therefore, an attempt to monitor an invalid address will raise an exception.
        * We are also dereferencing the monitored address in order to retrieve the data, which is always risky.
        * However, since we disabled interrupts earlier faulting here would lead to BugCheck even with exception handling.
        *
        * Finally, the correct way of using this requires checking the caching policy for the monitored address/page.
        * The manual is very pedantic about the fact we must only monitor addresses using the *write-back* policy type.
        * But seeing as we haven't bothered with the `cpuid` compatibility check either perhaps two wrongs do make a right...
        */
        _mm_monitor(
            reinterpret_cast< void* >( Address ),
            0lu,
            0lu
        );

        /*
         * Wait for it to trigger whenever some instruction writes to `Context->MonitoredAddress`.
         * Note: `mwait` behaves very much like the halt instruction (`hlt`) as far as I can see. I am not aware of any way to distinguish between them.
         * This also means that the CPU we pinned this thread to will be unusable for the duration of the waiting as it will transition to a low-power state.
         *
         * Further, the waiting state may also exit early due to a variety of reasons, such as:
         *  1) Reset signal;
         *  2) Any unmasked interrupt including INTR, NMI, SMI, INIT; and
         *  3) Others not directly specified by the manual but alluded to by the wording.
         *
         * Reason 2) is why we disabled interrupts before arming the monitor hardware.
        */
        _mm_mwait( 0lu, 0lu );

        /*
         * If we get here then one of two things happened:
         *
         *  1) A store occurred to the monitored address; or
         *  2) The waiting state exited early.
         *
         *  Per the documentation there doesn't seem to be any way to identify what caused
         *  the wait to expire. Due to this fact, we have to manually check whether the store occurred or not.
         *
         *  This implementation does not account for the fact that the same value may have been written to the monitored address.
         */

        const ULONG64 Previous = MostRecentRead;
        MostRecentRead = *reinterpret_cast< ULONG_PTR* >( Address );

        if ( Previous != MostRecentRead )
        {
            logmsg( "[%lx] Store detected on %p: 0x%llx != 0x%llx | delta: %llu\n",
                    KeGetCurrentProcessorNumber( ),
                    Address,
                    Previous,
                    MostRecentRead,
                    __rdtsc ( ) - Start
            );
        }

        /* 🤭 */
        if ( MostRecentRead == mw::MAGIC )
        {
            break;
        }
    }
}


VOID Worker( _In_ VOID *Context )
{
    const auto Ext = static_cast< mw::MWDEVICE_EXTENSION* >(
        Context
    );

    /* `_mm_mwait` will halt execution, hence we don't want this thread running on the same CPU as the worker.*/
    KeSetSystemAffinityThread( mw::WORKER_THREAD_CPU_AFFINITY );

    NT_ASSERT( KeGetCurrentProcessorNumber( ) != mw::WORKER_THREAD_CPU_AFFINITY );

    auto Status = PsCreateSystemThread(
        &Ext->MonitorThreadHandle,
        THREAD_ALL_ACCESS,
        nullptr,
        NtCurrentProcess ( ),
        &Ext->MonitorCid,
        Monitor,
        &Ext->MonitorContext
    );

    if ( !NT_SUCCESS( Status ) )
    {
        logmsg( "Unable to create thread: 0x%08x\n", Status );
        return;
    }

    for ( ;; )
    {
        const auto IsExiting = (
            KeWaitForSingleObject( &Ext->Unload, Executive, KernelMode, false, &mw::NoSleep ) == STATUS_SUCCESS
        );

        if ( IsExiting )
        {
            *reinterpret_cast< ULONG_PTR* >( Ext->MonitorContext.MonitoredAddress ) = mw::MAGIC;
            break;
        }

        // Occasionally write to the pointer.
        const auto TimeStamp = __rdtsc ( );

        if ( ( TimeStamp & 0xff ) == 0 )
        {
            *reinterpret_cast< ULONG_PTR* >( Ext->MonitorContext.MonitoredAddress ) = TimeStamp;
        }

        KeDelayExecutionThread( KernelMode, false, &mw::Sleep );
    }

    PETHREAD MonitorThreadObject = nullptr;
    Status = PsLookupThreadByThreadId( Ext->MonitorCid.UniqueThread, &MonitorThreadObject );

    if ( NT_SUCCESS( Status ) )
    {
        KeWaitForSingleObject(
            MonitorThreadObject,
            Executive,
            KernelMode,
            false,
            nullptr
        );

        /*
         * This handle was opened by PsCreateSystemThread.
         */
        ZwClose( Ext->MonitorThreadHandle );

        /*
         * Reference acquired by `PsLookupThreadByThreadId`.
         */
        ObfDereferenceObject( MonitorThreadObject );
    }
}

NTSTATUS DrvCreateClose( PDEVICE_OBJECT DeviceObject, PIRP Irp )
{
    UNREFERENCED_PARAMETER( DeviceObject );

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCompleteRequest( Irp, IO_NO_INCREMENT );

    return STATUS_SUCCESS;
}

VOID DriverUnload( PDRIVER_OBJECT DriverObject )
{
    const auto Device = DriverObject->DeviceObject;

    const auto Ext = static_cast< mw::MWDEVICE_EXTENSION* >(
        Device->DeviceExtension
    );

    KeSetEvent( &Ext->Unload, 0, false );


    PETHREAD WorkerObject = nullptr;
    const auto Status = PsLookupThreadByThreadId( Ext->WorkerCid.UniqueThread, &WorkerObject );

    if ( NT_SUCCESS( Status ) )
    {
        KeWaitForSingleObject(
            WorkerObject,
            Executive,
            KernelMode,
            false,
            nullptr
        );

        logmsg( "Worker thread exited\n" );

        /* This handle was opened by PsCreateSystemThread */
        ZwClose( Ext->WorkerHandle );

        ObfDereferenceObject( WorkerObject );
    }

    IoDeleteSymbolicLink( &mw::SYMLINK_NAME );
    IoDeleteDevice( Device );

    logmsg( "Bye from %p\n", WorkerObject );
}

EXTERN_C NTSTATUS DriverEntry( PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath )
{
    UNREFERENCED_PARAMETER( RegistryPath );

    NTSTATUS Status = STATUS_SUCCESS;

    bool CreatedSymbolicLink = false;

    PDEVICE_OBJECT DeviceObject = nullptr;

    constexpr auto MW_DEVEXT_SIZE = sizeof( mw::MWDEVICE_EXTENSION );

    do
    {
        Status = IoCreateDevice(
            DriverObject,
            MW_DEVEXT_SIZE,
            &mw::DEVICE_NAME,
            0,
            0,
            false,
            &DeviceObject
        );

        if ( !NT_SUCCESS( Status ) )
        {
            logmsg( "Unable to create device: 0x%08x\n", Status );
            break;
        }

        Status = IoCreateSymbolicLink(
            &mw::SYMLINK_NAME,
            &mw::DEVICE_NAME
        );

        CreatedSymbolicLink = NT_SUCCESS( Status );

        if ( !CreatedSymbolicLink )
        {
            logmsg( "Unable to create symbolic link: 0x%08x\n", Status );
            break;
        }
    }
    while ( false );

    if ( !NT_SUCCESS( Status ) )
    {
        if ( CreatedSymbolicLink )
            IoDeleteSymbolicLink( &mw::SYMLINK_NAME );

        if ( DeviceObject )
            IoDeleteDevice( DeviceObject );

        return Status;
    }

    const auto Ext = static_cast< mw::MWDEVICE_EXTENSION* >(
        DeviceObject->DeviceExtension
    );

    memset( Ext, 0, MW_DEVEXT_SIZE );

    Ext->Self = DeviceObject;

    KeInitializeEvent( &Ext->Unload, NotificationEvent, false );

    DriverObject->MajorFunction[ IRP_MJ_CREATE ] =
            DriverObject->MajorFunction[ IRP_MJ_CLOSE ] = DrvCreateClose;
    DriverObject->DriverUnload = DriverUnload;

    DeviceObject->Flags |= DO_BUFFERED_IO;

    Ext->MonitorContext.MonitoredAddress = reinterpret_cast< ULONG_PTR >(
        &mw::TestVariable
    );

    Status = PsCreateSystemThread(
        &Ext->WorkerHandle,
        THREAD_ALL_ACCESS,
        nullptr,
        NtCurrentProcess ( ),
        &Ext->WorkerCid,
        Worker,
        Ext
    );

    if ( !NT_SUCCESS( Status ) )
    {
        logmsg( "Unable to create system thread: 0x%08x\n", Status );

        IoDeleteSymbolicLink( &mw::SYMLINK_NAME );
        IoDeleteDevice( DeviceObject );
    }

    return STATUS_SUCCESS;
}
