#pragma once
#include <3ds.h>
#include <stdbool.h>

// The top-level chunk list of a .bun, walked once per file per session
// (TASKS.md #270).
//
// GLOBALB.BUN is 8 MB in 298 top-level chunks, and the career, shop, track,
// car and difficulty tables each walked it from offset 0 with one 8-byte
// seek+read per chunk to find their own -- the event container is chunk 208,
// 7.7 MB in, and that walk was most of a 685 ms "event db" on every world
// entry. The index reads the headers in 16 KB windows, seeks past a chunk too
// big for its window, and keeps the answer for the next caller.
//
// The walk stops where the loaders' own walks stopped: at a zero header, or
// at a size that runs past the end of the file.

// The first top-level chunk of `type` in the file at `path`: the file offset
// of its 8-byte header, and its payload size. False when the file cannot be
// read or holds no such chunk before the walk stops.
bool bunidx_find(const char* path, u32 type, long* hdrOff, u32* size);
