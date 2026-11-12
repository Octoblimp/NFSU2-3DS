// LZ4 block decoding, shared by the world archive (world.c, WORLD.NFSZ) and
// the topology (topology.c, a compressed WORLD.NFST; #270).

#include "lz4dec.h"

#include <string.h>

// LZ4's block format, with every length checked against both buffers: a bad
// read off the card has to fail the load, not write past the end of a GPU
// pool. True only for exactly `cap` bytes out.
bool lz4_decode(const u8* ip, u32 n, u8* op, u32 cap) {
	const u8* const iend = ip + n;
	u8* const base = op;
	u8* const oend = op + cap;
	while (ip < iend) {
		const u32 token = *ip++;
		u32 lit = token >> 4;
		if (lit == 15) {
			u32 b;
			do {
				if (ip >= iend)
					return false;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if ((u32)(iend - ip) < lit || (u32)(oend - op) < lit)
			return false;
		// Most runs are a few bytes, where a memcpy call per run costs more
		// than the copy, so they go eight bytes at a time wherever both
		// buffers have the slack to run past the end. Nothing reads the
		// overrun: it is past `op`, and the next sequence writes over it.
		if ((u32)(iend - ip) >= lit + 8 && (u32)(oend - op) >= lit + 8) {
			u8* const e = op + lit;
			const u8* const ie = ip + lit;
			do {
				memcpy(op, ip, 8);
				op += 8;
				ip += 8;
			} while (op < e);
			op = e;
			ip = ie;
		} else {
			memcpy(op, ip, lit);
			op += lit;
			ip += lit;
		}
		if (ip == iend)
			break;   // the last sequence is literals only
		if (iend - ip < 2)
			return false;
		const u32 off = (u32)ip[0] | ((u32)ip[1] << 8);
		ip += 2;
		if (!off || off > (u32)(op - base))
			return false;
		u32 len = (token & 15) + 4;
		if ((token & 15) == 15) {
			u32 b;
			do {
				if (ip >= iend)
					return false;
				b = *ip++;
				len += b;
			} while (b == 255);
		}
		if ((u32)(oend - op) < len)
			return false;
		const u8* m = op - off;
		if (off >= 8 && (u32)(oend - op) >= len + 8) {
			// Each eight bytes read end at or before the eight being
			// written, so this is right even where the match overlaps.
			u8* const e = op + len;
			do {
				memcpy(op, m, 8);
				op += 8;
				m += 8;
			} while (op < e);
			op = e;
		} else if (off >= len) {
			memcpy(op, m, len);
			op += len;
		} else {
			// An overlapping match repeats the last `off` bytes; byte by byte
			// is what makes it read what it has just written.
			while (len--)
				*op++ = *m++;
		}
	}
	return op == oend;
}
