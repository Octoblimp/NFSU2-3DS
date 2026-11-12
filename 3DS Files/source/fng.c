#include "fng.h"
#include <math.h>
#include <string.h>
#include "ffn.h"

#define FNG_MAX_RES_RECORDS 128

typedef struct {
	u32 nameOffset; // byte offset into the RsNm blob captured by this package's Res\xCC chunk
} FngResRecord;

typedef struct {
	FngResRecord records[FNG_MAX_RES_RECORDS];
	int recordCount;
	const u8* nameBlob; // Res\xCC's RsNm sibling; records' nameOffset indexes into this
	size_t nameBlobLen;
	FngScreen* screen;
	int curWidget; // the FOb's ObjD widget its Scrp chunks belong to, or -1
	// FngWidget.initHides' two halves, per widget, until the post-pass in
	// fng_parse() joins them: INIT's Sc names HIDE, and HIDE takes alpha to 0.
	u8 initChainsHide[FNG_MAX_WIDGETS];
	u8 hideClearsAlpha[FNG_MAX_WIDGETS];
} FngParseState;

#define FNG_SCRIPT_INIT 0x001744B3u // FEHashUpper("INIT")
#define FNG_SCRIPT_HIDE 0x0016A259u // FEHashUpper("HIDE")

// C port of the real engine's LoadResources__5cFEngP9FEPackagelP17FEResourceRequest
// (Speed_gekko_import.elf @ 8014d644), decompiled to find where a resource record's
// texture-pack hash (FEObject+0x24, consumed by GetTextureInfo/RenderImage) actually
// comes from -- it's NOT the RsRq record's on-disk `hash` field (that's a different,
// package-local hash; the real field4/field5 the engine copies into +0x24/+0x28 are
// always 0 on disk, populated only at load time by this exact routine). The engine:
// GetBaseName(name) [strips path AND extension] -> ToUpper -> bStringHash. Confirmed
// against real bytes: hashing "U2_Menu_Box.tga" this way gives 0xa67c3fe2, which is
// exactly GLOBALB.BUN's stored texture-pack hash for its "U2_MENU_BOX" entry.
static u32 b_string_hash(const char* s) {
	u32 h = (u32)-1;
	for (const unsigned char* p = (const unsigned char*)s; *p; p++)
		h = (u32)*p + h * 0x21u;
	return h;
}

// True for a resource record naming one of the engine's `.ffn` bitmap fonts.
// Font records are kind=2 in the RsRq table, but the kind field isn't carried
// into FngResRecord, and the name suffix is just as authoritative.
static bool name_is_font(const char* name) {
	size_t n = strlen(name);
	return n >= 4 && (strcmp(name + n - 4, ".ffn") == 0 || strcmp(name + n - 4, ".FFN") == 0);
}

// Point size out of a font record's own name: the last run of digits before
// the extension. "ConduitMdITC_TT21i.ffn" -> 21, "ConduitMdITC_TT14i.ffn" ->
// 14. Returns 0 when the name carries no digits, which the caller treats as
// "fall back to the flat menu scale" rather than as a size of zero.
static u16 font_point_size(const char* name) {
	size_t n = strlen(name);
	if (n >= 4)
		n -= 4; // skip ".ffn"
	size_t end = n;
	while (end > 0 && (name[end - 1] < '0' || name[end - 1] > '9'))
		end--;
	size_t start = end;
	while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9')
		start--;
	u16 v = 0;
	for (size_t i = start; i < end; i++)
		v = (u16)(v * 10 + (name[i] - '0'));
	return v;
}

static u32 resource_texture_hash(const char* name) {
	// GetBaseName__FPcPCc: strip directory (last '\' or '/'), then strip
	// extension (from the last '.' onward), leaving the bare base name.
	const char* base = name;
	for (const char* p = name; *p; p++) {
		if (*p == '\\' || *p == '/')
			base = p + 1;
	}
	char buf[256];
	size_t n = 0;
	for (; base[n] && n < sizeof(buf) - 1; n++)
		buf[n] = base[n];
	buf[n] = '\0';
	for (size_t i = n; i > 0; i--) {
		if (buf[i - 1] == '.') {
			buf[i - 1] = '\0';
			break;
		}
	}
	// bToUpper__FPc: ASCII a-z -> A-Z only.
	for (size_t i = 0; buf[i]; i++) {
		if ((unsigned char)(buf[i] - 'a') < 26)
			buf[i] &= 0x5f;
	}
	return b_string_hash(buf);
}

static u32 read_u32(const u8* b, size_t off) {
	u32 v;
	memcpy(&v, b + off, 4);
	return v;
}

static u16 read_u16(const u8* b, size_t off) {
	u16 v;
	memcpy(&v, b + off, 2);
	return v;
}

static float read_float(const u8* b, size_t off) {
	float v;
	memcpy(&v, b + off, 4);
	return v;
}

static bool tag2_is(const u8* payload, const char* tag) {
	return payload[0] == tag[0] && payload[1] == tag[1];
}

// Outer chunk format: [FourCC:4][Length(LE u32, payload-only):4][Payload],
// siblings found by linear scan. `handle` is called once per sibling chunk.
typedef void (*ChunkHandler)(const u8* buf, size_t pstart, size_t pend, const u8* tag, FngParseState* st);

static void walk_chunks(const u8* buf, size_t start, size_t end, FngParseState* st, ChunkHandler handle) {
	size_t off = start;
	while (off + 8 <= end) {
		const u8* tag = buf + off;
		u32 length = read_u32(buf, off + 4);
		size_t pstart = off + 8;
		size_t pend = pstart + length;
		if (pend > end)
			break;
		handle(buf, pstart, pend, tag, st);
		off = pend;
	}
}

// Parses one ObjD entry's inner Tag(2)/Len(u16)/Payload TLV stream into a new
// FngWidget appended to st->screen.
static void parse_objd(const u8* payload, size_t n, FngParseState* st) {
	st->curWidget = -1;
	if (st->screen->widgetCount >= FNG_MAX_WIDGETS)
		return;
	st->curWidget = st->screen->widgetCount;
	FngWidget* w = &st->screen->widgets[st->screen->widgetCount++];
	memset(w, 0, sizeof(*w));
	w->parentIndex = -1;
	// resourceIndex is resolved against st->records after the whole file has
	// been walked (the OP tag and the Res\xCC table can appear in either
	// order relative to each other at the outer-container level).
	bool hasResourceIndex = false;
	u16 resourceIndex = 0;

	size_t off = 0;
	while (off + 4 <= n) {
		const u8* tag = payload + off;
		u16 length = read_u16(payload, off + 2);
		off += 4;
		if (off + length > n)
			break;
		const u8* data = payload + off;
		off += length;

		if (tag2_is(tag, "Ot")) {
			if (length >= 4)
				w->type = (int)read_u32(data, 0);
		} else if (tag2_is(tag, "OP")) {
			if (length >= 16) {
				// OP's first two fields are NOT x/y screen coordinates --
				// confirmed by decompile (ReadObjectTags writes them to
				// FEObject+0xc, a GUID used for cross-package reference
				// lookup, and +0x10, shared with the separate Oh tag). Real
				// position/size live in the SA tag below. The +0xc GUID is
				// what a child object's PA tag (below) references as its
				// parent -- captured here so fng_parse() can resolve real
				// parent-child links after the whole screen is walked.
				w->guid = read_u32(data, 0);
				w->hasGuid = true;
				// +4 is the object's name hash (see fng.h): the handle screen
				// code addresses an authored widget by at runtime.
				w->objHash = read_u32(data, 4);
				w->hasObjHash = true;
				w->flags = read_u32(data, 8);
				w->hasFlags = true;
				resourceIndex = read_u16(data, 12);
				hasResourceIndex = true;
			}
		} else if (tag2_is(tag, "PA")) {
			// Parent: payload is a GUID, resolved via FindObjectByGUID at
			// load time (confirmed by decompile -- see fng.h). Resolved
			// to a widget index by fng_parse() below, once every
			// widget's own GUID (above) has been collected.
			if (length >= 4) {
				w->parentGuid = read_u32(data, 0);
				w->hasParentGuid = true;
			}
		} else if (tag2_is(tag, "SA")) {
			// See fng.h: SA's payload is copied verbatim into the object's
			// track-data buffer; Position (Vector3) is at +0x1c, Size
			// (Vector3) at +0x38. Position is (X, Y, depth) -- +0x20 is the
			// on-screen vertical axis and +0x24 is z-order, not the reverse
			// (fng.h's comment block has the shipped-data evidence).
			if (length >= 0x10) {
				// Color track (see fng.h): an FEColor, four u32 channels
				// in B,G,R,A order.
				w->colorB = (u8)read_u32(data, 0x00);
				w->colorG = (u8)read_u32(data, 0x04);
				w->colorR = (u8)read_u32(data, 0x08);
				w->colorA = (u8)read_u32(data, 0x0c);
				w->hasColor = true;
			}
			if (length >= 0x28) {
				w->posX = read_float(data, 0x1c);
				w->posY = read_float(data, 0x20);
				w->posDepth = read_float(data, 0x24);
				w->hasPos = true;
			}
			if (length >= 0x38) {
				// Rotation quaternion (see fng.h), reduced to the
				// screen-plane 2x2 of its rotation matrix.
				float qx = read_float(data, 0x28);
				float qy = read_float(data, 0x2c);
				float qz = read_float(data, 0x30);
				float qw = read_float(data, 0x34);
				w->rotAngle = 2.0f * atan2f(qz, qw);
				w->rotM[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
				w->rotM[1] = 2.0f * (qx * qy - qz * qw);
				w->rotM[2] = 2.0f * (qx * qy + qz * qw);
				w->rotM[3] = 1.0f - 2.0f * (qx * qx + qz * qz);
				w->hasRot = true;
			}
			if (length >= 0x44) {
				w->sizeW = read_float(data, 0x38);
				w->sizeH = read_float(data, 0x3c);
				w->hasSize = true;
			}
		} else if (tag2_is(tag, "SH")) {
			if (length >= 4) {
				w->stringHash = read_u32(data, 0);
				w->hasStringHash = true;
			}
		} else if (tag2_is(tag, "Sj")) {
			// Justification bits -- see fng.h's FngWidget::justify comment.
			if (length >= 4) {
				w->justify = read_u32(data, 0);
				w->hasJustify = true;
			}
		} else if (tag2_is(tag, "Sw")) {
			if (length >= 4) {
				w->maxWidth = read_u32(data, 0);
				w->hasMaxWidth = true;
			}
		} else if (tag2_is(tag, "Sl")) {
			if (length >= 4) {
				w->leading = (s32)read_u32(data, 0);
				w->hasLeading = true;
			}
		} else if (tag2_is(tag, "St")) {
			// Literal UTF-16LE string embedded directly in this widget's
			// ObjD entry -- real on-disk data, not resolved via SH/
			// English.bin. See fng.h's FngWidget.literalText comment (only
			// confirmed use case so far: LS_LangSelect.fng's "French"
			// option, whose SH hash has no English.bin entry). Plain
			// UTF-16LE->ASCII decode, truncated at the first NUL code unit
			// or the literalText buffer size, whichever comes first.
			size_t chars = length / 2;
			size_t maxChars = sizeof(w->literalText) - 1;
			size_t i;
			for (i = 0; i < chars && i < maxChars; i++) {
				u16 cu = read_u16(data, i * 2);
				if (cu == 0)
					break;
				w->literalText[i] = (cu < 0x80) ? (char)cu : '?';
			}
			w->literalText[i] = '\0';
			if (i > 0)
				w->hasLiteralText = true;
		}
	}

	if (hasResourceIndex) {
		if (resourceIndex != 0xFFFF && resourceIndex < (u16)st->recordCount) {
			u32 nameOff = st->records[resourceIndex].nameOffset;
			if (st->nameBlob && nameOff < st->nameBlobLen) {
				const char* name = (const char*)st->nameBlob + nameOff;
				if (name_is_font(name)) {
					// See fng.h: a font record is a point-size selector, not a
					// texture. Deliberately does NOT set hasResourceRef.
					w->hasFontRef = true;
					w->fontPointSize = font_point_size(name);
					w->fontNameHash = ffn_name_hash(name);
				} else {
					w->resourceHash = resource_texture_hash(name);
					w->hasResourceRef = true;
				}
			}
		}
		// else: out of range, or the 0xFFFF cross-package sentinel -- leave
		// hasResourceRef false (see fng.h's hierarchy/cross-package note).
	}
}

// Parses the Res\xCC chunk's RsRq record table (24-byte records:
// {hash, nameOffset, kind, field3, field4, field5}, see
// docs/formats/frontend-menu-re.md / tools/fng_parse.py's parse_res()).
static void parse_res_child(const u8* buf, size_t pstart, size_t pend, const u8* tag, FngParseState* st) {
	static const u8 TAG_RsNm[4] = {'R', 's', 'N', 'm'};
	static const u8 TAG_RsRq[4] = {'R', 's', 'R', 'q'};
	if (memcmp(tag, TAG_RsNm, 4) == 0) {
		st->nameBlob = buf + pstart;
		st->nameBlobLen = pend - pstart;
		return;
	}
	if (memcmp(tag, TAG_RsRq, 4) != 0)
		return;
	size_t n = pend - pstart;
	if (n < 4)
		return;
	u32 count = read_u32(buf, pstart);
	for (u32 i = 0; i < count && st->recordCount < FNG_MAX_RES_RECORDS; i++) {
		size_t roff = pstart + 4 + (size_t)i * 24;
		if (roff + 24 > pend)
			break;
		st->records[st->recordCount].nameOffset = read_u32(buf, roff + 4);
		st->recordCount++;
	}
}

// One Scrp chunk: the same [Tag:2][Length:2][Payload] records as ObjD, read
// the way FEPackageReader::ReadScriptTags (0x8013c44c) reads them, for the one
// thing this port needs out of them -- which members the tracks drive. "Sh"
// opens a script and zeroes the running offset; each "FI" opens a track whose
// LongOffset defaults to that running offset, which then advances by the
// track's ParamSize / 4; a "To" after it overrides the offset with its first
// byte.
//
// Key data ("Kd") is kept for one kind of track: the Color track of an INIT
// script that loops, the loading screens' dots. ReadScriptTags makes INIT
// (0x1744B3) the object's current script, so it plays from the moment the
// package is up, and FEPackage::UpdateObject (0x8013958C) wraps the time of a
// script whose Sh flags & 3 are 1 by its length. Sh is {hash, length ms,
// flags, track count}; FI is {param type, param size, interp type, interp
// action, u32 track length}. A Kd row is the u32 time and the param's values:
// the first row, stamped 0xFFFFFFFF, is the base, and every later row is a
// key holding deltas from it. Only linear tracks (interp 1 or 3,
// FEKeyInterp 0x801401EC) with two keys or more are kept; the rest of the Kd
// data is skipped.
static void parse_scrp(const u8* payload, size_t n, FngParseState* st) {
	if (st->curWidget < 0)
		return;
	FngWidget* w = &st->screen->widgets[st->curWidget];
	int running = 0, track = -1;
	u32 shHash = 0, shLen = 0, shFlags = 0;
	u8 fiType = 0, fiSize = 0, fiInterp = 0, fiAction = 0;
	u32 fiLen = 0;
	size_t off = 0;
	while (off + 4 <= n) {
		const u8* tag = payload + off;
		u16 length = read_u16(payload, off + 2);
		off += 4;
		if (off + length > n)
			break;
		const u8* data = payload + off;
		off += length;
		if (tag2_is(tag, "Sh") || tag2_is(tag, "FI")) {
			if (track >= 0 && track < 32)
				w->scriptTrackMask |= 1u << track;
			track = -1;
			if (tag2_is(tag, "Sh")) {
				running = 0;
				if (length >= 12) {
					shHash = read_u32(data, 0);
					shLen = read_u32(data, 4);
					shFlags = read_u32(data, 8);
				}
			} else {
				track = running;
				if (length >= 2)
					running += data[1] >> 2;
				if (length >= 8) {
					fiType = data[0];
					fiSize = data[1];
					fiInterp = data[2];
					fiAction = data[3];
					fiLen = read_u32(data, 4);
				}
			}
		} else if (tag2_is(tag, "To") && track >= 0 && length >= 1) {
			track = data[0];
		} else if (tag2_is(tag, "Sc") && shHash == FNG_SCRIPT_INIT && length >= 4 &&
				read_u32(data, 0) == FNG_SCRIPT_HIDE) {
			st->initChainsHide[st->curWidget] = 1;
		} else if (tag2_is(tag, "Kd") && shHash == FNG_SCRIPT_HIDE && track == 0 &&
				fiType == 6 && fiSize == 16 && length >= 20) {
			// FEColor's base row {time, B, G, R, A}: HIDE's alpha.
			if (read_u32(data, 16) == 0)
				st->hideClearsAlpha[st->curWidget] = 1;
		} else if (tag2_is(tag, "Kd") && track == 0 && fiType == 6 && fiSize == 16 &&
				shHash == 0x1744B3u && (shFlags & 3) == 1 && shLen > 0 && shLen <= 0xFFFF &&
				fiLen <= 0xFFFF && (fiInterp == 1 || fiInterp == 3) && w->loopColorKeys == 0) {
			// FEColor's track: rows of {time, B, G, R, A}.
			FngScreen* s = st->screen;
			const int keys = length / 20 - 1;
			if (keys >= 2 && keys <= 255 && s->colorKeyCount + keys <= FNG_MAX_COLOR_KEYS) {
				for (int c = 0; c < 4; c++) {
					const u32 v = read_u32(data, 4 + 4 * c);
					w->loopColorBase[c] = (u8)(v > 255 ? 255 : v);
				}
				w->loopColorFirst = (u8)s->colorKeyCount;
				w->loopColorKeys = (u8)keys;
				w->loopColorAction = fiAction & 0x7f;
				w->loopColorMs = (u16)shLen;
				w->loopColorTrackMs = (u16)fiLen;
				for (int k = 0; k < keys; k++) {
					FngColorKey* key = &s->colorKeys[s->colorKeyCount++];
					const size_t row = 20 * (size_t)(k + 1);
					key->t = (s32)read_u32(data, row);
					for (int c = 0; c < 4; c++)
						key->d[c] = (s16)(s32)read_u32(data, row + 4 + 4 * c);
				}
			}
		}
	}
	if (track >= 0 && track < 32)
		w->scriptTrackMask |= 1u << track;
}

static void walk(const u8* buf, size_t start, size_t end, FngParseState* st);

static void walk_child(const u8* buf, size_t pstart, size_t pend, const u8* tag, FngParseState* st) {
	static const u8 TAG_FEn[4] = {'F', 'E', 'n', 0xE7};
	static const u8 TAG_ObjCC[4] = {'O', 'b', 'j', 0xCC};
	static const u8 TAG_FObEA[4] = {'F', 'O', 'b', 0xEA};
	static const u8 TAG_ResCC[4] = {'R', 'e', 's', 0xCC};
	static const u8 TAG_ObjD[4] = {'O', 'b', 'j', 'D'};
	static const u8 TAG_Scrp[4] = {'S', 'c', 'r', 'p'};

	if (memcmp(tag, TAG_ResCC, 4) == 0) {
		walk_chunks(buf, pstart, pend, st, parse_res_child);
	} else if (memcmp(tag, TAG_FEn, 4) == 0 || memcmp(tag, TAG_ObjCC, 4) == 0 || memcmp(tag, TAG_FObEA, 4) == 0) {
		walk(buf, pstart, pend, st);
	} else if (memcmp(tag, TAG_ObjD, 4) == 0) {
		parse_objd(buf + pstart, pend - pstart, st);
	} else if (memcmp(tag, TAG_Scrp, 4) == 0) {
		parse_scrp(buf + pstart, pend - pstart, st);
	} else if (memcmp(tag, "MsgR", 4) == 0 && st->curWidget >= 0) {
		// Only whether the object has one (FngWidget.hasMsgResp); the
		// responses themselves are not read.
		st->screen->widgets[st->curWidget].hasMsgResp = true;
	}
	// PkHd/TypS/PkgR/Targ/LibL/LibR: not needed for this render
	// pass (see fng.h) -- skipped.
}

static void walk(const u8* buf, size_t start, size_t end, FngParseState* st) {
	walk_chunks(buf, start, end, st, walk_child);
}

// A widget's PA tag can reference a GUID belonging to a widget declared
// later in the file, so parent links can only be resolved once every
// widget's own OP-tag GUID has been collected -- a post-pass over the fully
// parsed flat widget list.
static void resolve_parents(FngScreen* screen) {
	for (int i = 0; i < screen->widgetCount; i++) {
		FngWidget* w = &screen->widgets[i];
		if (!w->hasParentGuid)
			continue;
		for (int j = 0; j < screen->widgetCount; j++) {
			if (j == i)
				continue;
			if (screen->widgets[j].hasGuid && screen->widgets[j].guid == w->parentGuid) {
				w->parentIndex = j;
				break;
			}
		}
	}
}

bool fng_parse(const u8* buf, size_t len, FngScreen* out) {
	memset(out, 0, sizeof(*out));
	if (len < 4 || memcmp(buf, "FEn\xE7", 4) != 0)
		return false;

	FngParseState st = {0};
	st.screen = out;
	st.curWidget = -1;
	walk(buf, 0, len, &st);
	resolve_parents(out);
	for (int i = 0; i < out->widgetCount; i++)
		out->widgets[i].initHides = st.initChainsHide[i] && st.hideClearsAlpha[i];
	return true;
}

// FEInterpLinear (Speed.elf 0x80140318) over a looping INIT Color track (see
// parse_scrp). The script's time wraps at its length (FEPackage::UpdateObject
// 0x8013958C). GetDeltaKeyAt finds the first key at or after it, else the last.
// Between two keys the deltas are lerped. Past the last key or before the first,
// the interp action decides:
//   - 1 runs on from the last key to the first across the track's length;
//   - 2 runs the time back from the track's end;
//   - 0 holds the end key.
// FELerpColor adds the result to the base row.
bool fng_loop_color(const FngScreen* s, const FngWidget* w, u32 ms, u8 out[4]) {
	if (!w->loopColorKeys || !w->loopColorMs)
		return false;
	const FngColorKey* k = &s->colorKeys[w->loopColorFirst];
	const int n = w->loopColorKeys;
	const s32 len = w->loopColorTrackMs;
	s32 t = (s32)(ms % w->loopColorMs);
	if (w->loopColorAction == 2 && t > len)
		t = 2 * len - t;
	int j = 0;
	while (j < n - 1 && k[j].t < t)
		j++;
	const FngColorKey* a = &k[j];
	const FngColorKey* b = &k[j];
	float f = 0.0f;
	if (k[j].t < t) {
		if (w->loopColorAction == 1) {
			const float span = (float)(len - k[j].t + k[0].t);
			b = &k[0];
			f = span > 0.0f ? (float)(t - k[j].t) / span : 0.0f;
		}
	} else if (k[j].t > t) {
		if (j > 0) {
			a = &k[j - 1];
			f = (float)(t - a->t) / (float)(k[j].t - a->t);
		} else if (w->loopColorAction == 1) {
			a = &k[n - 1];
			const float span = (float)(len - a->t + k[0].t);
			f = span > 0.0f ? (float)(t + len - a->t) / span : 1.0f;
		}
	}
	for (int c = 0; c < 4; c++) {
		const float v = (float)w->loopColorBase[c] + (float)a->d[c] + (float)(b->d[c] - a->d[c]) * f;
		out[c] = (u8)(v < 0.0f ? 0 : v > 255.0f ? 255 : (int)v);
	}
	return true;
}
