#ifndef NFSU2_DBGLOG_H
#define NFSU2_DBGLOG_H

#include <stdarg.h>
#include <stdbool.h>

// sdmc:/nfsu2_debug.txt, the port's one debug log (TASKS.md #233).
//
// Every writer in the port comes through here, from any thread. A call copies
// the line into a RAM ring and returns; one background thread owns the file
// and does every SD access. The game never waits on the card to log a line.
//
// The file is also never grown while the game runs. It is kept at a whole
// number of megabytes from one run to the next and overwritten from the
// start, so the run reuses the clusters the last run already had. What is
// past the end of this run's text is blank lines (spaces), not the old run.
// Why: on the user's card, every 32 KB cluster the FAT had to find for a
// growing file held the FS for ~2 s, and with the old open-append-close on
// the main thread that was a two-second freeze each time the log crossed one.

// Opens the file and starts the writer. Call first thing in main(); a line
// logged before it is written synchronously, as is everything if the thread
// will not start.
void dbglog_init(void);

// `prefix` (may be NULL) + `msg` + '\n'.
void dbglog_line(const char* prefix, const char* msg);

// `prefix` (may be NULL) + the formatted text, with no newline added -- for
// writers whose format strings carry their own (the vp6 shim's).
void dbglog_vprintf(const char* prefix, const char* fmt, va_list ap);

// Raises the writer above the main thread, or puts it back: gpuwatch.c's,
// for a stall report that has to reach the card past a main thread that
// never yields.
void dbglog_urgent(bool on);

// Blocks until everything logged so far is on the card, or ~3 s pass.
void dbglog_flush(void);

// Flushes, stops the writer and closes the file.
void dbglog_exit(void);

#endif
