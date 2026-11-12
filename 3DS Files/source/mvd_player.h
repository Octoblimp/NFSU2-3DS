/*
 * mvd_player.h -- plays a boot/cutscene movie through the New 3DS's hardware
 * H.264 decoder (the `mvd:STD` service) instead of the software VP6 decoder.
 *
 * Input is <GameDir>/video/<name>.mvd, written by tools/fmv_mvd_pack.py from
 * the real MOVIES/<name>.vp6: the same frames, re-encoded as baseline H.264
 * with an index. The decode block writes each frame as linear RGB565 straight
 * into a buffer in linear memory, and the GPU's transfer engine tiles that
 * into a texture -- no ARM11 work per pixel at all, where the VP6 path spends
 * a whole core on bitstream decode plus a YUV->RGB+swizzle pass.
 *
 * The decoder runs on its own thread (core 2), up to MVD_SLOTS frames ahead
 * of the screen; the main thread only picks the newest frame that is due and
 * queues its copy into a texture (TASKS.md #440).
 *
 * New 3DS only. mvd_player_open() refuses (and logs why) on an Old 3DS, when
 * the MVD sysmodule is not running, when the .mvd is missing, or when the
 * decoder does not produce the clip's first frame -- the caller then plays the
 * .vp6 instead (see movie.c). Azahar registers an MVD service but has no H.264
 * decoder behind it, so on the emulator this always falls back; config
 * HwVideoSelfTest=1 runs a test card through the same slots, thread and
 * transfer instead.
 */
#ifndef MVD_PLAYER_H
#define MVD_PLAYER_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stdio.h>

/* Decoder output buffers: one being decoded, one being copied to the
 * texture, the rest decoded ahead. */
#define MVD_SLOTS 4

typedef struct MvdPlayer {
    FILE    *f;
    u32     *index;          /* frameCount x {file offset, size} */
    u32      frameCount;
    u32      maxAu;
    u32      nextAu;         /* next access unit to feed the decoder */

    u16      codedW, codedH; /* decoder output == texture size (512x256) */
    u16      picX, picY, picW, picH; /* the real picture inside it */
    u8       levelIdc;

    u8      *packet;         /* linear: one access unit, read straight from SD */
    u8      *out[MVD_SLOTS]; /* linear: decoder output, codedW x codedH RGB565 */
    MVDSTD_Config cfg[MVD_SLOTS]; /* one per output buffer */
    bool     mvdOpen;        /* mvdstdInit() succeeded, mvdstdExit() owed */
    bool     selfTest;       /* synthetic frames instead of MVD -- see .c */
    bool     first;          /* next access unit is the clip's first */
    bool     skip;           /* last feed already produced its frame: drain
                                any pending output before feeding more */

    /* Slots, shared with the decode thread under 'lock'. */
    u8       slotState[MVD_SLOTS];
    u32      slotFrame[MVD_SLOTS];
    u32      produced;       /* frames decoded so far (the next one's number) */
    bool     ended;          /* the thread has stopped: end of clip or error */
    bool     failed;         /* ... because of a decoder error */
    volatile bool stop;      /* close asked the thread to stop */
    LightLock      lock;
    LightSemaphore freeSlots;
    Thread   thread;         /* NULL: decoded on the main thread instead */

    C3D_Tex           tex[2];
    Tex3DS_SubTexture subtex;
    C2D_Image         image[2];
    int      dispTex;
    bool     tex_ready;

    double   fps;
    unsigned nb_frames;
    unsigned frames_decoded; /* frames handed to the screen, same meaning as
                                Vp6Player.frames_decoded */

    /* diagnostics, logged at close */
    u32      statFed, statNoOutput, statDrained, statRenderSpins;
    u32      statShown, statDropped, statStarved;
    u64      statDecodeTicks, statMaxDecodeTicks;
} MvdPlayer;

bool mvd_player_open(MvdPlayer *p, const char *path, bool selfTest);

/* Shows the newest decoded frame numbered below frames_decoded + count (the
 * frames it passes over are dropped, never transferred). Call between
 * C3D_FrameBegin() and the draw. false = the clip is over. */
bool mvd_player_advance(MvdPlayer *p, unsigned count);

void mvd_player_draw(MvdPlayer *p, float boxX, float boxY, float boxW, float boxH);

void mvd_player_close(MvdPlayer *p);

#endif /* MVD_PLAYER_H */
