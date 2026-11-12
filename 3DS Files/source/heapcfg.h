#ifndef NFSU2_HEAPCFG_H
#define NFSU2_HEAPCFG_H

#include <3ds.h>

// Filled in by __system_allocateHeaps() (heapcfg.c) before main() runs, so
// initPlatformPerf() can log what the console actually handed over instead of
// what this build asked for. Plain integers only: this struct is populated
// before the malloc heap exists, so the code that writes it cannot allocate,
// cannot printf, and cannot touch the filesystem.
typedef struct {
	u32 commitMax;    // RESLIMIT_COMMIT limit -- this IS the APPMEMTYPE grant
	u32 commitUsed;   // already committed when we ran (code + data + bss)
	u32 avail;        // commitMax - commitUsed, 4K aligned
	u32 linearAsked;  // the first linear size we tried
	u32 linearGot;    // what we actually got
	u32 linearTries;  // 1 = the first ask succeeded, >1 = the cap was too high
	u32 linearFailRc; // last failing Result from the linear step-down
	u32 heapAsked;
	u32 heapGot;
	u32 heapTries;
	u32 heapFailRc;
	u32 queryFailed;  // 1 = svcGetResourceLimit failed and avail is a guess
} NfsuHeapInfo;

extern NfsuHeapInfo g_heapInfo;

// What the two heaps can still hand out, for the logs that have to say why
// an allocation failed (#270). heap_malloc_free() is newlib's free list plus
// the part of the sbrk window it has not claimed yet. heap_linear_largest()
// is the biggest single linearAlloc() that would succeed right now, found by
// trying: linearSpaceFree() is the total, and a fragmented heap can refuse a
// 2 MB vertex pool with far more than that free. Main thread only -- the
// linear allocator is not thread-safe.
u32 heap_malloc_free(void);
u32 heap_linear_largest(void);

#endif
