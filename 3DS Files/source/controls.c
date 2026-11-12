// PORT: the remappable controls (TASKS.md #447) -- see controls.h.
#include "controls.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dbglog.h"
#include "options.h"

_Static_assert(CONTROLS_SLOTS == OPTIONS_CONTROL_BUTTONS, "one slot per CButton");

static void dlogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlogf(const char* fmt, ...) {
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line(NULL, b);
}

// The 3DS control at each of UI_OptionsController's fourteen slots, the
// port's answer to retail's controller picture (#259): slot 2 is the stick
// scheme 0 steers with, slots 3-6 the four D-pad directions the free-roam
// screens sit on, and the other nine the nine remaining buttons. Retail's pad
// has four shoulders where a 3DS has two, so two slots fall on ZL and ZR,
// which only a New 3DS carries -- the only console the port runs on (#264).
static const struct {
	u32 keys;         // the KEY_ bits, 0 for the stick (read as an axis)
	const char* name; // what the Controls page and the .controls file call it
} kSlot[CONTROLS_SLOTS] = {
	{ KEY_ZR,     "ZR"          }, //  1
	{ KEY_B,      "B"           }, //  2
	{ 0,          "Circle Pad"  }, //  3  steered, not pressed
	{ KEY_DUP,    "D-Pad Up"    }, //  4
	{ KEY_DDOWN,  "D-Pad Down"  }, //  5
	{ KEY_DLEFT,  "D-Pad Left"  }, //  6
	{ KEY_DRIGHT, "D-Pad Right" }, //  7
	{ KEY_X,      "X"           }, //  8
	{ KEY_A,      "A"           }, //  9
	{ KEY_ZL,     "ZL"          }, // 10
	{ KEY_Y,      "Y"           }, // 11
	{ KEY_L,      "L"           }, // 12
	{ KEY_R,      "R"           }, // 13
	{ KEY_SELECT, "SELECT"      }, // 14
};

// The order LEFT / RIGHT walk the buttons in: the face buttons, the
// shoulders, the D-pad, SELECT.
static const u8 kCycle[CONTROLS_ACTIONS] = { 8, 1, 7, 10, 11, 12, 9, 0, 3, 4, 5, 6, 13 };

// The thirteen actions, in the page's order, with the .controls file's name.
static const struct {
	u32 hash;
	const char* key;
} kAction[CONTROLS_ACTIONS] = {
	{ OPT_ACT_ACCELERATE,    "Accelerate"  },
	{ OPT_ACT_BRAKE,         "Brake"       },
	{ OPT_ACT_NITROUS,       "Nitrous"     },
	{ OPT_ACT_HANDBRAKE,     "Handbrake"   },
	{ OPT_ACT_SHIFT_UP,      "ShiftUp"     },
	{ OPT_ACT_SHIFT_DOWN,    "ShiftDown"   },
	{ OPT_ACT_CHANGE_CAMERA, "Camera"      },
	{ OPT_ACT_LOOK_BACK,     "LookBack"    },
	{ OPT_ACT_RESET_CAR,     "ResetCar"    },
	{ OPT_ACT_ENTER_SHOP,    "EnterShop"   },
	{ OPT_ACT_WORLD_MAP,     "WorldMap"    },
	{ OPT_ACT_CAREER_STATS,  "CareerStats" },
	{ OPT_ACT_DISPLAY_SMS,   "SMS"         },
};

// The slot each action is on, -1 for none.
static s8 s_slot[CONTROLS_ACTIONS];
static bool s_init;
// The row controls_step() last moved, and the action its last step took a
// button from (-1 none) with that button: given back if the row moves on.
static int s_stepRow = -1;
static int s_bumped = -1;
static s8 s_bumpedSlot = -1;

static int actionIndex(u32 hash) {
	for (int i = 0; i < CONTROLS_ACTIONS; i++)
		if (kAction[i].hash == hash)
			return i;
	return -1;
}

// A binding with no button on two actions (#527: an action may have none).
static bool valid(const s8 slot[CONTROLS_ACTIONS]) {
	bool used[CONTROLS_SLOTS] = { false };
	for (int i = 0; i < CONTROLS_ACTIONS; i++) {
		const int s = slot[i];
		if (s == -1)
			continue;
		if (s < 0 || s >= CONTROLS_SLOTS || s == CONTROLS_STICK_SLOT || used[s])
			return false;
		used[s] = true;
	}
	return true;
}

static void defaultInto(s8 slot[CONTROLS_ACTIONS]) {
	for (int i = 0; i < CONTROLS_ACTIONS; i++)
		slot[i] = -1;
	for (int s = 0; s < CONTROLS_SLOTS; s++) {
		const int i = actionIndex(options_control_action(s, 0, true));
		if (i >= 0)
			slot[i] = (s8)s;
	}
}

static void ensure(void) {
	if (!s_init)
		controls_default();
}

void controls_default(void) {
	defaultInto(s_slot);
	s_init = true;
	controls_step_commit();
}

void controls_clear(void) {
	for (int i = 0; i < CONTROLS_ACTIONS; i++)
		s_slot[i] = -1;
	s_init = true;
	controls_step_commit();
	dlogf("controls: Delete All Controls -- every action off its button");
}

void controls_step_commit(void) {
	s_stepRow = -1;
	s_bumped = -1;
	s_bumpedSlot = -1;
}

bool controls_is_default(void) {
	ensure();
	s8 d[CONTROLS_ACTIONS];
	defaultInto(d);
	return memcmp(d, s_slot, sizeof(d)) == 0;
}

u32 controls_slot_keys(int slot) {
	return (slot >= 0 && slot < CONTROLS_SLOTS) ? kSlot[slot].keys : 0;
}

const char* controls_slot_name(int slot) {
	return (slot >= 0 && slot < CONTROLS_SLOTS) ? kSlot[slot].name : "";
}

// The .controls file's and the log's name for slot `slot`, "None" for none.
static const char* fileName(int slot) {
	return (slot >= 0 && slot < CONTROLS_SLOTS) ? kSlot[slot].name : "None";
}

u32 controls_action_hash(int i) {
	return (i >= 0 && i < CONTROLS_ACTIONS) ? kAction[i].hash : 0;
}

u32 controls_action_at(int slot) {
	if (slot == CONTROLS_STICK_SLOT)
		return OPT_ACT_STEER;
	ensure();
	for (int i = 0; i < CONTROLS_ACTIONS; i++)
		if (s_slot[i] == slot)
			return kAction[i].hash;
	return 0;
}

int controls_slot_of(int i) {
	ensure();
	return (i >= 0 && i < CONTROLS_ACTIONS) ? s_slot[i] : -1;
}

// The stops LEFT / RIGHT walk: the thirteen buttons in kCycle's order, then
// none (#527).
#define CONTROLS_STOPS (CONTROLS_ACTIONS + 1)

void controls_step(int i, bool right) {
	if (i < 0 || i >= CONTROLS_ACTIONS)
		return;
	ensure();
	if (i != s_stepRow)
		controls_step_commit();
	s_stepRow = i;
	const s8 from = s_slot[i];
	// The button this row's last step took is the one it is leaving: back to
	// the action that had it, unless something else has since given it one.
	if (s_bumped >= 0 && s_slot[s_bumped] == -1 && from == s_bumpedSlot) {
		s_slot[s_bumped] = from;
		dlogf("controls: %s gets %s back", kAction[s_bumped].key, fileName(from));
	}
	s_bumped = -1;
	s_bumpedSlot = -1;
	int c = 0;
	while (c < CONTROLS_ACTIONS && kCycle[c] != from)
		c++;   // none (or a slot not in the cycle) is the stop past the last
	c = (c + (right ? 1 : CONTROLS_STOPS - 1)) % CONTROLS_STOPS;
	const s8 to = c < CONTROLS_ACTIONS ? (s8)kCycle[c] : -1;
	for (int j = 0; to >= 0 && j < CONTROLS_ACTIONS; j++) {
		if (j != i && s_slot[j] == to) {
			// #527: no swap -- the other action loses the button.
			s_slot[j] = -1;
			s_bumped = j;
			s_bumpedSlot = to;
			dlogf("controls: %s -> %s, taken off %s", kAction[i].key, fileName(to),
				kAction[j].key);
			break;
		}
	}
	s_slot[i] = to;
}

static void path(char* out, int cap, const char* savePath, const char* ext) {
	snprintf(out, cap, "%s.controls%s", savePath, ext);
}

// -1 for "None", -2 for a name that is no button.
static int slotByName(const char* name) {
	if (strcasecmp(name, "None") == 0 || !name[0])
		return -1;
	for (int s = 0; s < CONTROLS_SLOTS; s++)
		if (s != CONTROLS_STICK_SLOT && strcasecmp(kSlot[s].name, name) == 0)
			return s;
	return -2;
}

static char* trim(char* s) {
	while (*s == ' ' || *s == '\t')
		s++;
	char* e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = '\0';
	return s;
}

static bool loadInto(const char* savePath);

bool controls_load(const char* savePath) {
	const bool ok = loadInto(savePath);
	controls_menu_commit();
	return ok;
}

static bool loadInto(const char* savePath) {
	controls_default();
	if (!savePath || !savePath[0])
		return false;
	char p[320];
	path(p, sizeof(p), savePath, "");
	FILE* f = fopen(p, "r");
	if (!f) {
		dlogf("controls: no %s -- the default", p);
		return false;
	}
	s8 slot[CONTROLS_ACTIONS];
	for (int i = 0; i < CONTROLS_ACTIONS; i++)
		slot[i] = -1;
	bool bad = false;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char* s = trim(line);
		if (*s == '#' || *s == ';' || !*s)
			continue;
		char* eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = '\0';
		const char* name = trim(s);
		const char* btn = trim(eq + 1);
		for (int i = 0; i < CONTROLS_ACTIONS; i++) {
			if (strcasecmp(kAction[i].key, name) != 0)
				continue;
			const int s = slotByName(btn);
			bad |= s == -2;
			slot[i] = (s8)(s < 0 ? -1 : s);
		}
	}
	fclose(f);
	if (bad || !valid(slot)) {
		dlogf("controls: %s names a button that is not one, or one button twice -- the default", p);
		return false;
	}
	memcpy(s_slot, slot, sizeof(s_slot));
	char d[400];
	controls_describe(d, sizeof(d));
	dlogf("controls: %s -- %s", p, d);
	return true;
}

bool controls_save(const char* savePath) {
	if (!savePath || !savePath[0])
		return false;
	ensure();
	char p[320], tmp[328];
	path(p, sizeof(p), savePath, "");
	path(tmp, sizeof(tmp), savePath, ".tmp");
	FILE* f = fopen(tmp, "w");
	if (!f) {
		dlogf("controls: cannot write %s", tmp);
		return false;
	}
	fprintf(f, "# Need for Speed Underground 2 (3DS) controls: Action=Button, one button each.\n");
	fprintf(f, "# Buttons: A B X Y L R ZL ZR SELECT, D-Pad Up/Down/Left/Right, or None. Steer\n");
	fprintf(f, "# is the Circle Pad and pause is START. Delete this file for the default.\n");
	for (int i = 0; i < CONTROLS_ACTIONS; i++)
		fprintf(f, "%s=%s\n", kAction[i].key, fileName(s_slot[i]));
	const bool ok = fflush(f) == 0;
	fclose(f);
	if (!ok) {
		remove(tmp);
		dlogf("controls: write to %s failed", tmp);
		return false;
	}
	remove(p);
	if (rename(tmp, p) != 0) {
		dlogf("controls: rename %s -> %s failed", tmp, p);
		return false;
	}
	char d[400];
	controls_describe(d, sizeof(d));
	dlogf("controls: saved %s -- %s", p, d);
	return true;
}

// The menus read the real buttons (the user, 2026-11-09: the Controls page
// "only changes controls for driving etc. and nothing else"). The 2026-11-09
// swap, which moved the menus' jobs and prompts with the binding (#447's
// "Menus follow the remap"), is gone: these are the identity now, kept so the
// menu and world call sites read the same either way.
void controls_menu_commit(void) {
	ensure();
}

u32 controls_menu_keys(u32 phys) {
	return phys;
}

u32 controls_menu_phys(u32 logical) {
	return logical;
}

void controls_describe(char* out, int cap) {
	ensure();
	int n = 0;
	out[0] = '\0';
	for (int i = 0; i < CONTROLS_ACTIONS && n < cap; i++)
		n += snprintf(out + n, cap - n, "%s%s=%s", i ? " " : "", kAction[i].key,
			fileName(s_slot[i]));
}
