/*
 * movie.h -- one boot/cutscene movie, played by whichever decoder can.
 *
 * The New 3DS's hardware H.264 decoder (mvd_player.c, playing
 * <GameDir>/video/<name>.mvd) when it is there and the clip has been packed
 * for it; otherwise the software VP6 decoder on the real MOVIES/<name>.vp6
 * (vp6/vp6_display.c), whose colour conversion runs on the y2r:u block when
 * it can (#197). The choice is made once per clip at open, and logged.
 */
#ifndef MOVIE_H
#define MOVIE_H

#include "mvd_player.h"
#include "vp6/vp6_display.h"

typedef struct MoviePlayer {
    bool      hw;   /* true: mvd is live; false: vp6 is */
    MvdPlayer mvd;
    Vp6Player vp6;
} MoviePlayer;

/* nfsu2_config.txt's HwVideo (default on) and HwVideoSelfTest (default off).
 * HwVideo=0 forces the VP6 path everywhere, for A/B comparison on a New 3DS.
 * HwVideoSelfTest=1 replaces every movie with mvd_player.c's test card,
 * hardware or not. */
void movie_set_policy(bool hwVideo, bool hwSelfTest);

/* mvdPath may be NULL (no hardware version of this clip). */
bool movie_open(MoviePlayer *m, const char *mvdPath, const char *vp6Path, int vp6DownscaleDiv);

/* Brings the screen up to frame 'wantFrame' (0-based, from the wall clock).
 * The hardware path catches up several frames in one call; the VP6 path
 * decodes at most one per call (see the pacing comment in main.c). Returns
 * false once the clip has no more frames. Call after C3D_FrameBegin(). */
bool movie_advance(MoviePlayer *m, unsigned wantFrame);

/* Stretches the current frame over the box, top-left at (boxX, boxY). */
void movie_draw(MoviePlayer *m, float boxX, float boxY, float boxW, float boxH);
void movie_close(MoviePlayer *m);

double      movie_fps(const MoviePlayer *m);
unsigned    movie_frames_decoded(const MoviePlayer *m);
bool        movie_tex_ready(const MoviePlayer *m);
/* What is on screen: picture size and the texture it sits in. */
void        movie_dims(const MoviePlayer *m, int *vidW, int *vidH, int *texW, int *texH);
const char *movie_backend(const MoviePlayer *m);

#endif /* MOVIE_H */
