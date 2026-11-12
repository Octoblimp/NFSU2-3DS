// The PC's cheat codes on 3DS buttons (#446). See cheats.h.
#include "cheats.h"
#include "cardb.h"

#include <string.h>

// The buttons of the GameCube table's joy events, as the 3DS has them.
#define B_U  KEY_DUP
#define B_D  KEY_DDOWN
#define B_L  KEY_DLEFT
#define B_R  KEY_DRIGHT
#define B_LS KEY_L      // L1
#define B_RS KEY_R      // R1
#define B_SQ KEY_Y      // Square, the left face button

typedef struct {
	const char* pc;     // the code as typed on the PC
	const char* hashOf; // the name 0x575770 hashes for the parameter, or NULL
	int  category;      // +0x20
	u32  param;         // +0x24
	u32  keys[8];       // the GameCube EasterEggs sequence, on 3DS buttons
	bool active;        // +0x30
	bool available;     // +0x32
} CheatRec;

// SPEED2.EXE 0x7FB960 in its own order, each record's (category, param) as
// 0x575770 leaves it. Both gimmevisual records are (1, 2): the lookup finds
// gimmevisual1's first, so gimmevisual2's active byte is never read. Kept.
// The sequences are the GameCube table's for the same effect (its order puts
// the visual codes before the performance ones).
static CheatRec g_cheats[CHEAT_COUNT] = {
	{ "needperformance1",  NULL, 2, 1, { B_LS, B_RS, B_LS, B_RS, B_L, B_L, B_R, B_U } },
	{ "needperformance2",  NULL, 2, 2, { B_RS, B_RS, B_LS, B_RS, B_L, B_R, B_U, B_D } },
	{ "gimmevisual1",      NULL, 1, 2, { B_RS, B_RS, B_U, B_D, B_LS, B_LS, B_U, B_D } },
	{ "gimmevisual2",      NULL, 1, 2, { B_LS, B_RS, B_U, B_D, B_LS, B_U, B_U, B_D } },
	{ "gimmechingy",       "SPONSOR_CHINGY",      6, 0, { B_U, B_R, B_U, B_U, B_D, B_R, B_D, B_R } },
	{ "wannacapone",       "SPONSOR_CAPONE",      6, 0, { B_U, B_L, B_U, B_U, B_D, B_L, B_D, B_L } },
	{ "wantmyd3",          "SPONSOR_D3",          6, 0, { B_U, B_D, B_R, B_U, B_L, B_D, B_R, B_R } },
	{ "shinestreetbright", "SPONSOR_SHINESTREET", 6, 0, { B_U, B_D, B_L, B_U, B_L, B_U, B_R, B_L } },
	{ "davidchoeart",      "SPONSOR_DAVIDCHOE",   6, 0, { B_D, B_U, B_D, B_U, B_U, B_D, B_R, B_L } },
	{ "tunejapantuning",   "SPONSOR_JAPANTUNING", 6, 0, { B_D, B_R, B_L, B_R, B_D, B_U, B_D, B_R } },
	{ "yodogg",            "SPONSOR_SNOOP_DOGG",  6, 0, { B_U, B_U, B_D, B_U, B_D, B_U, B_D, B_L } },
	{ "opendoors",         "SPONSOR_THE_DOORS",   6, 0, { B_U, B_U, B_U, B_D, B_L, B_R, B_L, B_R } },
	{ "needmybestbuy",     "AD_BESTBUY",          7, 0, { B_U, B_D, B_U, B_D, B_D, B_U, B_R, B_L } },
	{ "goforoldspice",     "AD_OLDSPICE",         7, 0, { B_D, B_D, B_L, B_L, B_R, B_D, B_U, B_D } },
	{ "gottaedge",         "AD_EDGE",             7, 0, { B_U, B_D, B_U, B_D, B_L, B_L, B_R, B_L } },
	{ "gottahavebk",       "AD_BURGERKING",       7, 0, { B_U, B_U, B_U, B_U, B_D, B_U, B_U, B_L } },
	{ "gotmycingular",     "AD_CINGULAR",         7, 0, { B_U, B_U, B_U, B_D, B_D, B_U, B_R, B_L } },
	{ "regmybank",         NULL, 5, 2, { B_U, B_U, B_U, B_L, B_RS, B_RS, B_RS, B_D } },
	{ "regmebaby",         NULL, 5, 3, { B_D, B_U, B_D, B_L, B_LS, B_LS, B_LS, B_D } },
	{ "ordermebaby",       NULL, 5, 4, { B_L, B_L, B_R, B_SQ, B_SQ, B_R, B_LS, B_RS } },
};

static bool g_entry;
static u32 g_window[8];   // JoyHandler's ring, oldest first
static int g_count;
static unsigned g_bonus200;  // 0x86512C
static unsigned g_bonus1000; // 0x865130

// bStringHash (0x43DB50), as the car parts' and presets' names are hashed.
static u32 nameHash(const char* s) {
	u32 h = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)s; *p; p++)
		h = h * 33u + *p;
	return h;
}

void cheats_init(int region) {
	for (int i = 0; i < CHEAT_COUNT; i++) {
		CheatRec* c = &g_cheats[i];
		if (c->hashOf)
			c->param = nameHash(c->hashOf);
		c->active = false;
		c->available = true;
		// 0x575770's tail: the Best Buy vinyl's record is unavailable when
		// 0x864F24 (the build region, 0x570A30) is non-zero.
		// PORT (#455): Region=all keeps it, as the US build does.
		if (c->category == CHEAT_CAT_VINYL && c->param == nameHash("AD_BESTBUY") && region != 0 &&
				region != CARDB_REGION_ALL)
			c->available = false;
	}
	g_entry = false;
	g_count = 0;
}

void cheats_set_entry(bool on) {
	g_entry = on;
	g_count = 0;
}

// 0x579CB0. The group clear (0x576050) is skipped: every shipped record's
// group is 0, which it ignores.
static CheatFeed activate(int i, bool careerStarted, unsigned cash) {
	CheatFeed f = { -1, false, 0 };
	CheatRec* c = &g_cheats[i];
	if (!c->available)
		return f;
	f.record = i;
	f.wasActive = c->active;
	c->active = true;
	if (c->category == CHEAT_CAT_SPECIAL) {
		// 0x5760A0: the bonus is set, not added, and paid in at once while
		// no career has started and the wallet is empty.
		unsigned amount = 0;
		if (c->param == 2)
			amount = g_bonus200 = 200;
		else if (c->param == 4)
			amount = g_bonus1000 = 1000;
		if (amount && !careerStarted && cash == 0)
			f.deposit = amount;
	}
	g_count = 0;
	return f;
}

CheatFeed cheats_feed(u32 kDown, bool careerStarted, unsigned cash) {
	CheatFeed none = { -1, false, 0 };
	if (!g_entry)
		return none;
	// Every button the pad has but the two that leave the screen, in a fixed
	// order when a frame brings more than one.
	static const u32 order[] = {
		KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT, KEY_L, KEY_R, KEY_X, KEY_Y,
		KEY_B, KEY_ZL, KEY_ZR, KEY_SELECT,
	};
	CheatFeed got = none;
	for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
		if (!(kDown & order[k]))
			continue;
		if (g_count == 8) {
			memmove(g_window, g_window + 1, sizeof(u32) * 7);
			g_count = 7;
		}
		g_window[g_count++] = order[k];
		if (g_count < 8)
			continue;
		for (int i = 0; i < CHEAT_COUNT; i++) {
			if (memcmp(g_window, g_cheats[i].keys, sizeof(g_window)) == 0) {
				CheatFeed f = activate(i, careerStarted, cash);
				if (f.record >= 0) {
					cash += f.deposit;
					f.deposit += got.deposit;
					got = f;
				}
				g_count = 0;
				break;
			}
		}
	}
	return got;
}

bool cheats_lookup(int category, u32 param) {
	for (int i = 0; i < CHEAT_COUNT; i++)
		if (g_cheats[i].category == category && g_cheats[i].param == param)
			return g_cheats[i].active;
	return false;
}

bool cheats_visual_level(int level) { return cheats_lookup(CHEAT_CAT_VISUAL, (u32)level); }
bool cheats_perf_level(int level) { return cheats_lookup(CHEAT_CAT_PERF, (u32)level); }
bool cheats_sponsor_car(u32 presetHash) { return cheats_lookup(CHEAT_CAT_SPONSOR, presetHash); }
bool cheats_manufacturer_vinyl(u32 partHash) { return cheats_lookup(CHEAT_CAT_VINYL, partHash); }
bool cheats_contest_vinyls(void) { return cheats_lookup(CHEAT_CAT_SPECIAL, 3); }
bool cheats_stock_car_type(int carType) {
	return cheats_lookup(CHEAT_CAT_SPECIAL, 3) && (carType == 0x1B || carType == 0x15);
}

unsigned cheats_start_cash(void) {
	return g_bonus200 + g_bonus1000;
}

const char* cheats_pc_code(int i) {
	return i >= 0 && i < CHEAT_COUNT ? g_cheats[i].pc : "";
}
