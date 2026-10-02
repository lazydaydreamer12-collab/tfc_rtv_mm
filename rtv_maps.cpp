// rtv_maps.cpp - which maps can be nominated and voted for, the recently
// played list, and what comes next in the map cycle.
//
// THE MAP LIST is maps_file from the ini, or the server's mapcyclefile
// (mapcycle.txt) when that is empty. Each line's first word is the map; the
// rest (mapcycle's "minplayers" options) and // comments are ignored, and
// maps not on the server (IS_MAP_VALID) are left out with a line in the
// trace. Files are read with the engine's LOAD_FILE_FOR_ME, which looks in
// the game folder the same way the game's own mapcycle code does.
//
// RECENTLY PLAYED: rtv_history.txt next to the DLL, newest first, updated at
// every map start. The first map_history entries - this map and the ones
// before it - cannot be nominated or voted for. Maps listed in map_immune are
// never banned and are skipped when counting, so playing one doesn't push a
// real map off the ban list. The first time, the list is
// taken over from the AMX Mod X RTV's addons/amxmodx/data/rtv_history.txt if
// there is one, so the ban carries on across the switch.

#include "rtv_common.h"

RVMap g_maps[RV_MAX_MAPS];
int   g_mapCount = 0;
char  g_curMap[RV_MAX_NAME] = "";

static char g_cycle[RV_MAX_MAPS][RV_MAX_NAME];
static int  g_cycleCount = 0;
static char g_hist[RV_MAX_HISTORY][RV_MAX_NAME];
static int  g_histCount = 0;

// First word of each usable line of a map file, into names[].
static int RV_ReadMapFile(const char *file, char names[][RV_MAX_NAME], int max, bool validate)
{
	int len = 0;
	byte *data = LOAD_FILE_FOR_ME((char *)file, &len);
	if (!data)
	{
		RV_Trace("Maps: could not read %s", file);
		return 0;
	}
	int n = 0, skipped = 0;
	const char *p = (const char *)data, *end = p + len;
	while (p < end && n < max)
	{
		char line[256];
		size_t l = 0;
		while (p < end && *p != '\n' && *p != '\r')
		{
			if (l + 1 < sizeof(line))
				line[l++] = *p;
			p++;
		}
		while (p < end && (*p == '\n' || *p == '\r'))
			p++;
		line[l] = 0;
		char *cmt = strstr(line, "//");
		if (cmt)
			*cmt = 0;
		char *s = TrimInPlace(line);
		if (!s[0] || s[0] == ';' || s[0] == '#')
			continue;
		char word[RV_MAX_NAME];
		size_t w = 0;
		if (*s == '"')
			s++;
		while (*s && *s != ' ' && *s != '\t' && *s != '"' && w + 1 < sizeof(word))
			word[w++] = *s++;
		word[w] = 0;
		size_t wl = strlen(word);
		if (wl > 4 && !strcasecmp(word + wl - 4, ".bsp"))
			word[wl - 4] = 0;
		if (!word[0])
			continue;
		bool dup = false;
		for (int i = 0; i < n; i++)
			if (!strcasecmp(names[i], word))
				dup = true;
		if (dup)
			continue;
		if (validate && !IS_MAP_VALID(word))
		{
			RV_Trace("Maps: %s is in %s but not on the server - left out", word, file);
			skipped++;
			continue;
		}
		strncpy(names[n], word, RV_MAX_NAME - 1);
		names[n][RV_MAX_NAME - 1] = 0;
		n++;
	}
	FREE_FILE(data);
	return n;
}

static bool RV_HistoryPath(char *out, size_t len)
{
	char dir[400];
	if (!RV_ModuleDir(dir, sizeof(dir)))
		return false;
	_snprintf_wc(out, len - 1, "%s/rtv_history.txt", dir);
	out[len - 1] = 0;
	return true;
}

static void RV_HistoryLoad(void)
{
	g_histCount = 0;
	char path[450];
	FILE *f = RV_HistoryPath(path, sizeof(path)) ? fopen(path, "r") : NULL;
	if (f)
	{
		char line[128];
		while (fgets(line, sizeof(line), f) && g_histCount < RV_MAX_HISTORY)
		{
			char *s = TrimInPlace(line);
			if (!s[0] || s[0] == '/' || s[0] == '#')
				continue;
			strncpy(g_hist[g_histCount], s, RV_MAX_NAME - 1);
			g_hist[g_histCount][RV_MAX_NAME - 1] = 0;
			g_histCount++;
		}
		fclose(f);
		return;
	}
	// First run: carry on from the AMX Mod X RTV's list.
	char old[RV_MAX_HISTORY][RV_MAX_NAME];
	int n = RV_ReadMapFile("addons/amxmodx/data/rtv_history.txt", old, RV_MAX_HISTORY, false);
	for (int i = 0; i < n; i++)
		strcpy(g_hist[g_histCount++], old[i]);
	if (n)
		RV_Trace("Maps: took over %d recently played maps from the AMX Mod X RTV's rtv_history.txt", n);
}

static void RV_HistorySave(void)
{
	char path[450];
	if (!RV_HistoryPath(path, sizeof(path)))
		return;
	FILE *f = fopen(path, "w");
	if (!f)
	{
		RV_Trace("Maps: could not write %s", path);
		return;
	}
	fprintf(f, "// TFC RTV - maps played, newest first (the first map_history cannot be nominated)\n");
	for (int i = 0; i < g_histCount; i++)
		fprintf(f, "%s\n", g_hist[i]);
	fclose(f);
}

void RV_MapsLoad(void)
{
	strncpy(g_curMap, STRING(gpGlobals->mapname), RV_MAX_NAME - 1);
	g_curMap[RV_MAX_NAME - 1] = 0;

	const char *cyc = CVAR_GET_STRING("mapcyclefile");
	if (!cyc || !cyc[0])
		cyc = "mapcycle.txt";
	g_cycleCount = RV_ReadMapFile(cyc, g_cycle, RV_MAX_MAPS, true);

	char tmp[RV_MAX_MAPS][RV_MAX_NAME];
	const char *listFile = g_rv.mapsFile[0] ? g_rv.mapsFile : cyc;
	int n = g_rv.mapsFile[0] ? RV_ReadMapFile(listFile, tmp, RV_MAX_MAPS, true) : 0;
	if (!g_rv.mapsFile[0])
	{
		n = g_cycleCount;
		memcpy(tmp, g_cycle, sizeof(tmp[0]) * (size_t)n);
	}
	g_mapCount = n;
	for (int i = 0; i < n; i++)
		strcpy(g_maps[i].name, tmp[i]);

	// Recently played: this map goes on top.
	RV_HistoryLoad();
	int keep = 0;
	char h[RV_MAX_HISTORY][RV_MAX_NAME];
	strcpy(h[keep++], g_curMap);
	for (int i = 0; i < g_histCount && keep < RV_MAX_HISTORY; i++)
		if (strcasecmp(g_hist[i], g_curMap))
			strcpy(h[keep++], g_hist[i]);
	memcpy(g_hist, h, sizeof(h[0]) * (size_t)keep);
	g_histCount = keep;
	RV_HistorySave();

	char hist[200];
	RV_HistoryText(hist, sizeof(hist));
	RV_Trace("Maps: %d in the list (%s), %d in the cycle (%s); recently played (not nominatable): %s",
		g_mapCount, listFile, g_cycleCount, cyc, hist);
}

int RV_FindMap(const char *name)
{
	if (!name || !name[0])
		return -1;
	for (int i = 0; i < g_mapCount; i++)
		if (!strcasecmp(g_maps[i].name, name))
			return i;
	return -1;
}

int RV_MatchMaps(const char *part, int *out, int max)
{
	int n = 0;
	if (!part || !part[0])
		return 0;
	for (int i = 0; i < g_mapCount && n < max; i++)
		if (ContainsI(g_maps[i].name, part))
			out[n++] = i;
	return n;
}

// Simple glob: * matches any run of characters, case-insensitive.
static bool RV_Glob(const char *pat, const char *name)
{
	if (!*pat)
		return !*name;
	if (*pat == '*')
	{
		for (const char *n = name; ; n++)
		{
			if (RV_Glob(pat + 1, n))
				return true;
			if (!*n)
				return false;
		}
	}
	if (!*name)
		return false;
	char a = *pat, b = *name;
	if (a >= 'A' && a <= 'Z') a += 32;
	if (b >= 'A' && b <= 'Z') b += 32;
	return a == b && RV_Glob(pat + 1, name + 1);
}

bool RV_MapImmune(const char *name)
{
	for (int i = 0; i < g_rv.immuneCount; i++)
		if (RV_Glob(g_rv.immune[i], name))
			return true;
	return false;
}

// The banned maps: the current one, then the most recent ones played, up to
// map_history in all - immune maps skipped, and not counted.
static int RV_BannedList(const char **out, int max)
{
	int n = 0;
	if (!RV_MapImmune(g_curMap) && n < max)
		out[n++] = g_curMap;
	for (int i = 0; i < g_histCount && n < g_rv.mapHistory && n < max; i++)
	{
		if (!strcasecmp(g_hist[i], g_curMap) || RV_MapImmune(g_hist[i]))
			continue;
		out[n++] = g_hist[i];
	}
	return n;
}

bool RV_MapBanned(const char *name)
{
	const char *ban[RV_MAX_BAN + 1];
	int n = RV_BannedList(ban, RV_MAX_BAN + 1);
	for (int i = 0; i < n; i++)
		if (!strcasecmp(ban[i], name))
			return true;
	return false;
}

bool RV_MapEligible(int idx)
{
	return idx >= 0 && idx < g_mapCount && !RV_MapBanned(g_maps[idx].name);
}

// The map after this one in the cycle. If this map is not in the cycle (it
// was voted in), carry on from the most recent cycle map that was played.
int RV_CycleNext(char *out, size_t len)
{
	out[0] = 0;
	if (!g_cycleCount)
		return 0;
	int at = -1;
	for (int h = 0; h < g_histCount && at < 0; h++)
		for (int i = 0; i < g_cycleCount; i++)
			if (!strcasecmp(g_cycle[i], g_hist[h]))
			{
				at = i;
				break;
			}
	int next = (at + 1) % g_cycleCount;
	strncpy(out, g_cycle[next], len - 1);
	out[len - 1] = 0;
	return 1;
}

void RV_HistoryText(char *out, size_t len)
{
	out[0] = 0;
	size_t used = 0;
	const char *ban[RV_MAX_BAN + 1];
	int n = RV_BannedList(ban, RV_MAX_BAN + 1);
	for (int i = 0; i < n; i++)
	{
		int w = _snprintf_wc(out + used, len - used - 1, "%s%s", i ? ", " : "", ban[i]);
		if (w < 0 || (used += (size_t)w) >= len - 1)
			break;
	}
	out[len - 1] = 0;
	if (!out[0])
		_snprintf_wc(out, len - 1, "(none)");
}
