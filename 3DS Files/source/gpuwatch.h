// A watchdog over the main loop and citro3d's GX queue (TASKS.md #307), and
// a trace of the world's first frames on the GPU (#308).
//
// The 3DS froze on the first world frame with the loading screen still up,
// and the log stopped at the frame's first 2D line: the frame was recorded,
// and the GPU never finished it. Azahar ran the same save at 60 FPS. The
// second time the music stopped as well and the watchdog below, which then
// also read the GPU's registers and replayed the interrupt it waited for,
// said nothing: a GPU that hangs can take the console with it. So:
//
//   - The watchdog thread, above the main thread's priority, looks four times
//     a second and only reads memory. GX queue commands out and no progress
//     for a second: it logs the commands in flight (kind, addresses, the
//     stage that queued a command list). No frame for five seconds with the
//     queue idle: it logs the stage the main thread last named, and raises
//     the log writer's priority so the line reaches the card past a main
//     thread that never yields. Quiet while the app is suspended.
//   - The trace: in a traced frame each gpuwatch_trace() puts its line on the
//     card before the GPU sees a command of what it names, and then waits for
//     the GPU to finish it. Whatever the GPU dies on, console and all, is the
//     log's last line.
#pragma once

#include <3ds.h>

// After C3D_Init(); before C3D_Fini().
void gpuwatch_start(void);
void gpuwatch_stop(void);

// The main loop's heartbeat: once a frame, the loading screen's frames too.
void gpuwatch_frame(void);

// Where the main thread is, for a stall report: a string literal, kept until
// the next call.
void gpuwatch_stage(const char* stage);

// Names the command lists queued since the last call after the current
// stage. Call right after a C3D_FrameSplit() or a C3D_FrameEnd().
void gpuwatch_mark_queue(void);

// A traced frame, from begin to end: `kick` hands the GPU what has been
// recorded so far (world_gpu_kick()), `frame` numbers the lines.
void gpuwatch_trace_begin(void (*kick)(void), int frame);
void gpuwatch_trace_end(void);
bool gpuwatch_tracing(void);
// The traced frame's number, 1 to 3; 0 outside one.
int gpuwatch_trace_frame(void);

// In a traced frame: `what` names the work recorded since the last call. Its
// line goes on the card, then the kick, then up to a second's wait for the
// GPU, and the time it took or that it never finished. Nothing outside one.
void gpuwatch_trace(const char* what);

// The same in two halves, for work the caller submits itself (the present's
// C3D_FrameEnd()): the line before, the wait after.
void gpuwatch_trace_say(const char* what);
void gpuwatch_trace_wait(const char* what);
