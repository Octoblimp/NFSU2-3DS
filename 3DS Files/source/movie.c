/*
 * movie.c -- see movie.h.
 */
#include "movie.h"
#include "dbglog.h"
#include <string.h>

/* The hardware path decodes on its own thread ahead of the screen (#440), so
 * a render tick only picks the newest frame that is due: after a hitch it
 * jumps straight back onto the wall clock, the frames in between decoded (H.264
 * needs them as references) but never transferred. */

static bool g_hwVideo = true;
static bool g_hwSelfTest = false;

static void movie_log(const char *msg) {
    dbglog_line("[movie] ", msg);
}

void movie_set_policy(bool hwVideo, bool hwSelfTest) {
    g_hwVideo = hwVideo;
    g_hwSelfTest = hwSelfTest;
    /* HwVideo=0 is the all-CPU baseline: no MVD, and no Y2R under VP6. */
    vp6_set_hw_colour(hwVideo);
}

bool movie_open(MoviePlayer *m, const char *mvdPath, const char *vp6Path, int vp6DownscaleDiv) {
    char b[700];
    memset(m, 0, sizeof(*m));
    if (mvdPath && (g_hwVideo || g_hwSelfTest)) {
        if (mvd_player_open(&m->mvd, mvdPath, g_hwSelfTest)) {
            m->hw = true;
            snprintf(b, sizeof(b), "%s: %s", mvdPath, movie_backend(m));
            movie_log(b);
            return true;
        }
    }
    if (vp6_player_open(&m->vp6, vp6Path, vp6DownscaleDiv)) {
        snprintf(b, sizeof(b), "%s: %s%s", vp6Path, movie_backend(m),
                 !mvdPath ? " (no hardware version)" : !g_hwVideo ? " (HwVideo=0)" : "");
        movie_log(b);
        return true;
    }
    return false;
}

bool movie_advance(MoviePlayer *m, unsigned wantFrame) {
    if (!m->hw)
        return vp6_player_advance(&m->vp6);
    unsigned have = m->mvd.frames_decoded;
    unsigned count = wantFrame >= have ? wantFrame + 1 - have : 1;
    return mvd_player_advance(&m->mvd, count);
}

void movie_draw(MoviePlayer *m, float boxX, float boxY, float boxW, float boxH) {
    if (m->hw)
        mvd_player_draw(&m->mvd, boxX, boxY, boxW, boxH);
    else
        vp6_player_draw(&m->vp6, boxX, boxY, boxW, boxH);
}

void movie_close(MoviePlayer *m) {
    if (m->hw)
        mvd_player_close(&m->mvd);
    else
        vp6_player_close(&m->vp6);
    m->hw = false;
}

double movie_fps(const MoviePlayer *m) {
    return m->hw ? m->mvd.fps : m->vp6.fps;
}

unsigned movie_frames_decoded(const MoviePlayer *m) {
    return m->hw ? m->mvd.frames_decoded : m->vp6.frames_decoded;
}

bool movie_tex_ready(const MoviePlayer *m) {
    return m->hw ? m->mvd.tex_ready : m->vp6.tex_ready;
}

void movie_dims(const MoviePlayer *m, int *vidW, int *vidH, int *texW, int *texH) {
    if (m->hw) {
        *vidW = m->mvd.picW; *vidH = m->mvd.picH;
        *texW = m->mvd.codedW; *texH = m->mvd.codedH;
    } else {
        *vidW = m->vp6.vid_w; *vidH = m->vp6.vid_h;
        *texW = m->vp6.tex_w; *texH = m->vp6.tex_h;
    }
}

const char *movie_backend(const MoviePlayer *m) {
    if (!m->hw)
        return m->vp6.y2r ? "software VP6, hardware colour (Y2R)" : "software VP6";
    return m->mvd.selfTest ? "hardware path SELF-TEST card" : "hardware H.264 (MVD)";
}
