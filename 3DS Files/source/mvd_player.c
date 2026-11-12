/*
 * mvd_player.c -- see mvd_player.h.
 *
 * The decode sequence is the one known to work on real hardware, not one
 * derived from documentation alone. Two sources agree on it:
 *
 *   - 3dbrew's MVD Services page, which records what SKATER (the New 3DS
 *     Internet Browser) does: CalculateWorkBufSize, Initialize, the 0x05 /
 *     0x18 / 0x1B setup commands (all inside libctru's mvdstdInit()), then
 *     SPS and PPS in two separate ProcessNALUnit calls, then the stream; and
 *     on a BUSY from frame rendering, SetConfig + render again.
 *   - Core-2-Extreme's "Video player for 3DS", which ships MVD decoding to
 *     New 3DS owners: 3-byte 00 00 01 start codes, SetConfig before every
 *     feed, the first access unit fed TWICE, and frame arrival detected by
 *     sentinel bytes in the output buffer rather than trusted from the
 *     status code.
 *
 * Azahar cannot check any of this: it registers mvd:STD but has no decoder
 * behind it. So everything the hardware says is logged ("[mvd]" lines in
 * sdmc:/nfsu2_debug.txt): the refusal reason when it falls back, the status of
 * the first few feeds, and per-clip timing at close.
 */
#include "mvd_player.h"
#include "dbglog.h"
#include "framepace.h"
#include <stdlib.h>
#include <string.h>

/* libctru's own; heapcfg.c sets it. MVD only works on the 0x30000000 linear
 * mapping, which the kernel gives a process whose ExHeader says kernel
 * 2.44+ -- see app.rsf's ReleaseKernelMinor. */
extern u32 __ctru_linear_heap;

static void mlog(const char *msg) {
    dbglog_line("[mvd] ", msg);
}

typedef struct __attribute__((packed)) {
    char magic[4];
    u16  version, headerSize;
    u16  codedW, codedH, picX, picY, picW, picH;
    u32  fpsNum, fpsDen, frameCount, indexOffset, maxAu;
    u8   profileIdc, levelIdc, constraint, reserved;
    u32  spsOffset, spsSize, ppsOffset, ppsSize, dataOffset;
} MvdFileHeader;
_Static_assert(sizeof(MvdFileHeader) == 64, "MvdFileHeader must match tools/fmv_mvd_pack.py");

/* Linear row 0 (the picture's top) lands in tiled row 0, the same layout
 * vp6_display.c writes by hand, so the subtexture below uses the same
 * top=1.0 convention. RGB565 in and out: the decoder's MVD_OUTPUT_BGR565 is
 * the format libctru's own MVD example writes into a GSP_RGB565_OES
 * framebuffer, i.e. red in the top five bits, which is GPU_RGB565. */
#define MVD_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define SENTINEL        0x11
#define LOG_FIRST_FEEDS 6
#define RENDER_SPIN_MAX 4000

static bool is_pow2(u32 v) { return v && !(v & (v - 1)); }

/* svcGetSystemInfo type 0x20000 ("Citra information"), parameter 0
 * (EMULATOR_ID): Citra answers 1, Azahar 2. A real kernel rejects the type. */
static bool on_emulator(void) {
    s64 v = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v != 0;
}

/* Is the decode block there at all? Must be asked before mvdstdInit():
 * libctru's srvGetServiceHandle() BLOCKS until a service registers, so on a
 * console where the MVD sysmodule was never launched, mvdstdInit() would hang
 * the boot forever instead of failing.
 *
 * The emulator check has to come first. Azahar has no H.264 decoder behind
 * its mvd:STD, and its srv does not implement IsServiceRegistered at all: the
 * request is logged as unimplemented and never answered, so the caller hangs
 * right there (seen on the first test run of this code). */
static bool hw_present(char *why, size_t n) {
    bool n3ds = false;
    if (R_FAILED(APT_CheckNew3DS(&n3ds)) || !n3ds) {
        snprintf(why, n, "not a New 3DS");
        return false;
    }
    if (on_emulator()) {
        snprintf(why, n, "running under an emulator (no H.264 decoder behind its mvd:STD)");
        return false;
    }
    bool reg = false;
    Result rc = srvIsServiceRegistered(&reg, "mvd:STD");
    if (R_FAILED(rc) || !reg) {
        snprintf(why, n, "mvd:STD not registered (rc=0x%08lx registered=%d) -- MVD sysmodule not running",
                 (unsigned long)rc, reg);
        return false;
    }
    if (__ctru_linear_heap < 0x30000000) {
        snprintf(why, n, "linear heap at 0x%08lx; MVD needs the 0x30000000 mapping (ExHeader kernel >= 2.44)",
                 (unsigned long)__ctru_linear_heap);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- sentinels */

static void sentinel_offsets(const MvdPlayer *p, u32 off[4]) {
    u32 row = (u32)p->codedW * 2, total = row * p->codedH;
    off[0] = 0;
    off[1] = row - 1;
    off[2] = total - row;
    off[3] = total - 1;
}

/* Written then cleaned out to RAM: a dirty line evicted after the decoder's
 * DMA would otherwise put the sentinel back over a decoded pixel. */
static void arm_sentinels(MvdPlayer *p, u8 *out) {
    u32 off[4];
    sentinel_offsets(p, off);
    for (int i = 0; i < 4; i++)
        out[off[i]] = SENTINEL;
    for (int i = 0; i < 4; i++)
        GSPGPU_FlushDataCache(out + (off[i] & ~31u), 32);
}

/* The decoder writes RAM behind the CPU's back, so drop the cached copies
 * before looking. */
static bool sentinels_hit(MvdPlayer *p, u8 *out) {
    u32 off[4];
    sentinel_offsets(p, off);
    for (int i = 0; i < 4; i++)
        GSPGPU_InvalidateDataCache(out + (off[i] & ~31u), 32);
    for (int i = 0; i < 4; i++)
        if (out[off[i]] != SENTINEL)
            return true;
    return false;
}

/* --------------------------------------------------------------- reading */

static long g_filePos;

static bool read_au(MvdPlayer *p, u32 i, u32 *sizeOut) {
    u32 off = p->index[i * 2], size = p->index[i * 2 + 1];
    if (size == 0 || size > p->maxAu)
        return false;
    if ((long)off != g_filePos && fseek(p->f, (long)off, SEEK_SET) != 0)
        return false;
    if (fread(p->packet, 1, size, p->f) != size)
        return false;
    g_filePos = (long)off + (long)size;
    GSPGPU_FlushDataCache(p->packet, size);
    *sizeOut = size;
    return true;
}

static bool feed_param_set(MvdPlayer *p, u32 off, u32 size, const char *what) {
    char b[160];
    if (size < 4 || size > p->maxAu || fseek(p->f, (long)off, SEEK_SET) != 0 ||
        fread(p->packet, 1, size, p->f) != size) {
        snprintf(b, sizeof(b), "%s: unreadable (off=%lu size=%lu)", what, (unsigned long)off, (unsigned long)size);
        mlog(b);
        return false;
    }
    g_filePos = -1;
    GSPGPU_FlushDataCache(p->packet, size);
    Result ret = mvdstdProcessVideoFrame(p->packet, size, 0, NULL);
    snprintf(b, sizeof(b), "%s: %lu bytes -> 0x%08lx", what, (unsigned long)size, (unsigned long)ret);
    mlog(b);
    return MVD_CHECKNALUPROC_SUCCESS(ret);
}

/* ---------------------------------------------------------------- decode */

/* One frame into output slot 's'. 1 = got one, 0 = end of clip, -1 = decoder
 * error.
 *
 * 'skip' is Video player for 3DS's drain flag: once a feed has produced its
 * frame, the next call first asks the renderer for anything still queued
 * before feeding more data. A Baseline stream never queues (no B-frames), so
 * in practice the drain comes back empty, clears the flag, and the loop feeds
 * the next access unit -- but it is the same sequence that is proven on
 * hardware, and it stays correct for a re-encode with reordering.
 *
 * Each call returns once the frame is in the slot (the sentinels moved) or
 * the decoder failed, and close joins the thread before mvdstdExit(), so no
 * ProcessNALUnit or render request is still open when the work buffer is
 * freed. */
static int decode_one(MvdPlayer *p, int s) {
    char b[160];
    u8 *out = p->out[s];
    MVDSTD_Config *cfg = &p->cfg[s];
    for (int attempt = 0; attempt < 8; attempt++) {
        u64 t0 = svcGetSystemTick();
        bool got = false;
        Result ret = MVD_STATUS_OK;

        arm_sentinels(p, out);
        MVDSTD_SetConfig(cfg);

        if (!p->skip) {
            if (p->nextAu >= p->frameCount)
                return 0;
            u32 size = 0;
            if (!read_au(p, p->nextAu, &size)) {
                snprintf(b, sizeof(b), "access unit %lu unreadable", (unsigned long)p->nextAu);
                mlog(b);
                return -1;
            }
            ret = mvdstdProcessVideoFrame(p->packet, size, 0, NULL);
            if (p->first) {
                Result again = mvdstdProcessVideoFrame(p->packet, size, 0, NULL);
                snprintf(b, sizeof(b), "first access unit fed twice: 0x%08lx then 0x%08lx",
                         (unsigned long)ret, (unsigned long)again);
                mlog(b);
                ret = again;
                p->first = false;
            }
            p->nextAu++;
            p->statFed++;
            if (sentinels_hit(p, out)) {
                got = true;
                p->skip = true;
            }
            if (p->statFed <= LOG_FIRST_FEEDS) {
                snprintf(b, sizeof(b), "feed %lu (%lu bytes): status 0x%08lx, frame %s",
                         (unsigned long)(p->nextAu - 1), (unsigned long)size, (unsigned long)ret,
                         got ? "written" : "not yet");
                mlog(b);
            }
            if (!MVD_CHECKNALUPROC_SUCCESS(ret)) {
                snprintf(b, sizeof(b), "ProcessNALUnit failed on access unit %lu: 0x%08lx",
                         (unsigned long)(p->nextAu - 1), (unsigned long)ret);
                mlog(b);
                return -1;
            }
        }

        if (!got) {
            for (int spin = 0; spin < RENDER_SPIN_MAX; spin++) {
                ret = mvdstdRenderVideoFrame(cfg, false);
                p->statRenderSpins++;
                if (sentinels_hit(p, out)) {
                    got = true;
                    ret = MVD_STATUS_OK;
                    break;
                }
                if (ret != MVD_STATUS_BUSY)
                    break;
            }
            if (ret != MVD_STATUS_OK) {
                snprintf(b, sizeof(b), "frame rendering failed after access unit %lu: 0x%08lx",
                         (unsigned long)p->nextAu, (unsigned long)ret);
                mlog(b);
                return -1;
            }
            if (got)
                p->statDrained++;
        }

        u64 dt = svcGetSystemTick() - t0;
        p->statDecodeTicks += dt;
        if (dt > p->statMaxDecodeTicks)
            p->statMaxDecodeTicks = dt;

        if (got)
            return 1;
        if (p->skip) {
            p->skip = false;   /* nothing was queued: feed the next unit */
            continue;
        }
        p->statNoOutput++;     /* a unit that produced no picture */
    }
    snprintf(b, sizeof(b), "no picture after 8 attempts at access unit %lu", (unsigned long)p->nextAu);
    mlog(b);
    return -1;
}

/* HwVideoSelfTest: a test card through the exact display path the decoder
 * output takes -- the same slots, thread and transfer. Picture-relative
 * corners are red (top-left), green (top-right), blue (bottom-left), white
 * (bottom-right); a yellow bar sweeps left to right; the background ramps red
 * with x and green with y. Anything else on screen -- a mirrored card,
 * swapped red/blue, a band of garbage -- is a fault in the
 * transfer/texture/draw path, independent of the MVD. */
static int synth_frame(MvdPlayer *p, int s) {
    if (p->nextAu >= p->frameCount)
        return 0;
    u64 t0 = svcGetSystemTick();
    u16 *px = (u16 *)p->out[s];
    const int m = 40;
    int bar = (int)((p->nextAu * 4) % p->picW);
    for (int y = 0; y < p->codedH; y++) {
        for (int x = 0; x < p->codedW; x++) {
            u16 c = 0;
            int lx = x - p->picX, ly = y - p->picY;
            if (lx >= 0 && ly >= 0 && lx < p->picW && ly < p->picH) {
                c = (u16)(((lx * 31 / p->picW) << 11) | ((ly * 63 / p->picH) << 5) | 8);
                bool right = lx >= p->picW - m, bottom = ly >= p->picH - m;
                if (lx < m && ly < m)            c = 0xF800;
                else if (right && ly < m)        c = 0x07E0;
                else if (lx < m && bottom)       c = 0x001F;
                else if (right && bottom)        c = 0xFFFF;
                if (lx >= bar && lx < bar + 4)   c = 0xFFE0;
            }
            px[y * p->codedW + x] = c;
        }
    }
    GSPGPU_FlushDataCache(p->out[s], (u32)p->codedW * p->codedH * 2);
    p->nextAu++;
    p->statFed++;
    u64 dt = svcGetSystemTick() - t0;
    p->statDecodeTicks += dt;
    if (dt > p->statMaxDecodeTicks)
        p->statMaxDecodeTicks = dt;
    return 1;
}

/* ----------------------------------------------------------------- slots */

enum { SLOT_FREE, SLOT_DECODING, SLOT_READY, SLOT_SHOWN };

/* One frame into a free slot. false = no free slot, or the clip is over. */
static bool produce_one(MvdPlayer *p) {
    int s = -1;
    LightLock_Lock(&p->lock);
    if (!p->ended)
        for (int i = 0; i < MVD_SLOTS; i++)
            if (p->slotState[i] == SLOT_FREE) {
                s = i;
                p->slotState[i] = SLOT_DECODING;
                break;
            }
    LightLock_Unlock(&p->lock);
    if (s < 0)
        return false;

    int r = p->selfTest ? synth_frame(p, s) : decode_one(p, s);

    LightLock_Lock(&p->lock);
    if (r == 1) {
        p->slotState[s] = SLOT_READY;
        p->slotFrame[s] = p->produced++;
    } else {
        p->slotState[s] = SLOT_FREE;
        p->ended = true;
        p->failed = r < 0;
    }
    LightLock_Unlock(&p->lock);
    return r == 1;
}

/* The decode thread: a frame whenever a slot is free, until the clip ends or
 * close asks it to stop. It blocks in the MVD's IPC most of the time, so the
 * main thread draws while the block decodes -- before #440 each frame's
 * 10-50 ms decode came out of the frame it was shown in. */
static void decode_thread(void *arg) {
    MvdPlayer *p = (MvdPlayer *)arg;
    for (;;) {
        LightSemaphore_Acquire(&p->freeSlots, 1);
        if (p->stop || !produce_one(p))
            break;
    }
}

/* Main thread, under the lock: give slot 's' back to the decoder. */
static void slot_free_locked(MvdPlayer *p, int s) {
    p->slotState[s] = SLOT_FREE;
    if (p->thread)
        LightSemaphore_Release(&p->freeSlots, 1);
}

/* Queued, not done: inside a frame citro3d puts the transfer after the
 * commands so far and runs it at C3D_FrameEnd(). So the source slot stays
 * SHOWN until the next advance (after the next C3D_FrameBegin() has waited
 * for it), and close hands every buffer to framepace_free_after_gpu(). */
static void upload(MvdPlayer *p, int s) {
    int t = p->tex_ready ? 1 - p->dispTex : 0;
    // #481: the copy submits the draws so far itself, unflushed; on the 3DS
    // a running queue then reads the list before the cache has written it.
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    C3D_SyncDisplayTransfer((u32 *)p->out[s], GX_BUFFER_DIM(p->codedW, p->codedH),
                            (u32 *)p->tex[t].data, GX_BUFFER_DIM(p->codedW, p->codedH),
                            MVD_TRANSFER_FLAGS);
    p->dispTex = t;
    p->tex_ready = true;
}

/* ------------------------------------------------------------------- API */

static void stop_thread(MvdPlayer *p) {
    if (!p->thread)
        return;
    p->stop = true;
    LightSemaphore_Release(&p->freeSlots, 1);
    threadJoin(p->thread, U64_MAX);
    threadFree(p->thread);
    p->thread = NULL;
}

static void release(MvdPlayer *p) {
    stop_thread(p);
    if (p->mvdOpen)
        mvdstdExit();
    /* A transfer queued this frame may still read a slot and write a
     * texture: the memory goes back once the GPU is past it (#439). */
    for (int i = 0; i < 2; i++) {
        framepace_free_after_gpu(p->tex[i].data);
        p->tex[i].data = NULL;
    }
    for (int i = 0; i < MVD_SLOTS; i++)
        framepace_free_after_gpu(p->out[i]);
    framepace_free_after_gpu(p->packet);
    free(p->index);
    if (p->f)
        fclose(p->f);
    memset(p, 0, sizeof(*p));
}

static bool start_thread(MvdPlayer *p) {
    /* Core 2, as the VP6 decoder before it and the world loader after: the
     * audio stream thread there is above it. One step below the main thread,
     * for when core 2 is refused and it shares the game's core. */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio += 1;
    if (prio > 0x3F)
        prio = 0x3F;
    int freeNow = 0;
    for (int i = 0; i < MVD_SLOTS; i++)
        freeNow += p->slotState[i] == SLOT_FREE;
    LightSemaphore_Init(&p->freeSlots, (s16)freeNow, MVD_SLOTS + 1);
    p->thread = threadCreate(decode_thread, p, 16 * 1024, prio, 2, false);
    if (!p->thread)
        p->thread = threadCreate(decode_thread, p, 16 * 1024, prio, -1, false);
    return p->thread != NULL;
}

bool mvd_player_open(MvdPlayer *p, const char *path, bool selfTest) {
    char b[300];
    memset(p, 0, sizeof(*p));
    p->selfTest = selfTest;
    LightLock_Init(&p->lock);

    if (!selfTest) {
        char why[160];
        if (!hw_present(why, sizeof(why))) {
            snprintf(b, sizeof(b), "hardware decode unavailable: %s", why);
            mlog(b);
            return false;
        }
    }

    u64 tOpen = svcGetSystemTick();
    p->f = fopen(path, "rb");
    if (!p->f) {
        snprintf(b, sizeof(b), "%s: not found", path);
        mlog(b);
        return false;
    }
    setvbuf(p->f, NULL, _IOFBF, 64 * 1024);
    g_filePos = -1;

    MvdFileHeader h;
    if (fread(&h, 1, sizeof(h), p->f) != sizeof(h) || memcmp(h.magic, "MVD1", 4) != 0 ||
        h.version != 1 || h.headerSize != sizeof(h) || !is_pow2(h.codedW) || !is_pow2(h.codedH) ||
        h.codedW < 64 || h.codedH < 64 || h.codedW > 1024 || h.codedH > 1024 ||
        h.picW == 0 || h.picH == 0 || h.picX + h.picW > h.codedW || h.picY + h.picH > h.codedH ||
        h.frameCount == 0 || h.maxAu == 0 || h.maxAu > (4u << 20) || h.fpsNum == 0 || h.fpsDen == 0) {
        snprintf(b, sizeof(b), "%s: bad header", path);
        mlog(b);
        release(p);
        return false;
    }
    p->codedW = h.codedW; p->codedH = h.codedH;
    p->picX = h.picX; p->picY = h.picY; p->picW = h.picW; p->picH = h.picH;
    p->frameCount = h.frameCount;
    p->nb_frames = h.frameCount;
    p->fps = (double)h.fpsNum / (double)h.fpsDen;
    p->levelIdc = h.levelIdc;
    p->maxAu = h.maxAu;
    if (h.spsSize > p->maxAu) p->maxAu = h.spsSize;
    if (h.ppsSize > p->maxAu) p->maxAu = h.ppsSize;

    p->index = (u32 *)malloc((size_t)h.frameCount * 8);
    if (!p->index || fseek(p->f, (long)h.indexOffset, SEEK_SET) != 0 ||
        fread(p->index, 8, h.frameCount, p->f) != h.frameCount) {
        snprintf(b, sizeof(b), "%s: index unreadable (%lu frames)", path, (unsigned long)h.frameCount);
        mlog(b);
        release(p);
        return false;
    }

    /* +64: the decoder is handed an exact size, but nothing promises it never
     * reads a word past it. The output slots are not cleared: the decoder or
     * the test card fills each whole before it is shown. Their cache lines
     * are cleaned, so nothing a previous owner left dirty is written back
     * over a decoded frame. */
    u32 outBytes = (u32)p->codedW * p->codedH * 2;
    u32 packetBytes = (p->maxAu + 64 + 0xFFF) & ~0xFFFu;
    p->packet = (u8 *)linearMemAlign(packetBytes, 0x80);
    bool ok = p->packet != NULL;
    for (int i = 0; i < MVD_SLOTS && ok; i++) {
        p->out[i] = (u8 *)linearMemAlign(outBytes, 0x80);
        ok = p->out[i] != NULL;
    }
    if (!ok ||
        !C3D_TexInit(&p->tex[0], p->codedW, p->codedH, GPU_RGB565) ||
        !C3D_TexInit(&p->tex[1], p->codedW, p->codedH, GPU_RGB565)) {
        snprintf(b, sizeof(b), "%s: out of linear memory (%lu free)", path, (unsigned long)linearSpaceFree());
        mlog(b);
        release(p);
        return false;
    }
    memset(p->packet, 0, packetBytes);
    GSPGPU_FlushDataCache(p->packet, packetBytes);
    for (int i = 0; i < MVD_SLOTS; i++)
        GSPGPU_FlushDataCache(p->out[i], outBytes);
    for (int i = 0; i < 2; i++) {
        C3D_TexSetFilter(&p->tex[i], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&p->tex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        p->image[i].tex = &p->tex[i];
        p->image[i].subtex = &p->subtex;
    }
    p->subtex.width  = p->picW;
    p->subtex.height = p->picH;
    p->subtex.left   = (float)p->picX / p->codedW;
    p->subtex.right  = (float)(p->picX + p->picW) / p->codedW;
    p->subtex.top    = 1.0f - (float)p->picY / p->codedH;
    p->subtex.bottom = 1.0f - (float)(p->picY + p->picH) / p->codedH;

    if (selfTest) {
        snprintf(b, sizeof(b), "%s: SELF-TEST card, %lu frames @ %.3f fps, %ux%u in %ux%u",
                 path, (unsigned long)p->frameCount, p->fps, p->picW, p->picH, p->codedW, p->codedH);
        mlog(b);
    } else {
        /* Work buffer: the size SKATER (the system browser) runs every video
         * with -- libctru's MVD_DEFAULT_WORKBUF_SIZE, ~9 MB. The libctru this
         * builds against has no CalculateWorkBufSize wrapper, and a level-2.1
         * 512x256 stream needs far less, so the fixed size is a safe ceiling.
         * mvdstdInit() allocates it from linear memory and mvdstdExit() frees
         * it, so it is only held while a clip plays. */
        u32 work = MVD_DEFAULT_WORKBUF_SIZE;
        Result rc = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, work, NULL);
        snprintf(b, sizeof(b),
                 "%s: %lu frames @ %.3f fps, %ux%u in %ux%u, level_idc %u; work buffer %lu; "
                 "linear heap 0x%08lx, %lu free; mvdstdInit -> 0x%08lx",
                 path, (unsigned long)p->frameCount, p->fps, p->picW, p->picH, p->codedW, p->codedH,
                 h.levelIdc, (unsigned long)work,
                 (unsigned long)__ctru_linear_heap, (unsigned long)linearSpaceFree(), (unsigned long)rc);
        mlog(b);
        if (R_FAILED(rc)) {
            release(p);
            return false;
        }
        p->mvdOpen = true;

        for (int i = 0; i < MVD_SLOTS; i++)
            mvdstdGenerateDefaultConfig(&p->cfg[i], p->codedW, p->codedH, p->codedW, p->codedH,
                                        NULL, (u32 *)p->out[i], (u32 *)p->out[i]);

        if (!feed_param_set(p, h.spsOffset, h.spsSize, "SPS") ||
            !feed_param_set(p, h.ppsOffset, h.ppsSize, "PPS")) {
            release(p);
            return false;
        }
        p->first = true;
    }

    /* The first picture now, on this thread, so a decoder that accepts
     * everything but draws nothing (an emulator's stub, a firmware surprise)
     * is caught here and the caller can still play the .vp6 from the top. */
    if (!produce_one(p)) {
        snprintf(b, sizeof(b), "%s: no first picture (%s) -- falling back", path,
                 p->failed ? "decoder error" : "empty clip");
        mlog(b);
        release(p);
        return false;
    }
    bool threaded = start_thread(p);
    double tpm = (double)SYSCLOCK_ARM11 / 1000.0;
    snprintf(b, sizeof(b), "%s active, opened in %.1f ms; decoding %s, %d frames ahead",
             selfTest ? "self-test card" : "hardware decode",
             (double)(svcGetSystemTick() - tOpen) / tpm,
             threaded ? "on its own thread" : "on the main thread (no thread)", MVD_SLOTS - 1);
    mlog(b);
    return true;
}

bool mvd_player_advance(MvdPlayer *p, unsigned count) {
    if (count == 0)
        count = 1;
    const u32 target = p->frames_decoded + count - 1;

    /* Without the thread, decode here: what is due, a few at most. */
    if (!p->thread) {
        LightLock_Lock(&p->lock);
        for (int i = 0; i < MVD_SLOTS; i++)
            if (p->slotState[i] == SLOT_SHOWN)
                slot_free_locked(p, i);
        LightLock_Unlock(&p->lock);
        for (int n = 0; n < 4 && p->produced <= target && produce_one(p); n++)
            ;
    }

    int show = -1;
    bool more;
    LightLock_Lock(&p->lock);
    if (p->thread)
        for (int i = 0; i < MVD_SLOTS; i++)
            if (p->slotState[i] == SLOT_SHOWN)
                slot_free_locked(p, i);   /* its transfer ran last frame */
    for (int i = 0; i < MVD_SLOTS; i++)
        if (p->slotState[i] == SLOT_READY && p->slotFrame[i] <= target &&
            (show < 0 || p->slotFrame[i] > p->slotFrame[show]))
            show = i;
    if (show >= 0)
        for (int i = 0; i < MVD_SLOTS; i++)
            if (i != show && p->slotState[i] == SLOT_READY && p->slotFrame[i] < p->slotFrame[show]) {
                slot_free_locked(p, i);   /* passed over: never transferred */
                p->statDropped++;
            }
    more = !p->ended;
    for (int i = 0; i < MVD_SLOTS && !more; i++)
        more = p->slotState[i] == SLOT_READY;
    if (show >= 0)
        p->slotState[show] = SLOT_SHOWN;
    LightLock_Unlock(&p->lock);

    if (show >= 0) {
        upload(p, show);
        p->frames_decoded = p->slotFrame[show] + 1;
        p->statShown++;
        return true;
    }
    if (more) {
        p->statStarved++;   /* due, but the decoder is behind: keep the last */
        return true;
    }
    if (p->failed)
        mlog("decoder error -- ending clip early");
    return false;
}

void mvd_player_draw(MvdPlayer *p, float boxX, float boxY, float boxW, float boxH) {
    if (!p->tex_ready)
        return;
    C2D_DrawImageAt(p->image[p->dispTex], boxX, boxY, 0.5f, NULL,
                    boxW / (float)p->picW, boxH / (float)p->picH);
}

void mvd_player_close(MvdPlayer *p) {
    double tpm = (double)SYSCLOCK_ARM11 / 1000.0;
    u64 t0 = svcGetSystemTick();
    bool log = p->f && p->statFed;
    stop_thread(p);
    u64 tStop = svcGetSystemTick() - t0;
    if (log) {
        char b[300];
        snprintf(b, sizeof(b),
                 "closed after %u/%u frames: fed %lu, no-picture %lu, drained %lu, render calls %lu, "
                 "decode avg %.2f ms max %.2f ms; shown %lu, dropped %lu, waits on the decoder %lu; "
                 "thread stopped in %.1f ms",
                 p->frames_decoded, p->nb_frames, (unsigned long)p->statFed, (unsigned long)p->statNoOutput,
                 (unsigned long)p->statDrained, (unsigned long)p->statRenderSpins,
                 (double)p->statDecodeTicks / (double)p->statFed / tpm, (double)p->statMaxDecodeTicks / tpm,
                 (unsigned long)p->statShown, (unsigned long)p->statDropped, (unsigned long)p->statStarved,
                 (double)tStop / tpm);
        mlog(b);
    }
    release(p);
}
