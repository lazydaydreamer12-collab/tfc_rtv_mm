// rtv_config.cpp - configs/rtv.ini, and who counts as an admin.
//
// "key value" per line; blank lines and lines starting with //, # or ; are
// ignored, as is anything after a // on a line. Unknown keys are reported in
// the trace log so a typo shows up. Reloaded at every map start and by
// "rtv_reload".
//
// ADMINS: "admin STEAM_0:1:2345" lines here, plus (amxx_users 1) every SteamID
// entry in AMX Mod X's users.ini whose access flags include amxx_flag ("f",
// AMXX's map-change flag, by default).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtv_common.h"

RVConfig g_rv;


void RV_ConfigDefaults(void)
{
	memset(&g_rv, 0, sizeof(g_rv));
	g_rv.enabled        = 1;
	g_rv.debug          = 1;

	g_rv.rtvPercent     = 40.0f;
	g_rv.rtvMinPlayers  = 2;
	g_rv.includeBots    = 0;
	g_rv.mapStartDelay  = 180.0f;
	g_rv.adminInstant   = 1;
	g_rv.afkTime        = 60.0f;

	g_rv.mapHistory     = 3;
	g_rv.mapsFile[0]    = 0;
	g_rv.nominateTyped  = 1;

	g_rv.voteMaps       = 9;
	g_rv.voteDuration   = 15.0f;
	g_rv.voteCountdown  = 5.0f;
	g_rv.voteConfirm    = 1;
	g_rv.runoff         = 1;
	g_rv.runoffPercent  = 5.0f;
	g_rv.runoffDuration = 15.0f;
	g_rv.changeDelay    = 5.0f;

	g_rv.endmapVote     = 1;
	g_rv.endmapWarnTime = 300.0f;
	g_rv.endmapVoteTime = 180.0f;
	g_rv.controlNextmap = 1;

	g_rv.sounds         = 1;
	g_rv.advertInterval = 300;
	g_rv.advertFirst    = 120;
	g_rv.advertJoin     = 25;

	g_rv.amxxUsers      = 1;
	g_rv.amxxFlag       = 'f';
}

static int   ClampI(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }
static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void AddAdmin(const char *auth)
{
	if (!auth || !auth[0])
		return;
	for (int i = 0; i < g_rv.adminCount; i++)
		if (!strcasecmp(g_rv.admins[i], auth))
			return;
	if (g_rv.adminCount >= RV_MAX_ADMINS)
		return;
	strncpy(g_rv.admins[g_rv.adminCount], auth, sizeof(g_rv.admins[0]) - 1);
	g_rv.admins[g_rv.adminCount][sizeof(g_rv.admins[0]) - 1] = 0;
	g_rv.adminCount++;
}

static void ApplyKey(const char *key, const char *val, int line)
{
	int iv = atoi(val);
	float fv = (float)atof(val);
	if      (!strcasecmp(key, "enabled"))            g_rv.enabled = iv ? 1 : 0;
	else if (!strcasecmp(key, "debug"))              g_rv.debug = iv ? 1 : 0;
	else if (!strcasecmp(key, "rtv_percent"))        g_rv.rtvPercent = ClampF(fv, 1, 100);
	else if (!strcasecmp(key, "rtv_min_players"))    g_rv.rtvMinPlayers = ClampI(iv, 1, 32);
	else if (!strcasecmp(key, "rtv_include_bots"))   g_rv.includeBots = iv ? 1 : 0;
	else if (!strcasecmp(key, "rtv_mapstart_delay")) g_rv.mapStartDelay = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "rtv_admin_instant"))  g_rv.adminInstant = iv ? 1 : 0;
	else if (!strcasecmp(key, "afk_time"))           g_rv.afkTime = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "map_history"))        g_rv.mapHistory = ClampI(iv, 0, RV_MAX_BAN);
	else if (!strcasecmp(key, "map_immune") || !strcasecmp(key, "immune_maps"))
	{
		// One or more maps, separated by spaces or commas; the line can repeat.
		char buf[256];
		strncpy(buf, val, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
		for (char *t = strtok(buf, " ,\t"); t; t = strtok(NULL, " ,\t"))
		{
			if (g_rv.immuneCount >= RV_MAX_IMMUNE)
			{
				RV_Trace("Config: line %d: more than %d map_immune maps - %s left out", line, RV_MAX_IMMUNE, t);
				break;
			}
			size_t tl = strlen(t);
			if (tl > 4 && !strcasecmp(t + tl - 4, ".bsp"))
				t[tl - 4] = 0;
			strncpy(g_rv.immune[g_rv.immuneCount], t, RV_MAX_NAME - 1);
			g_rv.immune[g_rv.immuneCount][RV_MAX_NAME - 1] = 0;
			g_rv.immuneCount++;
		}
	}
	else if (!strcasecmp(key, "maps_file"))
	{
		strncpy(g_rv.mapsFile, val, sizeof(g_rv.mapsFile) - 1);
		g_rv.mapsFile[sizeof(g_rv.mapsFile) - 1] = 0;
	}
	else if (!strcasecmp(key, "nominate_typed"))     g_rv.nominateTyped = iv ? 1 : 0;
	else if (!strcasecmp(key, "vote_maps"))          g_rv.voteMaps = ClampI(iv, 2, RV_MAX_OPTIONS);
	else if (!strcasecmp(key, "vote_duration"))      g_rv.voteDuration = ClampF(fv, 5, 120);
	else if (!strcasecmp(key, "vote_countdown"))     g_rv.voteCountdown = ClampF(fv, 0, 30);
	else if (!strcasecmp(key, "vote_confirm"))       g_rv.voteConfirm = iv ? 1 : 0;
	else if (!strcasecmp(key, "runoff"))             g_rv.runoff = iv ? 1 : 0;
	else if (!strcasecmp(key, "runoff_percent"))     g_rv.runoffPercent = ClampF(fv, 0, 100);
	else if (!strcasecmp(key, "runoff_duration"))    g_rv.runoffDuration = ClampF(fv, 5, 120);
	else if (!strcasecmp(key, "change_delay"))       g_rv.changeDelay = ClampF(fv, 0, 60);
	else if (!strcasecmp(key, "endmap_vote"))        g_rv.endmapVote = iv ? 1 : 0;
	else if (!strcasecmp(key, "endmap_warn_time"))   g_rv.endmapWarnTime = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "endmap_vote_time"))   g_rv.endmapVoteTime = ClampF(fv, 30, 3600);
	else if (!strcasecmp(key, "control_nextmap"))    g_rv.controlNextmap = iv ? 1 : 0;
	else if (!strcasecmp(key, "sounds"))             g_rv.sounds = iv ? 1 : 0;
	else if (!strcasecmp(key, "panel_color") || !strcasecmp(key, "panel_alpha"))
		; // retired: the vote list is the game's menu now; players pick its colours in !rtvmenu
	else if (!strcasecmp(key, "advert_interval"))    g_rv.advertInterval = (fv <= 0) ? 0 : ClampF(fv, 30, 3600);
	else if (!strcasecmp(key, "advert_first"))       g_rv.advertFirst = ClampF(fv, 0, 3600);
	else if (!strcasecmp(key, "advert_join"))        g_rv.advertJoin = (fv <= 0) ? 0 : ClampF(fv, 1, 600);
	else if (!strcasecmp(key, "advert"))
	{
		if (g_rv.advertCount < 8 && val[0])
		{
			strncpy(g_rv.adverts[g_rv.advertCount], val, sizeof(g_rv.adverts[0]) - 1);
			g_rv.adverts[g_rv.advertCount][sizeof(g_rv.adverts[0]) - 1] = 0;
			g_rv.advertCount++;
		}
		else if (val[0])
			RV_Trace("Config: line %d: more than 8 advert lines - left out", line);
	}
	else if (!strcasecmp(key, "vote_fkeys"))         ; // retired: players bind their own keys now
	else if (!strcasecmp(key, "amxx_users"))         g_rv.amxxUsers = iv ? 1 : 0;
	else if (!strcasecmp(key, "amxx_flag"))          g_rv.amxxFlag = val[0] ? val[0] : 'f';
	else if (!strcasecmp(key, "admin"))              AddAdmin(val);
	else
		RV_Trace("Config: line %d: unknown key \"%s\" ignored", line, key);
}

// Cut a // comment off a line - but not inside "quotes", so an advert can
// hold a web address.
static void StripComment(char *line)
{
	bool q = false;
	for (char *c = line; *c; c++)
	{
		if (*c == '"')
			q = !q;
		else if (!q && c[0] == '/' && c[1] == '/')
		{
			*c = 0;
			return;
		}
	}
}

static FILE *OpenFirst(const char *rel, char *usedPath, size_t usedLen)
{
	char dir[400];
	FILE *f = NULL;
	if (RV_ModuleDir(dir, sizeof(dir)))
	{
		_snprintf_wc(usedPath, usedLen - 1, "%s/%s", dir, rel);
		usedPath[usedLen - 1] = 0;
		f = fopen(usedPath, "r");
		if (f)
			return f;
	}
	char gamedir[256];
	GET_GAME_DIR(gamedir);
	_snprintf_wc(usedPath, usedLen - 1, "%s/addons/tfc_rtv_mm/%s", gamedir, rel);
	usedPath[usedLen - 1] = 0;
	return fopen(usedPath, "r");
}

// The next "quoted" or bare field of a users.ini line.
static const char *NextField(const char *s, char *out, size_t outLen)
{
	out[0] = 0;
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s)
		return s;
	size_t n = 0;
	if (*s == '"')
	{
		s++;
		while (*s && *s != '"')
		{
			if (n + 1 < outLen)
				out[n++] = *s;
			s++;
		}
		if (*s == '"')
			s++;
	}
	else
	{
		while (*s && *s != ' ' && *s != '\t')
		{
			if (n + 1 < outLen)
				out[n++] = *s;
			s++;
		}
	}
	out[n] = 0;
	return s;
}

static void LoadAmxxUsers(void)
{
	char path[512], gamedir[256], dir[400];
	FILE *f = NULL;
	if (RV_ModuleDir(dir, sizeof(dir)))
	{
		_snprintf_wc(path, sizeof(path) - 1, "%s/../amxmodx/configs/users.ini", dir);
		path[sizeof(path) - 1] = 0;
		f = fopen(path, "r");
	}
	if (!f)
	{
		GET_GAME_DIR(gamedir);
		_snprintf_wc(path, sizeof(path) - 1, "%s/addons/amxmodx/configs/users.ini", gamedir);
		path[sizeof(path) - 1] = 0;
		f = fopen(path, "r");
	}
	if (!f)
	{
		RV_Trace("Config: amxx_users is on but users.ini was not found (last tried %s)", path);
		return;
	}
	int before = g_rv.adminCount;
	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		char *s = TrimInPlace(line);
		if (!s[0] || s[0] == ';' || (s[0] == '/' && s[1] == '/'))
			continue;
		char auth[64], pass[64], access[64], flags[64];
		const char *p = NextField(s, auth, sizeof(auth));
		p = NextField(p, pass, sizeof(pass));
		p = NextField(p, access, sizeof(access));
		NextField(p, flags, sizeof(flags));
		// 'c' = the entry is a SteamID.
		if (!auth[0] || !strchr(access, g_rv.amxxFlag) || !strchr(flags, 'c'))
			continue;
		AddAdmin(auth);
	}
	fclose(f);
	RV_Trace("Config: %d admin(s) with flag '%c' taken from %s", g_rv.adminCount - before, g_rv.amxxFlag, path);
}

void RV_ConfigLoad(void)
{
	RV_ConfigDefaults();
	char path[512];
	FILE *f = OpenFirst("configs/rtv.ini", path, sizeof(path));
	if (!f)
		RV_Trace("Config: %s not found - running on built-in defaults", path);
	else
	{
		char line[256];
		int n = 0;
		while (fgets(line, sizeof(line), f))
		{
			n++;
			StripComment(line);
			char *s = TrimInPlace(line);
			if (!s[0] || s[0] == '#' || s[0] == ';')
				continue;
			char key[64];
			char *rest = SplitFirstToken(s, key, sizeof(key));
			rest = TrimInPlace(rest);
			size_t rl = strlen(rest);
			if (rl >= 2 && rest[0] == '"' && rest[rl - 1] == '"')
			{
				rest[rl - 1] = 0;
				rest++;
			}
			ApplyKey(key, rest, n);
		}
		fclose(f);
		RV_Trace("Config: loaded %s", path);
	}
	if (g_rv.amxxUsers)
		LoadAmxxUsers();
	if (g_rv.endmapVoteTime > g_rv.endmapWarnTime && g_rv.endmapWarnTime > 0)
		g_rv.endmapWarnTime = g_rv.endmapVoteTime;
	RV_Trace("Config: rtv %.0f%% min=%d bots=%d start=%.0fs admin_instant=%d afk=%.0fs | history=%d list=%s typed=%d | vote maps=%d %.0fs countdown=%.0fs confirm=%d runoff=%d %.0f%% %.0fs change=%.0fs | endmap=%d warn=%.0fs vote=%.0fs control=%d | sounds=%d admins=%d",
		g_rv.rtvPercent, g_rv.rtvMinPlayers, g_rv.includeBots, g_rv.mapStartDelay, g_rv.adminInstant,
		g_rv.afkTime, g_rv.mapHistory, g_rv.mapsFile[0] ? g_rv.mapsFile : "(mapcyclefile)", g_rv.nominateTyped,
		g_rv.voteMaps, g_rv.voteDuration, g_rv.voteCountdown, g_rv.voteConfirm, g_rv.runoff,
		g_rv.runoffPercent, g_rv.runoffDuration, g_rv.changeDelay, g_rv.endmapVote, g_rv.endmapWarnTime,
		g_rv.endmapVoteTime, g_rv.controlNextmap, g_rv.sounds, g_rv.adminCount);
	if (g_rv.immuneCount)
	{
		char im[400] = "";
		size_t used = 0;
		for (int i = 0; i < g_rv.immuneCount; i++)
		{
			int w = _snprintf_wc(im + used, sizeof(im) - used - 1, "%s%s", i ? ", " : "", g_rv.immune[i]);
			if (w < 0 || (used += (size_t)w) >= sizeof(im) - 1)
				break;
		}
		im[sizeof(im) - 1] = 0;
		RV_Trace("Config: immune maps (never banned, don't count toward map_history): %s", im);
	}
}

// STEAM_0:1:2345 and STEAM_1:1:2345 are the same account (the first digit is
// the "universe", which differs between engine builds), so compare the rest.
static const char *AuthTail(const char *a)
{
	if (!strncasecmp(a, "STEAM_", 6) || !strncasecmp(a, "VALVE_", 6))
	{
		const char *c = strchr(a, ':');
		return c ? c : a;
	}
	return a;
}

bool RV_IsAdminAuth(const char *auth)
{
	if (!auth || !auth[0])
		return false;
	for (int i = 0; i < g_rv.adminCount; i++)
		if (!strcasecmp(AuthTail(g_rv.admins[i]), AuthTail(auth)))
			return true;
	return false;
}
