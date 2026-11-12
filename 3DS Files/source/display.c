// The PC's display settings and their per-profile file (TASKS.md #326). See
// display.h.
#include "display.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbglog.h"

DisplaySettings g_display;

// The registry's value names, FUN_005BEA20 / FUN_005BEEA0, in the block's
// order. g_PerformanceLevel is not in the block but is saved in the same key,
// between g_RacingResolution and g_VSyncOn; the file keeps the PC's order.
// NotFirstTime is a value of its own that FUN_005B76F0 reads and sets to 1
// on every start (0x5B789E), so it is not written here: a file that exists is
// a profile that has run.
typedef struct {
	const char* name;
	int index; // u32 index in the block, or -1 for g_PerformanceLevel
} DisplayKey;

static const DisplayKey kKeys[] = {
	{ "VERSION", 0 },
	{ "SIZE", 1 },
	{ "g_CarEnvironmentMapEnable", 2 },
	{ "g_CarEnvironmentMapUpdateData", 3 },
	{ "g_CarShadowEnable", 4 },
	{ "g_CarHeadlightEnable", 5 },
	{ "g_CarLightingEnable", 6 },
	{ "g_CarDamageEnable", 7 },
	{ "g_CrowdEnable", 8 },
	{ "g_RoadReflectionEnable", 9 },
	{ "g_FogEnable", 10 },
	{ "g_MotionBlurEnable", 11 },
	{ "g_LightStreaksEnable", 12 },
	{ "g_LightGlowEnable", 13 },
	{ "g_AnimatedTextureEnable", 14 },
	{ "g_ParticleSystemEnable", 15 },
	{ "g_DepthOfFieldEnable", 16 },
	{ "g_WorldLodLevel", 17 },
	{ "g_CarLodLevel", 18 },
	{ "g_OverBrightEnable", 19 },
	{ "g_BleachByPassEnable", 20 },
	{ "g_TintingEnable", 21 },
	{ "g_FSAALevel", 22 },
	{ "g_HorizonFogEnable", 23 },
	{ "g_RainEnable", 24 },
	{ "g_TextureFiltering", 26 },
	{ "g_RacingResolution", 27 },
	{ "g_PerformanceLevel", -1 },
	{ "g_VSyncOn", 29 },
};
#define KEY_COUNT ((int)(sizeof(kKeys) / sizeof(kKeys[0])))

// PORT: the stereo settings' names.
#define KEY_STEREO "Stereo3D"
#define KEY_STEREO_DEPTH "Stereo3DDepth"
#define KEY_STEREO_FOCUS "Stereo3DFocus"
#define KEY_STEREO_HUD "Stereo3DHud"
#define KEY_STEREO_STYLE "Stereo3DStyle"

// The caps, FUN_005BE200's table at 0x8764D0, one per value from envmap to
// texFilter. The PC fills it from what the card can do; here a value's cap is
// the highest level whose effect this port draws, so a row for an effect that
// is not ported yet stays hidden as the PC hides a row its card cannot do,
// and the cap rises when the effect lands. Above the cap a value is clamped,
// as FUN_005BE690 clamps it.
static const u32 kCaps[DISPLAY_CAPPED] = {
	3, // +0x08 envmap: envmap.c's schedules for levels 1, 2 and 3
	1, // +0x0C envUpdate: 1 the scenery in the faces (FUN_005d6ad0), 0 none
	2, // +0x10 shadow: 1 the grid shadow, 2 the hull shadow under it too (carshadow_draw_hull)
	1, // +0x14 headlight
	0, // +0x18 carLighting: 0 on every PC card too
	1, // +0x1C carDamage: on the PC 0 on every card, read by nothing but the
	   //       clamp; here the Gameplay page's Car Damage row (#324), whose
	   //       cracked windows are ported (cardamage.c). No Customize row.
	1, // +0x20 crowd: the NIS people (crowd.c) and the race-start scene (#326)
	3, // +0x24 roadRefl: 1 the world, 2 the cars too, 3 blurred (reflection_blur)
	1, // +0x28 fog
	1, // +0x2C motionBlur: smear.c, the GameCube's eMotionBlurEffect (#399)
	1, // +0x30 lightStreaks: the flare pass's fading buffer, trails.c (#326)
	1, // +0x34 lightGlow: the flares
	1, // +0x38 animTex: the world's texture animations (model3d.c, #326)
	1, // +0x3C particles: afx.c
	1, // +0x40 dof: an NIS camera's depth of field, trails.c (#326)
	2, // +0x44 worldLod: FUN_005BE1C0 reads 3 as 2
	1, // +0x48 carLod
	1, // +0x4C overBright: the frame's glow into the light trail buffer, trails.c (#326)
	1, // +0x50 bleach: the visual treatment's grade, bleach.c (#326)
	1, // +0x54 tinting: never clamped (FUN_005BE690 skips it); the cap gates the row
	0, // +0x58 fsaa: CUT (#473, the user 2026-11-09: "Cut FSAA, leave light trails.
	   //       FSAA does a lot of damage to FPS."). Its 2x supersample cost the 3D's
	   //       fill twice over and 1.5 MB of VRAM. The code is gone and so is the
	   //       row; the value stays in the block (the PC's save layout), clamped 0.
	1, // +0x5C horizonFog: hfog.c
	1, // +0x60 rain: the Rain Splatter lens drops, rain.c (#406)
	1, // +0x64 the unnamed value
	1, // +0x68 texFilter: the PICA200 has point and bilinear, no anisotropic
};

static void dlogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlogf(const char* fmt, ...) {
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line(NULL, b);
}

u32* display_value(DisplaySettings* d, int index) {
	return (u32*)&d->b + index;
}

u32 display_cap(int index) {
	const int i = index - DISPLAY_FIRST_VALUE;
	return (i >= 0 && i < DISPLAY_CAPPED) ? kCaps[i] : 0;
}

void display_default(DisplaySettings* d) {
	memset(d, 0, sizeof(*d));
	d->b.version = 1;
	d->b.size = sizeof(DisplayBlock);
	d->b.animTex = 1;
	d->b.u64 = 1;
	d->b.texFilter = 1;
	d->b.notFirstTime = 0;
	display_default_stereo(d);
}

void display_default_stereo(DisplaySettings* d) {
	d->stereo = 1;
	d->stereoDepth = DISPLAY_STEREO_STEPS;
	d->stereoFocus = DISPLAY_STEREO_STEPS / 2;
	d->stereoHud = DISPLAY_STEREO_STEPS / 2;
	d->stereoStyle = DISPLAY_STEREO_INTO;
}

// FUN_005BE870, from g_PerformanceLevel.
static void preset(DisplaySettings* d) {
	DisplayBlock* b = &d->b;
	const u32 L = d->performanceLevel;
	b->shadow = 0;
	b->roadRefl = 0;
	b->rain = 0;
	b->bleach = 0;
	b->fsaa = 0;
	b->vsync = 0;

	const u32 l1 = (L > 1);
	if (l1)
		b->shadow = 2;
	b->tinting = l1;
	b->worldLod = l1;
	b->lightGlow = l1;
	b->crowd = l1;
	b->headlight = l1;

	const u32 l2 = (L > 2);
	if (l2) {
		b->roadRefl = 2;
		b->worldLod = 2;
	}
	b->texFilter = 1;
	b->horizonFog = l2;
	b->overBright = l2;
	b->carLod = l2;
	b->dof = l2;
	b->particles = l2;
	b->lightStreaks = l2;
	b->motionBlur = l2;
	b->fog = l2;
	b->envUpdate = l2;
	b->envmap = l2;

	if (L > 3) {
		b->envmap = 2;
		b->rain = 1;
		b->roadRefl = 3;
		b->bleach = 1;
		// The PC raises the resolution to its second mode here when the
		// card's score (FUN_005C0CD0, FUN_0075BED4) is over 4. The 3DS
		// panel has one mode. Its b->fsaa = 1 is left out: FSAA is cut.
	}
	if (L > 4) {
		b->texFilter = 2;
		b->envmap = 3;
	}
}

void display_clamp(DisplaySettings* d) {
	for (int i = DISPLAY_FIRST_VALUE; i < DISPLAY_FIRST_VALUE + DISPLAY_CAPPED; i++) {
		if (i == 0x54 / 4) // tinting
			continue;
		u32* v = display_value(d, i);
		if (*v > display_cap(i))
			*v = display_cap(i);
	}
	// The resolution clamp walks the card's mode table down from the set one;
	// the 3DS has mode 0 only.
	d->b.racingRes = 0;
	if (d->performanceLevel > DISPLAY_LEVEL_MAX)
		d->performanceLevel = DISPLAY_LEVEL_MAX;
	if (d->stereo > 1)
		d->stereo = 1;
	if (d->stereoDepth > DISPLAY_STEREO_STEPS)
		d->stereoDepth = DISPLAY_STEREO_STEPS;
	if (d->stereoFocus > DISPLAY_STEREO_STEPS)
		d->stereoFocus = DISPLAY_STEREO_STEPS;
	if (d->stereoHud > DISPLAY_STEREO_STEPS)
		d->stereoHud = DISPLAY_STEREO_STEPS;
	if (d->stereoStyle > DISPLAY_STEREO_POP)
		d->stereoStyle = DISPLAY_STEREO_POP;
}

// FUN_005BE9F0. Its ATI branch (vendor 0x1002 with FUN_005BF5A0 set turns
// FSAA off) is for a card the 3DS is not.
void display_apply_level(DisplaySettings* d) {
	preset(d);
	display_clamp(d);
}

void display_first_run(DisplaySettings* d) {
	d->performanceLevel = DISPLAY_FIRST_RUN_LEVEL;
	d->b.racingRes = 0;
	display_apply_level(d);
}

static void path(char* out, int outSize, const char* savePath, const char* ext) {
	snprintf(out, outSize, "%s.display%s", savePath, ext);
}

static bool setByName(DisplaySettings* d, const char* name, u32 v) {
	for (int k = 0; k < KEY_COUNT; k++) {
		if (strcmp(kKeys[k].name, name) != 0)
			continue;
		if (kKeys[k].index < 0)
			d->performanceLevel = v;
		else
			*display_value(d, kKeys[k].index) = v;
		return true;
	}
	if (strcmp(name, KEY_STEREO) == 0) d->stereo = v;
	else if (strcmp(name, KEY_STEREO_DEPTH) == 0) d->stereoDepth = v;
	else if (strcmp(name, KEY_STEREO_FOCUS) == 0) d->stereoFocus = v;
	else if (strcmp(name, KEY_STEREO_HUD) == 0) d->stereoHud = v;
	else if (strcmp(name, KEY_STEREO_STYLE) == 0) d->stereoStyle = v;
	else return false;
	return true;
}

bool display_load(DisplaySettings* d, const char* savePath) {
	display_default(d);
	char p[320];
	path(p, sizeof(p), savePath, "");
	FILE* f = savePath && savePath[0] ? fopen(p, "r") : NULL;
	if (!f) {
		display_first_run(d);
		dlogf("display: no %s -- first-run pick, level %lu", p,
			(unsigned long)d->performanceLevel);
		return false;
	}
	char line[128];
	int read = 0, unknown = 0;
	while (fgets(line, sizeof(line), f)) {
		char* s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '#' || *s == ';' || *s == '\r' || *s == '\n' || !*s)
			continue;
		char* eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char* end = eq;
		while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		const u32 v = (u32)strtoul(eq + 1, NULL, 0);
		if (setByName(d, s, v))
			read++;
		else
			unknown++;
	}
	fclose(f);
	// FUN_005CED60: a block of another version or size is not the PC's, and
	// the game would not have read it; the defaults and the first-run pick
	// stand in.
	if (d->b.version != 1 || d->b.size != sizeof(DisplayBlock)) {
		dlogf("display: %s has VERSION %lu SIZE %lu -- first-run pick", p,
			(unsigned long)d->b.version, (unsigned long)d->b.size);
		const DisplaySettings keep = *d;
		display_default(d);
		d->stereo = keep.stereo;
		d->stereoDepth = keep.stereoDepth;
		d->stereoFocus = keep.stereoFocus;
		d->stereoHud = keep.stereoHud;
		d->stereoStyle = keep.stereoStyle;
		display_first_run(d);
		return false;
	}
	d->b.notFirstTime = 1;
	display_clamp(d);
	dlogf("display: %s -- %d value(s), %d unknown", p, read, unknown);
	return true;
}

bool display_save(const DisplaySettings* d, const char* savePath) {
	if (!savePath || !savePath[0])
		return false;
	char p[320], tmp[328];
	path(p, sizeof(p), savePath, "");
	path(tmp, sizeof(tmp), savePath, ".tmp");
	FILE* f = fopen(tmp, "w");
	if (!f) {
		dlogf("display: cannot write %s", tmp);
		return false;
	}
	DisplaySettings c = *d;
	fprintf(f, "# Need for Speed Underground 2 display settings: SPEED2.EXE's registry\n");
	fprintf(f, "# values (HKLM\\Software\\EA Games\\Need for Speed Underground 2).\n");
	for (int k = 0; k < KEY_COUNT; k++) {
		const u32 v = kKeys[k].index < 0 ? c.performanceLevel
			: *display_value(&c, kKeys[k].index);
		fprintf(f, "%s=%lu\n", kKeys[k].name, (unsigned long)v);
	}
	fprintf(f, "# 3DS stereoscopic 3D (not on the PC). Depth, focus and HUD are 0..%d.\n",
		DISPLAY_STEREO_STEPS);
	fprintf(f, "%s=%lu\n", KEY_STEREO, (unsigned long)c.stereo);
	fprintf(f, "%s=%lu\n", KEY_STEREO_DEPTH, (unsigned long)c.stereoDepth);
	fprintf(f, "%s=%lu\n", KEY_STEREO_FOCUS, (unsigned long)c.stereoFocus);
	fprintf(f, "%s=%lu\n", KEY_STEREO_HUD, (unsigned long)c.stereoHud);
	fprintf(f, "# 3-D Style: 0 into the screen, 1 popping out of it.\n");
	fprintf(f, "%s=%lu\n", KEY_STEREO_STYLE, (unsigned long)c.stereoStyle);
	const bool ok = (fflush(f) == 0);
	fclose(f);
	if (!ok) {
		remove(tmp);
		dlogf("display: write to %s failed", tmp);
		return false;
	}
	remove(p);
	if (rename(tmp, p) != 0) {
		dlogf("display: rename %s -> %s failed", tmp, p);
		return false;
	}
	dlogf("display: saved %s", p);
	return true;
}

// The debug log's names for the values from envmap to texFilter.
static const char* const kShort[DISPLAY_CAPPED] = {
	"envmap", "envUpd", "shadow", "headlight", "carLight", "carDamage",
	"crowd", "roadRefl", "fog", "mblur", "streaks", "glow", "animTex",
	"particles", "dof", "worldLod", "carLod", "overbright", "bleach",
	"tint", "fsaa", "hfog", "rain", "u64", "texFilter",
};

int display_index_named(const char* name) {
	for (int i = 0; i < DISPLAY_CAPPED; i++) {
		if (strcmp(kShort[i], name) == 0)
			return DISPLAY_FIRST_VALUE + i;
	}
	return -1;
}

void display_describe(const DisplaySettings* d, char* out, int outSize) {
	int n = snprintf(out, outSize, "level %lu, 3D %s %s depth %lu focus %lu hud %lu:",
		(unsigned long)d->performanceLevel, d->stereo ? "on" : "off",
		d->stereoStyle == DISPLAY_STEREO_POP ? "pop-out" : "into-screen",
		(unsigned long)d->stereoDepth, (unsigned long)d->stereoFocus,
		(unsigned long)d->stereoHud);
	DisplaySettings c = *d;
	for (int i = 0; i < DISPLAY_CAPPED && n > 0 && n < outSize; i++) {
		const u32 v = *display_value(&c, DISPLAY_FIRST_VALUE + i);
		if (v)
			n += snprintf(out + n, outSize - n, " %s %lu", kShort[i], (unsigned long)v);
	}
}
