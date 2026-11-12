// The debug log, written off the main thread into a file that does not grow.
// See dbglog.h for why; TASKS.md #233 for the measurement.
//
// The evidence, from the user's 10-23 hardware log: the two "few-second
// freezes" are the two 120-frame profiler windows reading `sim 21.6` and
// `sim 18.1` against a usual 2-5, ~2 s of extra time each, and the log line
// that closed each window is the one that took nfsu2_debug.txt past 32,768
// and past 65,536 bytes. Crossing 49,152 cost nothing (`sim 4.5`). So the
// card's clusters are 32 KB, and every time the file needed a new one, the
// main thread's fopen("a")/fprintf/fclose sat in the FS while the FAT was
// searched for a free cluster. `sim` caught it because the per-second
// telemetry lines are logged from worldInput().
//
// Two separate fixes, because either alone leaves something behind:
//  * The writer thread means a slow card never stops a frame. But FS requests
//    from every thread queue behind a two-second allocation, so the music
//    stream (670 ms of slack, audio.c) and the section loader would still
//    stall.
//  * Keeping the file's size from run to run means there is no allocation to
//    stall on. The first run after this change grows the file once; every run
//    after that overwrites clusters the file already owns.

#include "dbglog.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH     "/nfsu2_debug.txt"
// A free-roam hour logs a little over a megabyte (a section load a second,
// the telemetry for the first ten minutes), so one step covers a long session
// and growing mid-run is the exception. Growing is one SetSize however many
// clusters it adds; whether the user's card makes that one FAT search or one
// per cluster is not known yet, and is what the "[log] ... grown" line times.
#define LOG_STEP     (2u * 1024u * 1024u)   // the file's size is whole steps of this
// #522 (the user, 2026-11-11: "remove the 2 MB cap on logging"): the file
// grows a step at a time with no cap of its own (it was 8 steps, 16 MB); the
// only limit left is FAT32's 4 GB file, the last whole step under it.
#define LOG_MAX      (0xFFFFFFFFull - 0xFFFFFFFFull % LOG_STEP)
#define RING_BYTES   (128u * 1024u)    // a power of two: the indices wrap with u32
#define STAGE_BYTES  (16u * 1024u)     // the most the writer takes in one write
#define BLANK_BYTES  (32u * 1024u)     // one blanking pass, read then (maybe) written
#define BLANK_LINE   64                // 63 spaces and a newline

static char s_ring[RING_BYTES];
static u32 s_head;      // bytes ever queued
static u32 s_tail;      // bytes ever taken by the writer
static volatile u32 s_done;   // bytes ever put on the card (or given up on)
static u32 s_dropped;   // lines refused since the last notice

static LightLock s_lock;  // the ring
static LightLock s_io;    // the file; only contended when there is no thread
static LightEvent s_wake;
static Thread s_thread;
static s32 s_prio;        // the writer's, one below the main thread
static volatile bool s_run;
static bool s_inited;
static bool s_started;   // dbglog_start(true) opened the file (#533)
static bool s_disabled;  // dbglog_start(false), or the file would not open

static FS_Archive s_arch;
static Handle s_file;
static bool s_open;
static u64 s_off;     // where this run's next byte goes
static u64 s_cap;     // the file's size, i.e. what it has clusters for
static u64 s_blank;   // [s_blank, s_cap) may still hold an earlier run's text

static char s_stage[STAGE_BYTES];
static char s_pattern[BLANK_BYTES + BLANK_LINE];
static char s_readBack[BLANK_BYTES];
static char s_note[128];   // a line the writer has to say about itself

static u32 msSince(u64 t0) {
	return (u32)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000));
}

// Makes the file at least `need` bytes, in whole LOG_STEPs. This is the
// allocation the whole module exists to keep off the main thread, so it says
// how long it took.
static bool grow(u64 need) {
	if (need <= s_cap)
		return true;
	u64 cap = s_cap;
	while (cap < need)
		cap += LOG_STEP;
	if (cap > LOG_MAX)
		return false;
	u64 t0 = svcGetSystemTick();
	if (R_FAILED(FSFILE_SetSize(s_file, cap)))
		return false;
	snprintf(s_note, sizeof(s_note), "[log] nfsu2_debug.txt grown %lu -> %lu KB in %lu ms\n",
		(unsigned long)(s_cap >> 10), (unsigned long)(cap >> 10), (unsigned long)msSince(t0));
	s_cap = cap;
	return true;
}

// Writes at the end of this run's text. Caller holds s_io.
static void put(const char* p, u32 n) {
	if (!s_open || n == 0 || !grow(s_off + n))
		return;   // past LOG_MAX: dropped
	u32 wrote = 0;
	FSFILE_Write(s_file, &wrote, s_off, p, n, FS_WRITE_FLUSH);
	s_off += n;
	if (s_blank < s_off)
		s_blank = s_off;
}

static void putNote(void) {
	if (!s_note[0])
		return;
	char line[sizeof(s_note)];
	memcpy(line, s_note, sizeof(line));
	s_note[0] = '\0';
	put(line, (u32)strlen(line));
}

// One pass of blanking an earlier run's text past where this run has got to.
// Read first: after the first few runs the region is already blank, and a
// read costs the card nothing. The pattern is aligned to the file, so the
// tail reads as whole lines wherever this run's text happens to stop. Caller
// holds s_io.
static void blankStep(void) {
	u64 at = s_blank;
	u32 n = (u32)(s_cap - at < BLANK_BYTES ? s_cap - at : BLANK_BYTES);
	const char* want = s_pattern + at % BLANK_LINE;
	u32 got = 0;
	if (R_FAILED(FSFILE_Read(s_file, &got, at, s_readBack, n)) || got != n ||
	    memcmp(s_readBack, want, n) != 0) {
		u32 wrote = 0;
		FSFILE_Write(s_file, &wrote, at, want, n, 0);
	}
	s_blank = at + n;
}

// Sizes the file for this run: a whole number of steps, never smaller than
// one. A file between steps is from before this module and is rounded down
// (freeing clusters is cheap) unless that would leave nothing, in which case
// it is grown -- once, on the first run after the change.
static void prepare(void) {
	u64 size = 0;
	FSFILE_GetSize(s_file, &size);
	u64 cap = size - size % LOG_STEP;
	if (cap > LOG_MAX)
		cap = LOG_MAX;
	if (cap == 0) {
		s_cap = size;
		grow(LOG_STEP);
	} else {
		if (cap != size)
			FSFILE_SetSize(s_file, cap);
		s_cap = cap;
		snprintf(s_note, sizeof(s_note), "[log] nfsu2_debug.txt reusing %lu KB from the last run\n",
			(unsigned long)(cap >> 10));
	}
	s_off = 0;
	s_blank = 0;
	putNote();
}

// Takes everything queued and writes it. Caller holds s_io.
static void drain(void) {
	for (;;) {
		LightLock_Lock(&s_lock);
		u32 n = s_head - s_tail;
		if (n > STAGE_BYTES)
			n = STAGE_BYTES;
		u32 at = s_tail % RING_BYTES;
		u32 first = RING_BYTES - at < n ? RING_BYTES - at : n;
		memcpy(s_stage, s_ring + at, first);
		memcpy(s_stage + first, s_ring, n - first);
		s_tail += n;
		LightLock_Unlock(&s_lock);
		if (n == 0)
			return;
		put(s_stage, n);
		putNote();
		s_done += n;
	}
}

static void writerThread(void* arg) {
	(void)arg;
	LightLock_Lock(&s_io);
	prepare();
	LightLock_Unlock(&s_io);
	for (;;) {
		bool run = s_run;   // read first, so a line queued before exit is drained
		LightLock_Lock(&s_io);
		drain();
		bool blanking = s_blank < s_cap;
		if (!run)
			while (s_blank < s_cap)
				blankStep();
		else if (blanking)
			blankStep();
		LightLock_Unlock(&s_io);
		if (!run)
			break;
		if (!blanking)
			LightEvent_Wait(&s_wake);
	}
}

// Caller holds s_lock.
static void ringPut(const char* p, u32 n) {
	u32 at = s_head % RING_BYTES;
	u32 first = RING_BYTES - at < n ? RING_BYTES - at : n;
	memcpy(s_ring + at, p, first);
	memcpy(s_ring, p + first, n - first);
	s_head += n;
}

static void submit(const char* a, u32 na, const char* b, u32 nb, bool newline) {
	if (!s_inited)
		dbglog_init();
	if (s_disabled)
		return;   // DebugLogging is off (#533)
	u32 n = na + nb + (newline ? 1 : 0);
	LightLock_Lock(&s_lock);
	if (s_dropped) {
		char notice[80];
		u32 k = (u32)snprintf(notice, sizeof(notice),
			"[log] %lu line(s) dropped -- the card fell behind\n", (unsigned long)s_dropped);
		if (RING_BYTES - (s_head - s_tail) >= k + n) {
			ringPut(notice, k);
			s_dropped = 0;
		}
	}
	if (RING_BYTES - (s_head - s_tail) >= n) {
		ringPut(a, na);
		ringPut(b, nb);
		if (newline)
			ringPut("\n", 1);
	} else {
		s_dropped++;
	}
	LightLock_Unlock(&s_lock);
	if (!s_started)
		return;   // held in the ring until dbglog_start() (#533)
	if (s_thread) {
		LightEvent_Signal(&s_wake);
	} else {
		LightLock_Lock(&s_io);
		drain();
		LightLock_Unlock(&s_io);
	}
}

void dbglog_init(void) {
	if (s_inited)
		return;
	s_inited = true;
	LightLock_Init(&s_lock);
	LightLock_Init(&s_io);
	LightEvent_Init(&s_wake, RESET_ONESHOT);
	memset(s_pattern, ' ', sizeof(s_pattern));
	for (u32 i = BLANK_LINE - 1; i < sizeof(s_pattern); i += BLANK_LINE)
		s_pattern[i] = '\n';
}

void dbglog_start(bool on) {
	if (!s_inited)
		dbglog_init();
	if (s_started || s_disabled)
		return;
	if (!on) {
		// DebugLogging is off: what the boot queued is dropped, and so is
		// everything after (submit() returns at once). No file is opened or
		// made, so the card is never written to.
		s_disabled = true;
		LightLock_Lock(&s_lock);
		s_tail = s_head;
		s_done = s_head;
		LightLock_Unlock(&s_lock);
		return;
	}

	// The FS archive directly rather than stdio: one handle for the whole run,
	// and writes at an offset, which is what lets the file keep its size.
	if (R_FAILED(FSUSER_OpenArchive(&s_arch, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
		s_disabled = true;
		return;
	}
	if (R_FAILED(FSUSER_OpenFile(&s_file, s_arch, fsMakePath(PATH_ASCII, LOG_PATH),
	        FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
		FSUSER_CloseArchive(s_arch);
		s_disabled = true;
		return;
	}
	s_open = true;

	// One below the main thread, like the section loader: it lives blocked
	// in the FS and never needs to preempt drawing.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio += 1;
	if (prio > 0x3F)
		prio = 0x3F;
	s_run = true;
	s_prio = prio;
	s_thread = threadCreate(writerThread, NULL, 16 * 1024, prio, -1, false);
	if (!s_thread) {
		// No thread: every line is written by its caller, as before, but the
		// file still keeps its size, which is the half that removes the stall.
		s_run = false;
		LightLock_Lock(&s_io);
		prepare();
		while (s_blank < s_cap)
			blankStep();
		LightLock_Unlock(&s_io);
	}
	// The lines the boot queued before the config was read go out now.
	s_started = true;
	if (s_thread) {
		LightEvent_Signal(&s_wake);
	} else {
		LightLock_Lock(&s_io);
		drain();
		LightLock_Unlock(&s_io);
	}
}

void dbglog_line(const char* prefix, const char* msg) {
	submit(prefix ? prefix : "", prefix ? (u32)strlen(prefix) : 0, msg, (u32)strlen(msg), true);
}

void dbglog_vprintf(const char* prefix, const char* fmt, va_list ap) {
	char b[512];
	int n = vsnprintf(b, sizeof(b), fmt, ap);
	if (n < 0)
		return;
	if (n >= (int)sizeof(b))
		n = sizeof(b) - 1;
	submit(prefix ? prefix : "", prefix ? (u32)strlen(prefix) : 0, b, (u32)n, false);
}

void dbglog_urgent(bool on) {
	if (s_thread)
		svcSetThreadPriority(threadGetHandle(s_thread), on ? s_prio - 2 : s_prio);
}

void dbglog_flush(void) {
	if (!s_thread)
		return;
	LightLock_Lock(&s_lock);
	u32 target = s_head;
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_wake);
	for (int i = 0; i < 3000 && (s32)(s_done - target) < 0; i++)
		svcSleepThread(1000000LL);
}

void dbglog_exit(void) {
	if (s_thread) {
		s_run = false;
		LightEvent_Signal(&s_wake);
		threadJoin(s_thread, 5000000000ULL);
		threadFree(s_thread);
		s_thread = NULL;
	}
	if (s_open) {
		FSFILE_Close(s_file);
		FSUSER_CloseArchive(s_arch);
		s_open = false;
	}
}
