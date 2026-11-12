// See gpuwatch.h.
#include "gpuwatch.h"

#include <stdio.h>
#include <string.h>

#include "dbglog.h"

// citro3d's context: its first member is the gxCmdQueue_s C3D_Init() binds
// to GX, as world.c's GPU kicks read it.
extern gxCmdQueue_s __C3D_Context;

#define WATCH_NS      250000000LL   // four looks a second
#define GPU_STALL_MS  1000          // a frame's whole queue takes ~15 ms
#define CPU_STALL_MS  5000          // a load's longest gap between frames is ~150 ms
#define TRACE_WAIT_MS 1000          // one stage of one frame: well under a millisecond
#define LABELS        32            // C3D_Init()'s queue

static Thread s_thread;
static volatile bool s_run;
static volatile bool s_suspended;
static aptHookCookie s_hook;
static volatile u32 s_frames;
static const char* volatile s_stage = "start";
// The stage that queued each command list, by queue index, with the list's
// address: an index the queue has reused since shows no stage.
static const char* s_label[LABELS];
static u32 s_labelAddr[LABELS];
static u16 s_marked;

// The trace (#308).
static void (*s_kick)(void);
static int  s_traceFrame;
static bool s_tracing;
static bool s_traceHung;   // a stage never finished: nothing more goes to the GPU
static u64  s_traceTick;

static u32 msSince(u64 t, u64 now) {
	return (u32)((now - t) / (SYSCLOCK_ARM11 / 1000));
}

void gpuwatch_frame(void) {
	s_frames++;
}

void gpuwatch_stage(const char* stage) {
	s_stage = stage;
}

void gpuwatch_mark_queue(void) {
	const gxCmdQueue_s* q = &__C3D_Context;
	const u16 n = q->numEntries;
	if (n < s_marked)
		s_marked = 0;   // a new frame's queue
	for (u16 i = s_marked; i < n && i < LABELS; i++) {
		if ((q->entries[i].data[0] & 0xFF) == 1) {
			s_label[i] = s_stage;
			s_labelAddr[i] = q->entries[i].data[1];
		}
	}
	s_marked = n;
}

void gpuwatch_trace_begin(void (*kick)(void), int frame) {
	s_kick = kick;
	s_traceFrame = frame;
	s_tracing = true;
	char b[120];
	snprintf(b, sizeof(b), "gpu trace f%d: each stage's line is on the card before the GPU "
		"gets it", frame);
	dbglog_line(NULL, b);
}

void gpuwatch_trace_end(void) {
	s_tracing = false;
}

bool gpuwatch_tracing(void) {
	return s_tracing;
}

int gpuwatch_trace_frame(void) {
	return s_tracing ? s_traceFrame : 0;
}

void gpuwatch_trace_say(const char* what) {
	s_stage = what;
	if (!s_tracing)
		return;
	char b[200];
	snprintf(b, sizeof(b), "gpu trace f%d: %s%s", s_traceFrame, what,
		s_traceHung ? " (recorded, not sent: the GPU is stuck)" : " -> GPU");
	dbglog_line(NULL, b);
	dbglog_flush();
	s_traceTick = svcGetSystemTick();
}

void gpuwatch_trace_wait(const char* what) {
	if (!s_tracing || s_traceHung)
		return;
	gxCmdQueue_s* q = &__C3D_Context;
	const bool done = gxCmdQueueWait(q, (s64)TRACE_WAIT_MS * (SYSCLOCK_ARM11 / 1000));
	const u64 now = svcGetSystemTick();
	char b[240];
	if (done) {
		snprintf(b, sizeof(b), "gpu trace f%d: %s done, %lu us", s_traceFrame, what,
			(unsigned long)((now - s_traceTick) / (SYSCLOCK_ARM11 / 1000000)));
		dbglog_line(NULL, b);
		// Idle, so the frame's queue can start over: a traced frame queues
		// more than C3D_Init()'s 32 entries, one for each stage.
		gxCmdQueueClear(q);
	} else {
		snprintf(b, sizeof(b), "gpu trace f%d: %s NOT done after %d ms -- the GPU hung on it "
			"(queue %u of %u done, %u sent)", s_traceFrame, what, TRACE_WAIT_MS,
			q->lastEntry, q->numEntries, q->curEntry);
		dbglog_line(NULL, b);
		dbglog_flush();
		s_traceHung = true;
	}
}

void gpuwatch_trace(const char* what) {
	gpuwatch_trace_say(what);
	if (!s_tracing || s_traceHung)
		return;
	if (s_kick)
		s_kick();
	gpuwatch_trace_wait(what);
}

static const char* kindName(u32 id) {
	switch (id) {
	case 0: return "dma";
	case 1: return "list";
	case 2: return "fill";
	case 3: return "transfer";
	case 4: return "copy";
	case 5: return "flush";
	}
	return "?";
}

static int describe(char* b, int cap, const gxCmdQueue_s* q, u16 i) {
	const u32* d = q->entries[i].data;
	const u32 id = d[0] & 0xFF;
	switch (id) {
	case 1:
		return snprintf(b, (size_t)cap, " #%u list '%s' %08lx +%lx", i,
			i < LABELS && s_labelAddr[i] == d[1] && s_label[i] ? s_label[i] : "?",
			(unsigned long)d[1], (unsigned long)d[2]);
	case 2:
		return snprintf(b, (size_t)cap, " #%u fill %08lx-%08lx %08lx-%08lx", i,
			(unsigned long)d[1], (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[6]);
	case 3: case 4:
		return snprintf(b, (size_t)cap, " #%u %s %08lx -> %08lx", i, kindName(id),
			(unsigned long)d[1], (unsigned long)d[2]);
	}
	return snprintf(b, (size_t)cap, " #%u %s %08lx", i, kindName(id), (unsigned long)d[0]);
}

// Reads memory and logs, nothing else. The #307 build also read the GPU's
// registers through GSP and replayed the interrupt a stuck command waited
// for; on the 3DS that run's hang took the music and the log writer with it.
static void watchThread(void* arg) {
	(void)arg;
	u32 frames = s_frames;
	u64 frameTick = svcGetSystemTick();
	u16 qn = 0, qc = 0, ql = 0;
	u64 qSince = frameTick;   // the queue as it is now, since when
	u64 saidTick = 0;
	bool cpuSaid = false, gpuSaid = false, urgent = false;
	while (s_run) {
		svcSleepThread(WATCH_NS);
		const u64 now = svcGetSystemTick();
		if (s_suspended) {
			frames = s_frames;
			frameTick = qSince = now;
			continue;
		}
		const gxCmdQueue_s* q = &__C3D_Context;
		const u16 n = q->numEntries, c = q->curEntry, l = q->lastEntry;
		const bool moved = s_frames != frames;
		if (moved || n != qn || c != qc || l != ql || c <= l) {
			if (gpuSaid)
				dbglog_line(NULL, "gpuwatch: the GPU queue is moving again");
			qn = n; qc = c; ql = l;
			qSince = now;
			gpuSaid = false;
		} else if (msSince(qSince, now) >= GPU_STALL_MS &&
				(!gpuSaid || msSince(saidTick, now) >= CPU_STALL_MS)) {
			// Commands out, nothing back for a second: said then, and every
			// five seconds while it lasts.
			char b[600];
			int k = snprintf(b, sizeof(b), "gpuwatch: GPU queue stuck %lu ms (frame %lu, "
				"main thread at '%s'): %u of %u done, %u sent --",
				(unsigned long)msSince(qSince, now), (unsigned long)s_frames, s_stage,
				l, n, c);
			for (u16 i = l; i < c && i < l + 4 && k < (int)sizeof(b) - 80; i++)
				k += describe(b + k, (int)sizeof(b) - k, q, i);
			if (c < n && k < (int)sizeof(b) - 80) {
				k += snprintf(b + k, sizeof(b) - (size_t)k, "; next");
				describe(b + k, (int)sizeof(b) - k, q, c);
			}
			dbglog_urgent(true);
			urgent = true;
			dbglog_line(NULL, b);
			gpuSaid = true;
			saidTick = now;
		}
		if (moved) {
			if (cpuSaid) {
				char b[120];
				snprintf(b, sizeof(b), "gpuwatch: frames again after %lu ms",
					(unsigned long)msSince(frameTick, now));
				dbglog_line(NULL, b);
			}
			if (urgent)
				dbglog_urgent(false);
			urgent = false;
			frames = s_frames;
			frameTick = now;
			cpuSaid = false;
		} else if (!cpuSaid && !gpuSaid && msSince(frameTick, now) >= CPU_STALL_MS) {
			char b[200];
			snprintf(b, sizeof(b), "gpuwatch: no frame for %lu ms (frame %lu), main thread at "
				"'%s'; GX queue %u of %u done, %u sent", (unsigned long)msSince(frameTick, now),
				(unsigned long)s_frames, s_stage, l, n, c);
			dbglog_urgent(true);
			urgent = true;
			dbglog_line(NULL, b);
			cpuSaid = true;
		}
	}
}

static void onApt(APT_HookType hook, void* param) {
	(void)param;
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP)
		s_suspended = true;
	else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
		s_suspended = false;
}

void gpuwatch_start(void) {
	if (s_thread)
		return;
	aptHook(&s_hook, onApt, NULL);
	s_run = true;
	// Above the main thread and the audio stream (one above it), on the same
	// core: it sleeps all but a few microseconds a second.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio -= 4;
	if (prio < 0x18)
		prio = 0x18;
	s_thread = threadCreate(watchThread, NULL, 8 * 1024, prio, -1, false);
	if (!s_thread) {
		s_run = false;
		aptUnhook(&s_hook);
		dbglog_line(NULL, "gpuwatch: no thread -- no watchdog this run");
	}
}

void gpuwatch_stop(void) {
	if (!s_thread)
		return;
	s_run = false;
	threadJoin(s_thread, 1000000000ULL);
	threadFree(s_thread);
	s_thread = NULL;
	aptUnhook(&s_hook);
}
