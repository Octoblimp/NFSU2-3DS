// EA Trax -- see eatrax.h for the reverse-engineering notes.

#include "eatrax.h"
#include "dbglog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Hold is the port's own choice (the real chyron is up for as long as the
// screen wants it). The two fade lengths are not: they are the authored
// durations of Chyron_FE.fng's entry and exit animation scripts -- see
// eatrax.h.
#define POPUP_HOLD_SECONDS  5.0f
#define POPUP_IN_SECONDS   (EATRAX_POPUP_IN_MS  / 1000.0f)
#define POPUP_OUT_SECONDS  (EATRAX_POPUP_OUT_MS / 1000.0f)
#define POPUP_TOTAL_SECONDS (POPUP_IN_SECONDS + POPUP_HOLD_SECONDS + POPUP_OUT_SECONDS)

static EatraxTrack s_tracks[EATRAX_MAX_TRACKS];
static int   s_count;
static int   s_bySong[OPTIONS_TRAX_SONGS]; // song number -> s_tracks slot, -1 if absent
static char  s_dir[512];

// One context's rotation; see eatrax.h for the offsets.
typedef struct {
	u32 all;
	u32 left;
	int position;
} Rotation;

static OptTraxSong s_table[OPTIONS_TRAX_SONGS];
static Rotation s_rot[2];
static u32 s_order = 1;              // +0x9C

static int   s_current = -1;         // the song playing, -1 for none
static EatraxContext s_currentCtx = EATRAX_CTX_FE;

static float s_popupTimer;           // counts down from POPUP_TOTAL_SECONDS
static int   s_popupSong = -1;

// Whether the manifest carries `song`. (s_bySong is only meaningful once a
// manifest has loaded.)
static bool haveSong(u32 song)
{
	return s_count > 0 && song < OPTIONS_TRAX_SONGS && s_bySong[song] >= 0;
}

// Copies a manifest field, trimming trailing CR/LF. Fields are UTF-8 and never
// contain '|', which is what makes the split unambiguous.
static void copyField(char* dst, size_t cap, const char* src)
{
	size_t n = 0;
	while (src[n] && src[n] != '\n' && src[n] != '\r' && n + 1 < cap) { dst[n] = src[n]; n++; }
	dst[n] = '\0';
}

static EatraxBank parseBank(const char* s)
{
	if (!strncmp(s, "IG", 2)) return EATRAX_BANK_IG;
	if (!strncmp(s, "AL", 2)) return EATRAX_BANK_AL;
	return EATRAX_BANK_FE;
}

bool eatraxInit(const char* dataDir)
{
	s_count = 0;
	s_current = -1;
	s_popupTimer = 0.0f;
	s_popupSong = -1;
	for (int i = 0; i < OPTIONS_TRAX_SONGS; i++)
		s_bySong[i] = -1;

	snprintf(s_dir, sizeof(s_dir), "%s/eatrax", dataDir);

	char path[600];
	snprintf(path, sizeof(path), "%s/tracks.txt", s_dir);
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
		if (!strncmp(line, "count ", 6) || !strncmp(line, "rate ", 5)) continue;
		if (s_count >= EATRAX_MAX_TRACKS) break;

		// index|bank|file|title|artist|album
		char* fields[6];
		int n = 0;
		char* p = line;
		fields[n++] = p;
		while (n < 6 && (p = strchr(p, '|')) != NULL) { *p++ = '\0'; fields[n++] = p; }
		if (n != 6) continue;

		EatraxTrack* t = &s_tracks[s_count];
		t->index = atoi(fields[0]);
		if (t->index < 0 || t->index >= OPTIONS_TRAX_SONGS || s_bySong[t->index] >= 0) continue;
		t->bank  = parseBank(fields[1]);
		copyField(t->file,   sizeof(t->file),   fields[2]);
		copyField(t->title,  sizeof(t->title),  fields[3]);
		copyField(t->artist, sizeof(t->artist), fields[4]);
		copyField(t->album,  sizeof(t->album),  fields[5]);
		s_bySong[t->index] = s_count;
		s_count++;
	}
	fclose(f);

	if (s_count <= 0) return false;

	// Until a profile says otherwise, the table a fresh profile has.
	OptTraxSong table[OPTIONS_TRAX_SONGS];
	options_default_trax(table);
	eatraxSetSongs(table, 1);
	srand((unsigned)time(NULL));
	return true;
}

void eatraxExit(void)
{
	eatraxStop();
	s_count = 0;
}

int eatraxTrackCount(void) { return s_count; }

const EatraxTrack* eatraxTrack(int song)
{
	if (song < 0 || !haveSong((u32)song)) return NULL;
	return &s_tracks[s_bySong[song]];
}

int eatraxCurrentSong(void) { return s_current; }

const EatraxTrack* eatraxCurrentTrack(void) { return eatraxTrack(s_current); }

EatraxContext eatraxCurrentContext(void) { return s_currentCtx; }

void eatraxSetSongs(const OptTraxSong table[OPTIONS_TRAX_SONGS], u32 playOrder)
{
	memcpy(s_table, table, sizeof(s_table));
	s_order = playOrder;
	memset(s_rot, 0, sizeof(s_rot));
	for (int p = 0; p < OPTIONS_TRAX_SONGS; p++) {
		const u32 song = s_table[p].song;
		// PORT: a song the manifest does not carry stays out of both
		// rotations; retail's table always has all 27.
		if (!haveSong(song)) continue;
		if (s_table[p].play & OPTIONS_TRAX_MENU) s_rot[EATRAX_CTX_FE].all |= 1u << song;
		if (s_table[p].play & OPTIONS_TRAX_RACING) s_rot[EATRAX_CTX_IG].all |= 1u << song;
	}
	for (int c = 0; c < 2; c++) {
		s_rot[c].left = s_rot[c].all;
		s_rot[c].position = -1;
	}
}

void eatraxTitleOpener(void)
{
	// 0x47AECB tests the first table entry's playability but sets bit 0, song
	// 0's, whichever song that entry holds; both are ported as they stand.
	if ((s_table[0].play & OPTIONS_TRAX_MENU) && haveSong(0))
		s_rot[EATRAX_CTX_FE].left = 1;
}

// 0x46B6F0: the next song in `ctx`, taken out of its `left`, or -1.
static int nextSong(EatraxContext ctx)
{
	Rotation* r = &s_rot[ctx];
	if (!r->all) return -1;
	if (!r->left) {
		r->left = r->all;
		r->position = -1;
	}

	if (s_order == 0) {
		for (int p = r->position + 1; p < OPTIONS_TRAX_SONGS; p++) {
			const u32 song = s_table[p].song;
			if (song < OPTIONS_TRAX_SONGS && (r->left & (1u << song))) {
				r->left &= ~(1u << song);
				r->position = p;
				return (int)song;
			}
		}
		return -1;
	}

	int n = 0;
	for (int s = 0; s < OPTIONS_TRAX_SONGS; s++)
		if (r->left & (1u << s)) n++;
	if (n <= 0) return -1;
	int pick = rand() % n;
	for (int s = 0; s < OPTIONS_TRAX_SONGS; s++) {
		if (!(r->left & (1u << s))) continue;
		if (pick-- == 0) {
			r->left &= ~(1u << s);
			return s;
		}
	}
	return -1;
}

static void raisePopup(int song)
{
	s_popupSong = song;
	s_popupTimer = POPUP_TOTAL_SECONDS;
}

static bool playSong(int song, EatraxContext ctx)
{
	const EatraxTrack* t = eatraxTrack(song);
	if (!t) return false;

	char path[700];
	snprintf(path, sizeof(path), "%s/%s", s_dir, t->file);

	// EA Trax songs never loop -- the rotation moves on when one drains.
	AudioBus bus = (ctx == EATRAX_CTX_IG) ? AUDIO_BUS_IG_MUSIC : AUDIO_BUS_FE_MUSIC;
	if (!audioMusicPlay(AUDIO_MUSIC_A, path, bus, false)) {
		s_current = -1;
		return false;
	}

	s_current = song;
	s_currentCtx = ctx;
	raisePopup(song);
	return true;
}

bool eatraxPlayNext(EatraxContext ctx)
{
	if (s_count <= 0) return false;
	const int song = nextSong(ctx);
	if (song < 0) {
		eatraxStop();
		s_currentCtx = ctx;
		return false;
	}
	return playSong(song, ctx);
}

bool eatraxPreview(int song)
{
	return playSong(song, EATRAX_CTX_FE);
}

void eatraxStop(void)
{
	audioMusicStop(AUDIO_MUSIC_A);
	s_current = -1;
}

bool eatraxIsPlaying(void) { return audioMusicIsPlaying(AUDIO_MUSIC_A); }

void eatraxPause(bool paused) { audioMusicPause(AUDIO_MUSIC_A, paused); }

void eatraxUpdate(float dt)
{
	if (s_count <= 0) return;

	if (s_popupTimer > 0.0f) {
		s_popupTimer -= dt;
		if (s_popupTimer < 0.0f) s_popupTimer = 0.0f;
	}

	// A drained stream means the song ended; the same context picks again.
	// One that never opened (audioMusicPlay only asks, #342) ends the rotation,
	// as a song that would not start always has.
	if (s_current >= 0 && audioMusicFinished(AUDIO_MUSIC_A)) {
		if (audioMusicFailed(AUDIO_MUSIC_A)) {
			char b[320];
			const EatraxTrack* t = eatraxTrack(s_current);
			snprintf(b, sizeof(b), "EA Trax: song %d (%s) would not start: %s", s_current,
				t ? t->file : "?", audioMusicStartBreakdown(AUDIO_MUSIC_A));
			dbglog_line(NULL, b);
			s_current = -1;
			return;
		}
		eatraxPlayNext(s_currentCtx);
	}
}

bool eatraxPopupActive(void)
{
	return s_popupTimer > 0.0f && s_popupSong >= 0;
}

float eatraxPopupElapsedMs(void)
{
	if (!eatraxPopupActive()) return 0.0f;
	return (POPUP_TOTAL_SECONDS - s_popupTimer) * 1000.0f;
}

EatraxPopupPhase eatraxPopupPhase(float* outMs)
{
	float elapsed = eatraxPopupElapsedMs();
	if (elapsed < EATRAX_POPUP_IN_MS) {
		if (outMs) *outMs = elapsed;
		return EATRAX_POPUP_PHASE_IN;
	}
	if (s_popupTimer > POPUP_OUT_SECONDS) {
		if (outMs) *outMs = elapsed - EATRAX_POPUP_IN_MS;
		return EATRAX_POPUP_PHASE_HOLD;
	}
	if (outMs) *outMs = (POPUP_OUT_SECONDS - s_popupTimer) * 1000.0f;
	return EATRAX_POPUP_PHASE_OUT;
}

// Kept as the coarse "is it on screen" ramp for anything that only wants one
// number; the chyron renderer drives its elements off eatraxPopupPhase()
// instead, because the authored scripts fade the plate, the end cap and the EA
// badge on three different schedules inside the same window.
float eatraxPopupAlpha(void)
{
	float ms = 0.0f;
	switch (eatraxPopupPhase(&ms)) {
	case EATRAX_POPUP_PHASE_IN:  return ms / EATRAX_POPUP_IN_MS;
	case EATRAX_POPUP_PHASE_OUT: return 1.0f - ms / EATRAX_POPUP_OUT_MS;
	default:                     return eatraxPopupActive() ? 1.0f : 0.0f;
	}
}

const EatraxTrack* eatraxPopupTrack(void) { return eatraxTrack(s_popupSong); }

void eatraxDismissPopup(void) { s_popupTimer = 0.0f; }

// Pins a raised popup at the end of its hold window: the countdown may still
// run the 800ms entry, but it is not allowed to fall into the 600ms exit while
// the caller keeps asking. Called every frame by the title screen -- that is
// the one screen in the boot flow that waits on the player instead of a timer,
// so the chyron's fixed 5s hold would otherwise expire long before the player
// reacts to "Please press START to begin" and the very first now-playing popup
// of the session would be gone unseen. Stop calling it and the popup retracts
// on its own from wherever the timer stands (see main.c's BOOT_TITLE leg).
void eatraxHoldPopup(void)
{
	if (!eatraxPopupActive()) return;
	const float floorT = POPUP_OUT_SECONDS + (1.0f / 30.0f);
	if (s_popupTimer < floorT) s_popupTimer = floorT;
}
