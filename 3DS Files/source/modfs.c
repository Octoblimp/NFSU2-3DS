// modfs.c -- data mods laid over the game data (TASKS.md #320); see modfs.h.
#include "modfs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MODFS_MAX_MODS   32
#define MODFS_MAX_FILES  32768
#define MODFS_MAX_DEPTH  12
#define MODFS_PATH       512

FILE* __real_fopen(const char* path, const char* mode);
int __real_stat(const char* path, struct stat* st);

typedef struct {
	char* rel;               // folded: upper case, '/' separators, no leading '/'
	unsigned char mod;       // index into s_mod
	volatile unsigned char used;  // read through the wraps, not yet reported
} ModFile;

static char s_gameDir[256];      // the config's GameDir, no trailing '/'
static char s_gameDirFold[256];  // folded, with a leading '/'
static size_t s_gameDirFoldLen;
static char s_mod[MODFS_MAX_MODS][64];
static int s_modCount;
static int s_modFiles[MODFS_MAX_MODS];
static ModFile* s_file;
static int s_fileCount, s_fileCap;
static int* s_table;             // open addressing, -1 empty
static unsigned s_tableMask;
static volatile bool s_pending;

static unsigned hashStr(const char* s) {
	unsigned h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

// `in` folded into `out`: an optional "sdmc:" dropped, '\\' as '/', runs of
// '/' as one, upper case. Returns false when it does not fit.
static bool foldPath(const char* in, char* out, size_t cap) {
	if (strncasecmp(in, "sdmc:", 5) == 0)
		in += 5;
	size_t o = 0;
	char prev = 0;
	for (; *in; in++) {
		char c = *in == '\\' ? '/' : (char)toupper((unsigned char)*in);
		if (c == '/' && prev == '/')
			continue;
		if (o + 1 >= cap)
			return false;
		out[o++] = c;
		prev = c;
	}
	out[o] = '\0';
	return true;
}

static int lookup(const char* rel) {
	if (!s_table)
		return -1;
	for (unsigned i = hashStr(rel) & s_tableMask;; i = (i + 1) & s_tableMask) {
		const int k = s_table[i];
		if (k < 0)
			return -1;
		if (strcmp(s_file[k].rel, rel) == 0)
			return k;
	}
}

static void addFile(const char* rel, int mod) {
	if (s_fileCount >= MODFS_MAX_FILES)
		return;
	if (s_fileCount == s_fileCap) {
		const int cap = s_fileCap ? s_fileCap * 2 : 256;
		ModFile* f = realloc(s_file, (size_t)cap * sizeof(*f));
		if (!f)
			return;
		s_file = f;
		s_fileCap = cap;
	}
	char* r = strdup(rel);
	if (!r)
		return;
	s_file[s_fileCount].rel = r;
	s_file[s_fileCount].mod = (unsigned char)mod;
	s_file[s_fileCount].used = 0;
	s_fileCount++;
}

// Every file under `dir` (a real path), recorded as `prefix` + its name.
static void walk(const char* dir, const char* prefix, int mod, int depth) {
	if (depth > MODFS_MAX_DEPTH)
		return;
	DIR* d = opendir(dir);
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2])))
			continue;
		char path[MODFS_PATH], rel[MODFS_PATH], fold[MODFS_PATH];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path))
			continue;
		if ((size_t)snprintf(rel, sizeof(rel), "%s%s", prefix, e->d_name) >= sizeof(rel))
			continue;
		bool isDir;
		if (e->d_type == DT_DIR)
			isDir = true;
		else if (e->d_type == DT_REG)
			isDir = false;
		else {
			struct stat st;
			if (__real_stat(path, &st) != 0)
				continue;
			isDir = S_ISDIR(st.st_mode);
		}
		if (isDir) {
			char sub[MODFS_PATH];
			if ((size_t)snprintf(sub, sizeof(sub), "%s/", rel) < sizeof(sub))
				walk(path, sub, mod, depth + 1);
		} else if (foldPath(rel, fold, sizeof(fold))) {
			addFile(fold, mod);
		}
	}
	closedir(d);
}

static int cmpName(const void* a, const void* b) {
	return strcasecmp((const char*)a, (const char*)b);
}

static bool addMod(const char* name) {
	if (s_modCount >= MODFS_MAX_MODS || !name[0] || strlen(name) >= sizeof(s_mod[0]))
		return false;
	for (int i = 0; i < s_modCount; i++)
		if (strcasecmp(s_mod[i], name) == 0)
			return false;
	snprintf(s_mod[s_modCount++], sizeof(s_mod[0]), "%s", name);
	return true;
}

void modfs_init(const char* gameDir, const char* modsCfg, void (*log)(const char*)) {
	char b[256];
	if (!gameDir || !gameDir[0])
		return;
	snprintf(s_gameDir, sizeof(s_gameDir), "%s", gameDir);
	size_t n = strlen(s_gameDir);
	while (n > 1 && (s_gameDir[n - 1] == '/' || s_gameDir[n - 1] == '\\'))
		s_gameDir[--n] = '\0';
	char tmp[260];
	snprintf(tmp, sizeof(tmp), "/%s", s_gameDir);
	if (!foldPath(tmp, s_gameDirFold, sizeof(s_gameDirFold)))
		return;
	s_gameDirFoldLen = strlen(s_gameDirFold);

	char modsDir[MODFS_PATH];
	snprintf(modsDir, sizeof(modsDir), "sdmc:%s/mods", s_gameDir);
	if (modsCfg && (strcasecmp(modsCfg, "off") == 0 || strcmp(modsCfg, "0") == 0)) {
		if (log) log("mods: off (config Mods=off)");
		return;
	}
	if (modsCfg && modsCfg[0]) {
		// The config's list, in its order.
		char list[512];
		snprintf(list, sizeof(list), "%s", modsCfg);
		for (char* t = strtok(list, ","); t; t = strtok(NULL, ",")) {
			while (*t == ' ' || *t == '\t') t++;
			char* end = t + strlen(t);
			while (end > t && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
			char p[MODFS_PATH];
			struct stat st;
			snprintf(p, sizeof(p), "%s/%s", modsDir, t);
			if (__real_stat(p, &st) == 0 && S_ISDIR(st.st_mode))
				addMod(t);
			else if (log) {
				snprintf(b, sizeof(b), "mods: \"%s\" (config Mods=) is not a folder in %s -- skipped",
					t, modsDir);
				log(b);
			}
		}
	} else {
		// Every folder in mods/, in name order.
		DIR* d = opendir(modsDir);
		if (!d) {
			if (log) log("mods: none (no mods folder in GameDir)");
			return;
		}
		static char names[MODFS_MAX_MODS][64];
		int count = 0;
		struct dirent* e;
		while ((e = readdir(d)) != NULL && count < MODFS_MAX_MODS) {
			if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(names[0]))
				continue;
			bool isDir = e->d_type == DT_DIR;
			if (e->d_type != DT_DIR && e->d_type != DT_REG) {
				char p[MODFS_PATH];
				struct stat st;
				snprintf(p, sizeof(p), "%s/%s", modsDir, e->d_name);
				isDir = __real_stat(p, &st) == 0 && S_ISDIR(st.st_mode);
			}
			if (isDir)
				snprintf(names[count++], sizeof(names[0]), "%s", e->d_name);
		}
		closedir(d);
		qsort(names, (size_t)count, sizeof(names[0]), cmpName);
		for (int i = 0; i < count; i++)
			addMod(names[i]);
	}

	for (int m = 0; m < s_modCount; m++) {
		char dir[MODFS_PATH];
		snprintf(dir, sizeof(dir), "%s/%s", modsDir, s_mod[m]);
		walk(dir, "", m, 0);
	}
	if (s_fileCount > 0) {
		unsigned size = 64;
		while (size < (unsigned)s_fileCount * 2u)
			size <<= 1;
		s_table = malloc(size * sizeof(int));
		if (s_table) {
			memset(s_table, 0xFF, size * sizeof(int));
			s_tableMask = size - 1;
			// In mod order, so the first mod's copy of a file is the one kept.
			for (int k = 0; k < s_fileCount; k++) {
				if (lookup(s_file[k].rel) >= 0) {
					s_file[k].used = 3;  // shadowed by an earlier mod: never read
					continue;
				}
				unsigned i = hashStr(s_file[k].rel) & s_tableMask;
				while (s_table[i] >= 0)
					i = (i + 1) & s_tableMask;
				s_table[i] = k;
				s_modFiles[s_file[k].mod]++;
			}
		}
	}
	if (!log)
		return;
	if (s_modCount == 0) {
		log("mods: none (the mods folder holds no mod folders)");
		return;
	}
	int kept = 0;
	for (int m = 0; m < s_modCount; m++)
		kept += s_modFiles[m];
	snprintf(b, sizeof(b), "mods: %d mod(s), %d file(s) over the game data%s", s_modCount,
		kept, s_fileCount >= MODFS_MAX_FILES ? " (the index is full: later files ignored)" : "");
	log(b);
	for (int m = 0; m < s_modCount; m++) {
		snprintf(b, sizeof(b), "mods: %d. %s -- %d file(s)%s", m + 1, s_mod[m], s_modFiles[m],
			m == 0 && s_modCount > 1 ? " (wins where two mods hold a file)" : "");
		log(b);
		int shown = 0;
		for (int k = 0; k < s_fileCount && shown < 16; k++) {
			if (s_file[k].mod != m || s_file[k].used == 3)
				continue;
			char stock[MODFS_PATH];
			struct stat st;
			snprintf(stock, sizeof(stock), "sdmc:%s/%s", s_gameDir, s_file[k].rel);
			const bool replaces = __real_stat(stock, &st) == 0;
			snprintf(b, sizeof(b), "mods:    %s (%s)", s_file[k].rel,
				replaces ? "replaces the stock file" : "new, not in the stock data");
			log(b);
			shown++;
		}
		if (s_modFiles[m] > shown) {
			snprintf(b, sizeof(b), "mods:    ... and %d more", s_modFiles[m] - shown);
			log(b);
		}
	}
}

int modfs_file_count(void) {
	return s_fileCount;
}

// The mod copy of `path`, into `out`, when a mod holds it.
static bool redirect(const char* path, char* out, size_t cap) {
	if (!s_table || !path)
		return false;
	char fold[MODFS_PATH];
	if (!foldPath(path, fold, sizeof(fold)))
		return false;
	if (fold[0] != '/')
		return false;
	if (strncmp(fold, s_gameDirFold, s_gameDirFoldLen) != 0 || fold[s_gameDirFoldLen] != '/')
		return false;
	const char* rel = fold + s_gameDirFoldLen + 1;
	if (strncmp(rel, "MODS/", 5) == 0)
		return false;
	const int k = lookup(rel);
	if (k < 0)
		return false;
	if ((size_t)snprintf(out, cap, "sdmc:%s/mods/%s/%s", s_gameDir, s_mod[s_file[k].mod], rel) >= cap)
		return false;
	if (!s_file[k].used) {
		s_file[k].used = 1;
		s_pending = true;
	}
	return true;
}

FILE* __wrap_fopen(const char* path, const char* mode) {
	if (s_table && mode && mode[0] == 'r' && !strchr(mode, '+')) {
		char alt[MODFS_PATH];
		if (redirect(path, alt, sizeof(alt))) {
			FILE* f = __real_fopen(alt, mode);
			if (f)
				return f;
		}
	}
	return __real_fopen(path, mode);
}

int __wrap_stat(const char* path, struct stat* st) {
	if (s_table) {
		char alt[MODFS_PATH];
		if (redirect(path, alt, sizeof(alt)) && __real_stat(alt, st) == 0)
			return 0;
	}
	return __real_stat(path, st);
}

void modfs_report(void (*log)(const char*)) {
	if (!s_pending || !log)
		return;
	s_pending = false;
	for (int k = 0; k < s_fileCount; k++) {
		if (s_file[k].used != 1)
			continue;
		s_file[k].used = 2;
		char b[MODFS_PATH + 64];
		snprintf(b, sizeof(b), "mods: %s read from %s", s_file[k].rel, s_mod[s_file[k].mod]);
		log(b);
	}
}
