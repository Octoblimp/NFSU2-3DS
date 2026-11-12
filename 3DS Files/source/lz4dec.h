#pragma once

#include <3ds.h>

// One LZ4 block (no frame), `n` bytes at `ip`, into exactly `cap` bytes at
// `op`. Every length is checked against both buffers; false on anything
// short, long or malformed.
bool lz4_decode(const u8* ip, u32 n, u8* op, u32 cap);
