# mwait

This project showcases a simple use of the `monitor`/`mwait` x64 instructions for monitoring writes to certain addresses.

My understanding is that these instructions were created to provide support for spinlock-like mechanims. It is also used in HAL functionality to identify writes to I/O ports (HalpBlkIdleMonitorMWait).

Note that no compatibility checks are made. If you run this code in an CPU with no `monitor` support it will cause #UD. Compatibility can be checked via `CPUID.0000_0001_ECX[MONITOR]`.