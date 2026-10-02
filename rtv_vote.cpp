// rtv_vote.cpp - players and AFK, rock the vote, nominations, the map vote
// (countdown, vote, runoff, result) and the end-of-map timer.

#include <stdarg.h>

#include "rtv_common.h"

RVPlayer g_pl[RV_MAX_PLAYERS + 1];

// ---------------------------------------------------------------------------
// Players
edict_t *RV_Player(int idx)
{
	if (idx < 1 || idx > gpGlobals->maxClients || idx > RV_MAX_PLAYERS)
		return NULL;
	edict_t *e = INDEXENT(idx);
	if (FNullEnt(e) || e->free || !(e->v.flags & FL_CLIENT))
		return NULL;
	if (!e->v.netname || !STRING(e->v.netname)[0])
		return NULL;
	return e;
}

// BOTS. FL_FAKECLIENT is not enough on its own: TFC's CBasePlayer::Spawn
// (tfc.so 0xc65d0) does "pev->flags &= FL_PROXY; pev->flags |= FL_CLIENT" on
// every respawn, which wipes FL_FAKECLIENT until the bot plugin sets it again.
// The first live run showed foxbot bots standing still being taken for AFK
// players because of it. The engine's own answer - the auth ID of a fake
// client is "BOT" - does not change, so that is checked too.
bool RV_IsBot(edict_t *p)
{
	if (FNullEnt(p))
		return false;
	if (p->v.flags & FL_FAKECLIENT)
		return true;
	const char *auth = GETPLAYERAUTHID(p);
	return auth && !strcmp(auth, "BOT");
}
bool RV_IsHLTV(edict_t *p) { return !FNullEnt(p) && (p->v.flags & FL_PROXY) != 0; }

bool RV_IsAdmin(edict_t *p)
{
	if (FNullEnt(p) || RV_IsBot(p))
		return false;
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > RV_MAX_PLAYERS)
		return false;
	RVPlayer &s = g_pl[idx];
	if (s.adminKnown)
		return s.admin;
	if (!IS_DEDICATED_SERVER() && idx == 1)
	{
		// The host of a listen server.
		s.adminKnown = s.admin = true;
		return true;
	}
	const char *auth = GETPLAYERAUTHID(p);
	if (!auth || !auth[0] || !strcasecmp(auth, "STEAM_ID_PENDING"))
		return false; // not known yet - ask again later
	s.admin = RV_IsAdminAuth(auth);
	s.adminKnown = true;
	return s.admin;
}

static void RV_ResetSlot(int i)
{
	memset(&g_pl[i], 0, sizeof(g_pl[i]));
	g_pl[i].nominated = -1;
	g_pl[i].voted = -1;
}

// Who the RTV total is made of: people here, not AFK. Spectators count.
static bool RV_Counts(int i, edict_t *e)
{
	if (!e || RV_IsHLTV(e))
		return false;
	if (RV_IsBot(e))
		return g_rv.includeBots != 0;
	return !g_pl[i].afk;
}

// ---------------------------------------------------------------------------
// State
enum { PH_IDLE, PH_COUNTDOWN, PH_VOTING, PH_CHANGING };
enum { VK_RTV, VK_ENDMAP };

static int   g_phase = PH_IDLE;
static int   g_kind = VK_RTV;
static float g_phaseEnd = 0;
static int   g_lastTick = -1;
static int   g_serial = 0;
static bool  g_runoffRound = false;

static int   g_opt[RV_MAX_OPTIONS];       // map index per option
static int   g_optNoms[RV_MAX_OPTIONS];
static int   g_optCount = 0;
static int   g_votes[RV_MAX_OPTIONS];

static char  g_next[RV_MAX_NAME] = "";
static char  g_nextHow[64] = "";
static bool  g_nextDecided = false;
static float g_mapStart = 0;
static bool  g_warned = false, g_endmapStarted = false;
static float g_nextHud = 0, g_nextReshow = 0;

bool RV_NextMapDecided(char *out, size_t len)
{
	if (!g_nextDecided || !g_next[0])
		return false;
	strncpy(out, g_next, len - 1);
	out[len - 1] = 0;
	return true;
}

static float RV_TimeLeft(void)
{
	float tl = CVAR_GET_FLOAT("mp_timelimit");
	if (tl <= 0)
		return -1;
	// CHalfLifeMultiplay::Think (tfc.so 0x8ccb0) ends the map when
	// gpGlobals->time reaches mp_timelimit * 60.
	float left = tl * 60.0f - gpGlobals->time;
	return left < 0 ? 0 : left;
}

static void RV_TimeText(float secs, char *out, size_t len)
{
	int s = (int)(secs + 0.5f);
	_snprintf_wc(out, len - 1, "%d:%02d", s / 60, s % 60);
	out[len - 1] = 0;
}

// ---------------------------------------------------------------------------
// Rock the vote
static int RV_PoolSize(void)
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
		if (RV_Counts(i, RV_Player(i)))
			n++;
	return n;
}

static int RV_RtvCount(void)
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (RV_Counts(i, e) && g_pl[i].rtv)
			n++;
	}
	return n;
}

static int RV_Needed(int pool)
{
	int need = (int)ceilf(pool * g_rv.rtvPercent / 100.0f - 0.0001f);
	return need < 1 ? 1 : need;
}

static void RV_ClearRtv(void)
{
	for (int i = 1; i <= RV_MAX_PLAYERS; i++)
	{
		g_pl[i].rtv = false;
		g_pl[i].rtvLostToAfk = false;
	}
}

static void RV_StartVote(int kind, const char *why);
static bool RV_UsesKeys(int i);
static void RV_PrefsLoad(void);
static bool RV_Confirm(int i);
enum { MC_TITLE = 0, MC_NUM, MC_NAME, MC_COUNT, MC_MARK, MC_HINT, MC_PARTS };
static const char  g_mcDefault[MC_PARTS] = { 'y', 'r', 'w', 'y', 'y', 'd' };
static const char *g_mcPart[MC_PARTS] = { "Title", "Numbers", "Map names", "Vote counts", "Your vote", "Hints" };
static char RV_MCol(int i, int part);       // menu colour code (w y r d) of one part
static bool RV_ListMenu(int i);             // the vote shows as a read-only list (results), not to pick from
static bool RV_SettingsKey(edict_t *p, int key);
static void RV_SettingsFrame(float now);
static void RV_ShowBindWindow(edict_t *p);
static int  ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void RV_Decide(const char *map, const char *how);

// Passed? Called whenever the count or the total may have changed.
static void RV_CheckRtv(const char *cause)
{
	if (!g_rv.enabled || g_phase != PH_IDLE)
		return;
	int pool = RV_PoolSize(), have = RV_RtvCount();
	if (have <= 0 || pool < g_rv.rtvMinPlayers)
		return;
	int need = RV_Needed(pool);
	if (have < need)
		return;
	RV_Trace("RTV: passed %d/%d (players here %d)%s%s", have, need, pool, cause ? " - " : "", cause ? cause : "");
	RV_SayAll("%s Rock the vote passed (%d of %d)!", RV_TAG, have, need);
	RV_ClearRtv();
	if (g_nextDecided)
	{
		// The end-of-map vote already chose - go there now.
		g_kind = VK_RTV;
		RV_Decide(g_next, "chosen earlier");
		return;
	}
	RV_StartVote(VK_RTV, "rock the vote");
}

static void RV_Rtv(edict_t *p)
{
	int i = ENTINDEX(p);
	if (!g_rv.enabled || RV_IsBot(p))
		return;
	if (g_phase == PH_CHANGING)
	{
		RV_Say(p, "%s The map is changing to %s.", RV_TAG, g_next);
		return;
	}
	if (g_phase != PH_IDLE)
	{
		RV_Say(p, "%s A map vote is already under way.", RV_TAG);
		return;
	}
	if (RV_IsAdmin(p) && g_rv.adminInstant)
	{
		RV_SayAll("%s Admin %s rocked the vote.", RV_TAG, STRING(p->v.netname));
		RV_Trace("RTV: admin %s - instant", STRING(p->v.netname));
		RV_ClearRtv();
		if (g_nextDecided)
			RV_Decide(g_next, "chosen earlier");
		else
			RV_StartVote(VK_RTV, "admin rtv");
		return;
	}
	float since = gpGlobals->time - g_mapStart;
	if (since < g_rv.mapStartDelay)
	{
		RV_Say(p, "%s Rock the vote opens %d seconds into the map (%d to go).", RV_TAG,
			(int)g_rv.mapStartDelay, (int)(g_rv.mapStartDelay - since + 0.5f));
		return;
	}
	int pool = RV_PoolSize();
	if (pool < g_rv.rtvMinPlayers)
	{
		RV_Say(p, "%s Rock the vote needs at least %d players here.", RV_TAG, g_rv.rtvMinPlayers);
		return;
	}
	if (g_pl[i].rtv)
	{
		RV_Say(p, "%s You already rocked the vote (%d of %d needed).", RV_TAG, RV_RtvCount(), RV_Needed(pool));
		return;
	}
	g_pl[i].rtv = true;
	g_pl[i].rtvLostToAfk = false;
	int have = RV_RtvCount(), need = RV_Needed(pool);
	RV_Trace("RTV: %s (%d/%d, players here %d)", STRING(p->v.netname), have, need, pool);
	if (have < need)
		RV_SayAll("%s %s wants to rock the vote (%d of %d). Say rtv to join in.", RV_TAG,
			STRING(p->v.netname), have, need);
	RV_CheckRtv(NULL);
}

static void RV_UnRtv(edict_t *p)
{
	int i = ENTINDEX(p);
	if (!g_pl[i].rtv)
	{
		RV_Say(p, "%s You haven't rocked the vote.", RV_TAG);
		return;
	}
	g_pl[i].rtv = false;
	int pool = RV_PoolSize();
	RV_SayAll("%s %s took back their rock the vote (%d of %d).", RV_TAG, STRING(p->v.netname),
		RV_RtvCount(), RV_Needed(pool));
}

// Append to a text buffer, never past its end.
static void RV_Append(char *buf, size_t size, size_t *used, const char *fmt, ...)
{
	if (*used >= size - 1)
		return;
	va_list ap;
	va_start(ap, fmt);
	int w = vsnprintf(buf + *used, size - *used, fmt, ap);
	va_end(ap);
	if (w > 0)
		*used += (size_t)w;
	if (*used > size - 1)
		*used = size - 1;
	buf[*used] = 0;
}

// ---------------------------------------------------------------------------
// Nominations
static int RV_Noms(int map)
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
		if (RV_Player(i) && g_pl[i].nominated == map)
			n++;
	return n;
}

// Someone else (not slot `except`) who nominated this map, or NULL.
static edict_t *RV_OtherNominator(int map, int except)
{
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (e && i != except && g_pl[i].nominated == map)
			return e;
	}
	return NULL;
}

// A nominator left. Before a vote, their nomination goes with them - unless
// someone else nominated the same map, then it stays. Once a vote has started
// the ballot is already fixed (RV_StartVote), so nothing changes then.
static void RV_DropNomination(int i, const char *name)
{
	int m = g_pl[i].nominated;
	g_pl[i].nominated = -1;
	if (m < 0 || m >= g_mapCount)
		return;
	if (g_phase == PH_COUNTDOWN || g_phase == PH_VOTING)
	{
		RV_Trace("Nominate: %s left during the vote - %s stays on the ballot", name ? name : "?", g_maps[m].name);
		return;
	}
	if (g_nextDecided)
		return;
	int n = RV_Noms(m);
	if (n > 0)
		RV_SayAll("%s %s left - %s stays nominated (%d nomination%s).", RV_TAG, name ? name : "Someone",
			g_maps[m].name, n, n == 1 ? "" : "s");
	else
		RV_SayAll("%s %s left - %s is no longer nominated.", RV_TAG, name ? name : "Someone", g_maps[m].name);
	RV_Trace("Nominate: %s left - %s now %d", name ? name : "?", g_maps[m].name, n);
}

static void RV_NominateList(edict_t *p)
{
	char buf[1500];
	size_t used = 0;
	#define ADD(...) RV_Append(buf, sizeof(buf), &used, __VA_ARGS__)
	buf[0] = 0;
	if ((g_phase == PH_COUNTDOWN || g_phase == PH_VOTING) && g_optCount > 0)
	{
		// The ballot is fixed: show it, nominations as they were when it started.
		ADD("THE MAP VOTE IS UNDER WAY\n\nNominations are locked in. The maps in this vote:\n\n");
		for (int k = 0; k < g_optCount; k++)
		{
			if (g_optNoms[k] && !g_runoffRound)
				ADD("%d. %s   (%d nomination%s)\n", k + 1, g_maps[g_opt[k]].name, g_optNoms[k], g_optNoms[k] == 1 ? "" : "s");
			else
				ADD("%d. %s\n", k + 1, g_maps[g_opt[k]].name);
		}
		ADD("\nSay !revote to open the vote menu.\n");
		#undef ADD
		RV_ShowWindow(p, buf);
		return;
	}
	#define ADD(...) RV_Append(buf, sizeof(buf), &used, __VA_ARGS__)
	ADD("MAPS YOU CAN NOMINATE\n\nSay  nominate <map>  (or just the map's name).\n\n");
	int shown = 0;
	for (int i = 0; i < g_mapCount; i++)
	{
		if (!RV_MapEligible(i))
			continue;
		int n = RV_Noms(i);
		if (n)
			ADD("%s   (%d nomination%s)\n", g_maps[i].name, n, n == 1 ? "" : "s");
		else
			ADD("%s\n", g_maps[i].name);
		shown++;
	}
	if (!shown)
		ADD("(none - every map on the list was played recently)\n");
	char hist[600];
	RV_HistoryText(hist, sizeof(hist));
	ADD("\nPlayed recently, can't be nominated:\n%s\n", hist);
	if (g_rv.immuneCount)
	{
		bool first = true;
		for (int i = 0; i < g_mapCount; i++)
			if (RV_MapImmune(g_maps[i].name))
			{
				if (first)
					ADD("\nAlways open, even when played recently:\n");
				ADD("%s%s", first ? "" : ", ", g_maps[i].name);
				first = false;
			}
		if (!first)
			ADD("\n");
	}
	char nx[RV_MAX_NAME];
	if (RV_NextMapDecided(nx, sizeof(nx)))
		ADD("\nNext map (already voted): %s\n", nx);
	#undef ADD
	RV_ShowWindow(p, buf);
}

static void RV_Nominate(edict_t *p, const char *arg, bool typed)
{
	int i = ENTINDEX(p);
	if (!arg || !arg[0] || !strcasecmp(arg, "list") || !strcasecmp(arg, "maps"))
	{
		RV_NominateList(p);
		return;
	}
	if (g_phase == PH_COUNTDOWN || g_phase == PH_VOTING)
	{
		RV_Say(p, "%s The vote is under way - nominations are locked in. !nominate list shows the maps in it.", RV_TAG);
		return;
	}
	if (g_nextDecided)
	{
		RV_Say(p, "%s The next map is already chosen: %s.", RV_TAG, g_next);
		return;
	}
	int m = RV_FindMap(arg);
	if (m < 0 && !typed)
	{
		int found[8];
		int n = RV_MatchMaps(arg, found, 8);
		if (n == 1)
			m = found[0];
		else if (n > 1)
		{
			char list[160] = "";
			size_t used = 0;
			for (int k = 0; k < n; k++)
			{
				int w = _snprintf_wc(list + used, sizeof(list) - used - 1, "%s%s", k ? ", " : "", g_maps[found[k]].name);
				if (w < 0 || (used += (size_t)w) >= sizeof(list) - 1)
					break;
			}
			list[sizeof(list) - 1] = 0;
			RV_Say(p, "%s \"%s\" could be: %s", RV_TAG, arg, list);
			return;
		}
	}
	if (m < 0)
	{
		if (!typed)
			RV_Say(p, "%s There is no map \"%s\" on the list. Say !nominate list to see them.", RV_TAG, arg);
		return;
	}
	if (RV_MapBanned(g_maps[m].name))
	{
		char hist[200];
		RV_HistoryText(hist, sizeof(hist));
		RV_Say(p, "%s %s was played recently and can't be nominated (recent: %s).", RV_TAG, g_maps[m].name, hist);
		return;
	}
	if (g_pl[i].nominated == m)
	{
		RV_Say(p, "%s You already nominated %s.", RV_TAG, g_maps[m].name);
		return;
	}
	int was = g_pl[i].nominated;
	edict_t *other = RV_OtherNominator(m, i);
	g_pl[i].nominated = m;
	int n = RV_Noms(m);
	if (other)
	{
		// Already in: this nomination backs it, so it stays if the first one leaves.
		char first[32];
		strncpy(first, STRING(other->v.netname), sizeof(first) - 1);
		first[sizeof(first) - 1] = 0;
		RV_Say(p, "%s %s was already nominated by %s - yours is added, so it stays in even if they leave.",
			RV_TAG, g_maps[m].name, first);
		RV_SayAll("%s %s also nominated %s (%d nominations).", RV_TAG, STRING(p->v.netname), g_maps[m].name, n);
	}
	else
		RV_SayAll("%s %s nominated %s (%d nomination%s).", RV_TAG, STRING(p->v.netname), g_maps[m].name, n, n == 1 ? "" : "s");
	if (was >= 0 && was < g_mapCount && was != m && RV_Noms(was) == 0)
		RV_Say(p, "%s Your nomination moved from %s, which has no nominations now.", RV_TAG, g_maps[was].name);
	RV_Trace("Nominate: %s -> %s (%d)%s%s", STRING(p->v.netname), g_maps[m].name, n,
		was >= 0 && was != m ? ", was " : "", was >= 0 && was != m && was < g_mapCount ? g_maps[was].name : "");
}

// ---------------------------------------------------------------------------
// The vote
static void RV_BuildBallot(void)
{
	g_optCount = 0;
	int max = g_rv.voteMaps;
	// Nominated maps, most nominations first.
	int noms[RV_MAX_MAPS];
	for (int m = 0; m < g_mapCount; m++)
		noms[m] = RV_MapEligible(m) ? RV_Noms(m) : 0;
	while (g_optCount < max)
	{
		int best = -1;
		for (int m = 0; m < g_mapCount; m++)
			if (noms[m] > 0 && (best < 0 || noms[m] > noms[best]))
				best = m;
		if (best < 0)
			break;
		g_optNoms[g_optCount] = noms[best];
		g_opt[g_optCount++] = best;
		noms[best] = 0;
	}
	// Fill up with random maps from the list.
	int pool[RV_MAX_MAPS], n = 0;
	for (int m = 0; m < g_mapCount; m++)
	{
		if (!RV_MapEligible(m))
			continue;
		bool in = false;
		for (int k = 0; k < g_optCount; k++)
			if (g_opt[k] == m)
				in = true;
		if (!in)
			pool[n++] = m;
	}
	while (g_optCount < max && n > 0)
	{
		int r = RANDOM_LONG(0, n - 1);
		g_optNoms[g_optCount] = 0;
		g_opt[g_optCount++] = pool[r];
		pool[r] = pool[--n];
	}
}

static void RV_CloseMenu(edict_t *e)
{
	RV_ShowMenu(e, 0, 0, " ");
}

// The vote menu for one player, in their menu colours. Number-menu players
// pick from it. Vote-key players (with the game-menu list) only read it: the
// maps, the counts and their own vote; a number key they press is a weapon
// key and is passed on (see RV_MenuSelect).
static char g_menuSent[RV_MAX_PLAYERS + 1][512];

static void RV_VoteMenuText(int i, char *buf, size_t len, int *keysOut)
{
	RVPlayer &s = g_pl[i];
	char t = RV_MCol(i, MC_TITLE), n = RV_MCol(i, MC_NUM), m = RV_MCol(i, MC_NAME), c = RV_MCol(i, MC_COUNT);
	char y = RV_MCol(i, MC_MARK), h = RV_MCol(i, MC_HINT);
	size_t used = 0;
	buf[0] = 0;
	#define ADD(...) RV_Append(buf, len, &used, __VA_ARGS__)
	const char *title = g_runoffRound ? "Runoff - choose one" : "Choose the next map";
	if (RV_ListMenu(i))
	{
		// Read-only: the maps, the counts, their own vote. A number key
		// pressed on it is a weapon key and is passed on (see RV_MenuSelect).
		bool keys = RV_UsesKeys(i);
		ADD("\\%c%s\\w\n", t, keys ? title : (g_runoffRound ? "Runoff - votes so far" : "Map vote - votes so far"));
		if (keys)
			ADD("\\%cVote keys: vote1-vote%d, vote0 = no vote\\w\n\n", h, g_optCount);
		else
			ADD("\\%cSay !revote to change your vote\\w\n\n", h);
		for (int k = 0; k < g_optCount; k++)
		{
			ADD("\\%c%d.\\%c %s   \\%c%d\\w", n, k + 1, m, g_maps[g_opt[k]].name, c, g_votes[k]);
			if (s.voted == k)
				ADD("  \\%c<- your vote\\w", y);
			ADD("\n");
		}
		if (keys)
		{
			ADD("\n\\%c0.\\%c No vote\\w", n, m);
			if (s.voted < 0 && s.declined)
				ADD("  \\%c<- you\\w", y);
		}
		else
			ADD("\n\\%c0.\\%c Hide this list\\w", n, m);
		#undef ADD
		*keysOut = 0x3FF; // every number key: weapon keys, passed on (0 hides it for menu voters)
		return;
	}
	#define ADD(...) RV_Append(buf, len, &used, __VA_ARGS__)
	float left = g_phaseEnd - gpGlobals->time;
	if (left < 1)
		left = 1;
	ADD("\\%c%s\\w  \\%c(%d s)\\w\n", t, title, h, (int)(left + 0.5f));
	if (RV_Confirm(i))
		ADD("\\%cPress a number twice to vote\\w\n", h);
	ADD("\n");
	int keys = 1 << 9;
	for (int k = 0; k < g_optCount; k++)
	{
		keys |= 1 << k;
		const char *name = g_maps[g_opt[k]].name;
		if (s.pendingKey == k + 1)
			ADD("\\%c%d.\\%c %s  <- press %d again", n, k + 1, y, name, k + 1);
		else if (s.voted == k)
			ADD("\\%c%d.\\%c %s  (your vote)", n, k + 1, y, name);
		else if (g_optNoms[k] && !g_runoffRound)
			ADD("\\%c%d.\\%c %s  \\%c(%d nom%s)", n, k + 1, m, name, c, g_optNoms[k], g_optNoms[k] == 1 ? "" : "s");
		else
			ADD("\\%c%d.\\%c %s", n, k + 1, m, name);
		ADD("\\w\n");
	}
	ADD("\n\\%c0.\\%c %s", n, m, s.voted >= 0 ? "Close" : "Don't vote");
	#undef ADD
	*keysOut = keys;
}

static void RV_SendVoteMenu(int i)
{
	edict_t *e = RV_Player(i);
	if (!e || RV_IsBot(e) || RV_IsHLTV(e))
		return;
	RVPlayer &s = g_pl[i];
	char buf[512];
	int keys = 0;
	RV_VoteMenuText(i, buf, sizeof(buf), &keys);
	int secs = (int)(g_phaseEnd - gpGlobals->time + 1.5f);
	if (secs < 1)
		secs = 1;
	if (secs > 120)
		secs = 120;
	RV_ShowMenu(e, keys, secs, buf);
	strcpy(g_menuSent[i], buf);
	s.menuShown = g_serial;
	s.setPage = 0; // the vote menu replaced the settings menu
	s.passHidden = false;
}

// The read-only lists are re-sent when what they show changes (a vote came in).
static void RV_UpdateLists(void)
{
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (!e || RV_IsBot(e) || RV_IsHLTV(e))
			continue;
		RVPlayer &s = g_pl[i];
		if (s.menuShown != g_serial || s.setPage || !RV_ListMenu(i))
			continue;
		char mt[512];
		int k;
		RV_VoteMenuText(i, mt, sizeof(mt), &k);
		if (strcmp(mt, g_menuSent[i]))
			RV_SendVoteMenu(i);
	}
}

static void RV_ClearLists(void)
{
	for (int i = 0; i <= RV_MAX_PLAYERS; i++)
		g_menuSent[i][0] = 0;
}

static void RV_BeginVoting(float duration)
{
	RV_ClearLists();
	g_phase = PH_VOTING;
	g_phaseEnd = gpGlobals->time + duration;
	g_serial++;
	memset(g_votes, 0, sizeof(g_votes));
	for (int i = 1; i <= RV_MAX_PLAYERS; i++)
	{
		g_pl[i].voted = -1;
		g_pl[i].pendingKey = 0;
		g_pl[i].declined = false;
		g_pl[i].menuShown = 0;
		g_pl[i].menuOnce = false;
		g_pl[i].listHidden = false;
		g_pl[i].passHidden = false;
	}
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		RV_SendVoteMenu(i);
		edict_t *e = RV_Player(i);
		if (e && !RV_IsBot(e) && RV_UsesKeys(i))
			RV_Say(e, "%s Vote with your keys: vote1-vote%d, vote0 = no vote. Number keys still switch weapons. !rtvmenu for your settings.",
				RV_TAG, g_optCount);
	}
	g_nextReshow = gpGlobals->time + 3.0f;
	g_nextHud = 0;
	RV_Sound(NULL, "buttons/bell1");
	char list[200] = "";
	size_t used = 0;
	for (int k = 0; k < g_optCount; k++)
	{
		int w = _snprintf_wc(list + used, sizeof(list) - used - 1, "%s%s", k ? ", " : "", g_maps[g_opt[k]].name);
		if (w < 0 || (used += (size_t)w) >= sizeof(list) - 1)
			break;
	}
	list[sizeof(list) - 1] = 0;
	RV_Trace("Vote%s: open for %.0fs: %s", g_runoffRound ? " (runoff)" : "", duration, list);
}

static void RV_StartVote(int kind, const char *why)
{
	g_runoffRound = false;
	RV_BuildBallot();
	g_kind = kind;
	if (g_optCount == 0)
	{
		RV_SayAll("%s No maps to vote for - every map on the list was played recently.", RV_TAG);
		RV_Trace("Vote: nothing to vote for (%s)", why);
		if (kind == VK_ENDMAP)
			g_endmapStarted = true;
		return;
	}
	if (g_optCount == 1)
	{
		RV_SayAll("%s Only one map is available.", RV_TAG);
		RV_Decide(g_maps[g_opt[0]].name, "the only map available");
		return;
	}
	if (kind == VK_ENDMAP)
		g_endmapStarted = true;
	RV_Trace("Vote: starting (%s)", why);
	// From here the ballot is fixed: nobody leaving takes a map off it.
	char locked[150] = "";
	size_t lu = 0;
	for (int k = 0; k < g_optCount && g_optNoms[k] > 0; k++)
	{
		int w = _snprintf_wc(locked + lu, sizeof(locked) - lu - 1, "%s%s (%d)", k ? ", " : "", g_maps[g_opt[k]].name, g_optNoms[k]);
		if (w < 0 || (lu += (size_t)w) >= sizeof(locked) - 1)
			break;
	}
	locked[sizeof(locked) - 1] = 0;
	if (locked[0])
	{
		RV_SayAll("%s Nominations are locked in: %s.", RV_TAG, locked);
		RV_Trace("Vote: nominations locked: %s", locked);
	}
	if (g_rv.voteCountdown >= 1.0f)
	{
		g_phase = PH_COUNTDOWN;
		g_phaseEnd = gpGlobals->time + g_rv.voteCountdown;
		g_lastTick = -1;
		RV_SayAll("%s Map vote in %d seconds!", RV_TAG, (int)g_rv.voteCountdown);
	}
	else
		RV_BeginVoting(g_rv.voteDuration);
}

static void RV_Cancel(const char *who)
{
	if (g_phase == PH_IDLE)
		return;
	RV_ClearLists();
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (e && g_phase == PH_VOTING && g_pl[i].menuShown == g_serial)
			RV_CloseMenu(e);
	}
	if (g_phase == PH_CHANGING)
	{
		g_nextDecided = false;
		g_next[0] = 0;
	}
	g_phase = PH_IDLE;
	RV_SayAll("%s The map vote was cancelled%s%s.", RV_TAG, who ? " by " : "", who ? who : "");
	RV_Trace("Vote: cancelled (%s)", who ? who : "?");
}

// The winner is known.
static void RV_Decide(const char *map, const char *how)
{
	strncpy(g_next, map, RV_MAX_NAME - 1);
	g_next[RV_MAX_NAME - 1] = 0;
	strncpy(g_nextHow, how, sizeof(g_nextHow) - 1);
	g_nextHow[sizeof(g_nextHow) - 1] = 0;
	g_nextDecided = true;
	RV_SetAmxxNextmap(g_next);
	for (int i = 1; i <= RV_MAX_PLAYERS; i++)
		g_pl[i].nominated = -1;
	RV_Trace("Vote: next map %s (%s)%s", g_next, how, g_kind == VK_RTV ? " - changing now" : " - at the end of the map");
	if (g_kind == VK_RTV)
	{
		g_phase = PH_CHANGING;
		g_phaseEnd = gpGlobals->time + g_rv.changeDelay;
		g_lastTick = -1;
		RV_SayAll("%s Next map: %s (%s). Changing in %d seconds.", RV_TAG, g_next, how, (int)(g_rv.changeDelay + 0.5f));
	}
	else
	{
		g_phase = PH_IDLE;
		char left[16];
		RV_TimeText(RV_TimeLeft(), left, sizeof(left));
		RV_SayAll("%s Next map: %s (%s). It starts when this map ends (%s left).", RV_TAG, g_next, how, left);
	}
}

static void RV_EndVoting(void)
{
	RV_ClearLists();
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (e && g_pl[i].menuShown == g_serial)
			RV_CloseMenu(e);
	}
	int total = 0;
	for (int k = 0; k < g_optCount; k++)
		total += g_votes[k];
	// Order the options by votes, ties at random.
	int order[RV_MAX_OPTIONS];
	int rnd[RV_MAX_OPTIONS];
	for (int k = 0; k < g_optCount; k++)
	{
		order[k] = k;
		rnd[k] = RANDOM_LONG(0, 0x7fff);
	}
	for (int a = 0; a < g_optCount; a++)
		for (int b = a + 1; b < g_optCount; b++)
		{
			int x = order[a], y = order[b];
			if (g_votes[y] > g_votes[x] || (g_votes[y] == g_votes[x] && rnd[y] > rnd[x]))
			{
				order[a] = y;
				order[b] = x;
			}
		}
	char tally[200] = "";
	size_t used = 0;
	for (int k = 0; k < g_optCount; k++)
	{
		int w = _snprintf_wc(tally + used, sizeof(tally) - used - 1, "%s%s %d", k ? ", " : "",
			g_maps[g_opt[order[k]]].name, g_votes[order[k]]);
		if (w < 0 || (used += (size_t)w) >= sizeof(tally) - 1)
			break;
	}
	tally[sizeof(tally) - 1] = 0;
	RV_Trace("Vote%s: closed, %d vote(s): %s", g_runoffRound ? " (runoff)" : "", total, tally);

	if (total == 0)
	{
		g_phase = PH_IDLE;
		if (g_kind == VK_RTV)
		{
			RV_SayAll("%s Nobody voted - staying on this map.", RV_TAG);
			return;
		}
		char nx[RV_MAX_NAME];
		RV_CycleNext(nx, sizeof(nx));
		if (nx[0])
			RV_Decide(nx, "nobody voted - next in the map cycle");
		return;
	}
	int top = order[0], second = g_optCount > 1 ? order[1] : -1;
	bool close = second >= 0 && g_votes[second] > 0
		&& (g_votes[top] - g_votes[second]) * 100.0f <= g_rv.runoffPercent * total;
	if (g_rv.runoff && !g_runoffRound && close)
	{
		int a = g_opt[top], b = g_opt[second];
		RV_SayAll("%s %s (%d) and %s (%d) are too close - runoff between the two!", RV_TAG,
			g_maps[a].name, g_votes[top], g_maps[b].name, g_votes[second]);
		g_opt[0] = a;
		g_opt[1] = b;
		g_optNoms[0] = g_optNoms[1] = 0;
		g_optCount = 2;
		g_runoffRound = true;
		RV_BeginVoting(g_rv.runoffDuration);
		return;
	}
	char how[64];
	bool tie = second >= 0 && g_votes[second] == g_votes[top];
	_snprintf_wc(how, sizeof(how) - 1, "%d of %d vote%s%s", g_votes[top], total, total == 1 ? "" : "s",
		tie ? ", tie broken at random" : "");
	how[sizeof(how) - 1] = 0;
	RV_Decide(g_maps[g_opt[top]].name, how);
}

// ---------------------------------------------------------------------------
// The menu
static void RV_CastVote(edict_t *p, int opt, const char *how)
{
	int i = ENTINDEX(p);
	RVPlayer &s = g_pl[i];
	if (s.voted == opt)
		return; // same vote again
	if (s.voted >= 0)
		g_votes[s.voted]--;
	bool changed = s.voted >= 0;
	s.voted = opt;
	s.declined = false;
	s.menuOnce = false; // a !revote is done
	s.listHidden = false;
	g_votes[s.voted]++;
	RV_Sound(p, "buttons/button9");
	RV_Say(p, "%s You %s %s. %s", RV_TAG, changed ? "changed your vote to" : "voted for",
		g_maps[g_opt[s.voted]].name, RV_UsesKeys(i) ? "Press another vote key to change it."
		: "Say !revote to change it.");
	RV_Trace("Vote: %s -> %s (%s)", STRING(p->v.netname), g_maps[g_opt[s.voted]].name, how);
	g_nextHud = 0;
}

bool RV_MenuSelect(edict_t *p, int key)
{
	int i = ENTINDEX(p);
	if (i < 1 || i > RV_MAX_PLAYERS)
		return false;
	if (g_pl[i].setPage)
		return RV_SettingsKey(p, key);
	if (g_phase != PH_VOTING)
		return false;
	RVPlayer &s = g_pl[i];
	if (s.menuShown != g_serial)
		return false; // not our menu on their screen
	s.lastActive = gpGlobals->time;
	if (RV_ListMenu(i))
	{
		if ((key == 0 || key == 10) && !RV_UsesKeys(i))
		{
			s.listHidden = true; // "0. Hide this list"
			s.menuShown = 0;
			return true;
		}
		// The list is read-only, so a number key pressed on it is a weapon
		// key: the menu ate the press, so hand it on as the slot command it
		// was. The menu steps aside (the client closed it) and comes back
		// shortly.
		char cmd[16];
		_snprintf_wc(cmd, sizeof(cmd) - 1, "slot%d\n", (key == 0 || key == 10) ? 10 : key);
		cmd[sizeof(cmd) - 1] = 0;
		CLIENT_COMMAND(p, cmd);
		s.menuShown = 0;
		s.passHidden = true;
		s.passUntil = gpGlobals->time + 1.5f;
		return true;
	}
	if (key == 0 || key == 10)
	{
		s.declined = true;
		s.menuShown = 0;
		s.pendingKey = 0;
		s.menuOnce = false;
		if (s.voted < 0 && !RV_UsesKeys(i))
			RV_Say(p, "%s No vote. Say !revote to open the vote again.", RV_TAG);
		if (RV_ListMenu(i))
			RV_SendVoteMenu(i); // back to their list
		return true;
	}
	if (key < 1 || key > g_optCount)
	{
		RV_SendVoteMenu(i);
		return true;
	}
	if (RV_Confirm(i) && s.pendingKey != key)
	{
		s.pendingKey = key;
		RV_Sound(p, "buttons/blip1");
		RV_SendVoteMenu(i);
		return true;
	}
	s.pendingKey = 0;
	s.menuShown = 0; // the client closed it on the key press
	RV_CastVote(p, key - 1, "menu");
	if (RV_ListMenu(i))
		RV_SendVoteMenu(i); // the results list, in the same spot
	return true;
}

// PLAYER VOTE SETTINGS. Each player can choose, and keep by SteamID in
// rtv_prefs.txt next to the DLL:
//   - how they vote: the number menu (default), or VOTE KEYS - keys they bind
//     themselves to "vote1" ... "vote9" and "vote0" (no vote). With vote keys
//     the vote shows as a read-only list in the game's menu, and the number
//     keys they press on it are passed on as weapon keys;
//   - press twice: whether a number in the menu has to be pressed twice
//     (default: vote_confirm in rtv.ini);
//   - the menu's colours, part by part (title, numbers, map names, counts,
//     their vote, hints). The game's menu has only four: white, yellow, red,
//     grey (TFC's client.dll CHudMenu::Draw knows \w \y \r \d and no more).
// All of it is in one menu, !rtvmenu; !votekeys and !votemenu do the vote-key
// part from chat. The first time, rtv_votekeys.txt from earlier versions is
// taken over.
static void RV_VoteKeys(edict_t *p, const char *arg);
static void RV_SettingsShow(int i, int page);

// The ID a player's choice is kept under: the SteamID (its first digit
// varies between engine builds, so it is written as STEAM_0), or the name
// on a LAN server. Empty = not known yet.
static void RV_KeyId(edict_t *p, char *out, size_t len)
{
	out[0] = 0;
	const char *auth = GETPLAYERAUTHID(p);
	if (!auth || !auth[0] || !strcasecmp(auth, "STEAM_ID_PENDING") || !strcmp(auth, "BOT"))
		return;
	if (!strncasecmp(auth, "STEAM_", 6) && strcasecmp(auth, "STEAM_ID_LAN") && strcasecmp(auth, "STEAM_666:88:666"))
	{
		const char *c = strchr(auth, ':');
		_snprintf_wc(out, len - 1, "STEAM_0%s", c ? c : auth + 6);
	}
	else
		_snprintf_wc(out, len - 1, "NAME:%s", STRING(p->v.netname));
	out[len - 1] = 0;
}


#define RV_MAX_PREFS 512
struct RVPref
{
	char id[40];
	int  keys;        // 1 = vote keys instead of the number menu
	int  confirm;     // -1 = server default, 0 = one press, 1 = press twice
	int  hasColor;    // picked a panel colour
	int  c[3];
	int  alpha;       // panel strength 10-100 %, 0 = server default
	char mc[MC_PARTS]; // menu colours - title, numbers, map names, counts, your vote, hints: w y r d, 0 = default
	int  list;        // with vote keys, the list is: 0 = the game's menu (default), 1 = a text panel
};
static RVPref g_prefs[RV_MAX_PREFS];
static int    g_prefCount = 0;

static bool RV_DataPath(const char *file, char *out, size_t len)
{
	char dir[400];
	if (!RV_ModuleDir(dir, sizeof(dir)))
		return false;
	_snprintf_wc(out, len - 1, "%s/%s", dir, file);
	out[len - 1] = 0;
	return true;
}

static RVPref *RV_PrefFind(const char *id, bool create)
{
	for (int k = 0; k < g_prefCount; k++)
		if (!strcasecmp(g_prefs[k].id, id))
			return &g_prefs[k];
	if (!create || g_prefCount >= RV_MAX_PREFS)
		return NULL;
	RVPref &p = g_prefs[g_prefCount++];
	memset(&p, 0, sizeof(p));
	strncpy(p.id, id, sizeof(p.id) - 1);
	p.confirm = -1;
	return &p;
}

static void RV_PrefsSave(void)
{
	char path[450];
	FILE *f = RV_DataPath("rtv_prefs.txt", path, sizeof(path)) ? fopen(path, "w") : NULL;
	if (!f)
	{
		RV_Trace("Prefs: could not write %s", path);
		return;
	}
	fprintf(f, "// TFC RTV - players' vote settings (!rtvmenu). keys=1 vote keys, confirm=-1/0/1 press twice "
		"(-1 server default), colors=title,numbers,maps,counts,your vote,hints as w/y/r/d (- default)\n");
	for (int k = 0; k < g_prefCount; k++)
	{
		RVPref &p = g_prefs[k];
		fprintf(f, "%s keys=%d confirm=%d", p.id, p.keys, p.confirm);
		char mc[MC_PARTS + 1];
		for (int k = 0; k < MC_PARTS; k++)
			mc[k] = p.mc[k] ? p.mc[k] : '-';
		mc[MC_PARTS] = 0;
		fprintf(f, " colors=%s\n", mc);
	}
	fclose(f);
}

static void RV_PrefsLoad(void)
{
	g_prefCount = 0;
	char path[450], line[256];
	FILE *f = RV_DataPath("rtv_prefs.txt", path, sizeof(path)) ? fopen(path, "r") : NULL;
	if (f)
	{
		while (fgets(line, sizeof(line), f))
		{
			char *s = TrimInPlace(line);
			if (!s[0] || s[0] == '/' || s[0] == '#')
				continue;
			char id[64];
			if (sscanf(s, "%63s", id) != 1)
				continue;
			RVPref *p = RV_PrefFind(id, true);
			if (!p)
				break;
			for (char *t = strtok(s + strlen(id), " \t"); t; t = strtok(NULL, " \t"))
			{
				int a, b, c;
				if (!strncmp(t, "keys=", 5))
					p->keys = atoi(t + 5) ? 1 : 0;
				else if (!strncmp(t, "confirm=", 8))
					p->confirm = ClampI(atoi(t + 8), -1, 1);
				else if (!strncmp(t, "alpha=", 6))
					p->alpha = atoi(t + 6) ? ClampI(atoi(t + 6), 10, 100) : 0;
				else if (!strncmp(t, "menu=", 5))   // older: the map-name colour only
					p->mc[MC_NAME] = (t[5] && strchr("wyrd", t[5])) ? t[5] : 0;
				else if (!strncmp(t, "list=", 5))
					p->list = !strcmp(t + 5, "panel") ? 1 : 0;
				else if (!strncmp(t, "colors=", 7))
					for (int k = 0; k < MC_PARTS && t[7 + k]; k++)
						p->mc[k] = strchr("wyrd", t[7 + k]) ? t[7 + k] : 0;
				else if (!strncmp(t, "color=", 6) && sscanf(t + 6, "%d,%d,%d", &a, &b, &c) == 3)
				{
					p->hasColor = 1;
					p->c[0] = ClampI(a, 0, 255);
					p->c[1] = ClampI(b, 0, 255);
					p->c[2] = ClampI(c, 0, 255);
				}
			}
		}
		fclose(f);
		return;
	}
	// First run: take over the separate files of earlier versions.
	int imported = 0;
	if (RV_DataPath("rtv_votekeys.txt", path, sizeof(path)) && (f = fopen(path, "r")) != NULL)
	{
		while (fgets(line, sizeof(line), f))
		{
			char *s = TrimInPlace(line);
			if (!s[0] || s[0] == '/' || s[0] == '#')
				continue;
			RVPref *p = RV_PrefFind(s, true);
			if (p)
			{
				p->keys = 1;
				imported++;
			}
		}
		fclose(f);
	}
	if (RV_DataPath("rtv_colors.txt", path, sizeof(path)) && (f = fopen(path, "r")) != NULL)
	{
		while (fgets(line, sizeof(line), f))
		{
			char *s = TrimInPlace(line);
			char id[64];
			int a, b, c;
			if (!s[0] || s[0] == '/' || s[0] == '#' || sscanf(s, "%63s %d %d %d", id, &a, &b, &c) != 4)
				continue;
			RVPref *p = RV_PrefFind(id, true);
			if (p)
			{
				p->hasColor = 1;
				p->c[0] = ClampI(a, 0, 255);
				p->c[1] = ClampI(b, 0, 255);
				p->c[2] = ClampI(c, 0, 255);
				imported++;
			}
		}
		fclose(f);
	}
	if (imported)
	{
		RV_Trace("Prefs: took over %d setting(s) from rtv_votekeys.txt / rtv_colors.txt", imported);
		RV_PrefsSave();
	}
}

// A player's saved settings (NULL = none saved, or their ID isn't known yet).
// create: make an entry for them if there is none.
static RVPref *RV_PrefOf(int i, bool create)
{
	if (i < 1 || i > RV_MAX_PLAYERS)
		return NULL;
	RVPlayer &s = g_pl[i];
	if (s.prefKnown && (s.pref >= 0 || !create))
		return s.pref >= 0 ? &g_prefs[s.pref] : NULL;
	edict_t *e = RV_Player(i);
	if (!e || RV_IsBot(e))
		return NULL;
	char id[40];
	RV_KeyId(e, id, sizeof(id));
	if (!id[0])
		return NULL; // ask again later
	RVPref *p = RV_PrefFind(id, create);
	s.pref = p ? (int)(p - g_prefs) : -1;
	s.prefKnown = true;
	return p;
}

static bool RV_UsesKeys(int i)
{
	RVPref *p = RV_PrefOf(i, false);
	return p && p->keys;
}

static bool RV_Confirm(int i)
{
	RVPref *p = RV_PrefOf(i, false);
	return (p && p->confirm >= 0) ? p->confirm != 0 : g_rv.voteConfirm != 0;
}

static char RV_MCol(int i, int part)
{
	RVPref *p = RV_PrefOf(i, false);
	return (p && p->mc[part]) ? p->mc[part] : g_mcDefault[part];
}

static bool RV_ListMenu(int i)
{
	RVPlayer &s = g_pl[i];
	if (s.menuOnce)
		return false; // !revote: the menu to pick from
	if (RV_UsesKeys(i))
		return true;  // vote keys: they only ever read the list
	return s.voted >= 0 && !s.listHidden;
}

static const char *RV_MenuColorName(char c)
{
	return c == 'y' ? "yellow" : c == 'r' ? "red" : c == 'd' ? "grey" : "white";
}

// ---------------------------------------------------------------------------
// !rtvmenu - every setting above in one number menu. Opening it is a choice,
// so here one press does it.
enum { SET_NONE = 0, SET_MAIN, SET_MENUCOLORS, SET_PREVIEW };
#define RV_SET_TIME 60

static void RV_SettingsMark(int i, int page)
{
	RVPlayer &s = g_pl[i];
	s.setPage = page;
	s.setUntil = gpGlobals->time + RV_SET_TIME;
	s.reopenAt = 0;
	if (s.menuShown == g_serial)
		s.menuShown = 0; // it replaced the vote menu (which comes back when this closes)
	s.passHidden = false;
}

// What the vote menu looks like with this player's colours: a sample.
static void RV_MenuPreview(int i, edict_t *e)
{
	char t = RV_MCol(i, MC_TITLE), n = RV_MCol(i, MC_NUM), m = RV_MCol(i, MC_NAME), c = RV_MCol(i, MC_COUNT);
	char y = RV_MCol(i, MC_MARK), h = RV_MCol(i, MC_HINT);
	char buf[512];
	_snprintf_wc(buf, sizeof(buf) - 1,
		"\\%cChoose the next map\\w  \\%c(preview)\\w\n"
		"\\%cVote keys: vote1-vote9, vote0 = no vote\\w\n\n"
		"\\%c1.\\%c 2fort   \\%c3\\w\n"
		"\\%c2.\\%c dustbowl   \\%c1\\w  \\%c<- your vote\\w\n"
		"\\%c3.\\%c well   \\%c0\\w\n\n"
		"\\%c0.\\%c Back to the settings",
		t, h, h, n, m, c, n, m, c, y, n, m, c, n, m);
	buf[sizeof(buf) - 1] = 0;
	RV_ShowMenu(e, 0x3FF, RV_SET_TIME, buf);
	RV_SettingsMark(i, SET_PREVIEW);
}

static void RV_SettingsShow(int i, int page)
{
	edict_t *e = RV_Player(i);
	if (!e || RV_IsBot(e))
		return;
	char buf[600];
	size_t used = 0;
	#define ADD(...) RV_Append(buf, sizeof(buf), &used, __VA_ARGS__)
	buf[0] = 0;
	int keys = 1 << 9; // 0 closes
	if (page == SET_MAIN)
	{
		bool vk = RV_UsesKeys(i);
		ADD("\\yRTV vote settings\\w\n\n");
		ADD("\\r1.\\w Vote with: \\y%s\\w\n", vk ? "my own vote keys" : "the number menu");
		ADD("\\r2.\\w Press twice to vote: \\y%s\\w%s\n", RV_Confirm(i) ? "on" : "off", vk ? "  \\d(number menu)\\w" : "");
		ADD("\\r3.\\w Menu colours\n");
		ADD("\\r4.\\w Show the vote key binds\n");
		ADD("\\r5.\\w Preview\n");
		ADD("\\r6.\\w Reset to the server's settings\n");
		ADD("\n\\r0.\\w Close");
		keys |= 0x3F; // 1-6
	}
	else if (page == SET_MENUCOLORS)
	{
		ADD("\\yMenu colours\\w\n\\dthe game's menu can only show these four:\\w\n"
			"\\wwhite  \\yyellow  \\rred  \\dgrey\\w  - each press changes that part\n\n");
		for (int k = 0; k < MC_PARTS; k++)
		{
			char c = RV_MCol(i, k);
			ADD("\\r%d.\\w %s: \\%c%s\\w\n", k + 1, g_mcPart[k], c, RV_MenuColorName(c));
			keys |= 1 << k;
		}
		ADD("\\r7.\\w Preview\n");
		ADD("\n\\r9.\\w Back\n\\r0.\\w Close");
		keys |= (1 << 6) | (1 << 8);
	}
	#undef ADD
	RV_ShowMenu(e, keys, RV_SET_TIME, buf);
	RV_SettingsMark(i, page);
}

void RV_SettingsOpen(edict_t *p)
{
	int i = ENTINDEX(p);
	if (i < 1 || i > RV_MAX_PLAYERS || RV_IsBot(p))
		return;
	g_pl[i].lastActive = gpGlobals->time;
	RV_SettingsShow(i, SET_MAIN);
}

// Settings closed: during a vote, the vote menu (or their list) comes back.
static void RV_SettingsClosed(int i)
{
	RVPlayer &s = g_pl[i];
	s.setPage = 0;
	if (g_phase == PH_VOTING && ((s.voted < 0 && !s.declined) || RV_ListMenu(i)))
		RV_SendVoteMenu(i);
}

static bool RV_SettingsKey(edict_t *p, int key)
{
	int i = ENTINDEX(p);
	RVPlayer &s = g_pl[i];
	s.lastActive = gpGlobals->time;
	int page = s.setPage;
	if (page == SET_PREVIEW)
	{
		RV_SettingsShow(i, s.reopenPage ? s.reopenPage : SET_MAIN);
		return true;
	}
	if (key == 0 || key == 10)
	{
		RV_SettingsClosed(i);
		return true;
	}
	RVPref *pr = RV_PrefOf(i, true);
	if (!pr)
	{
		RV_Say(p, "%s Your ID isn't known yet - try again in a moment.", RV_TAG);
		RV_SettingsClosed(i);
		return true;
	}
	if (page == SET_MAIN)
	{
		switch (key)
		{
		case 1:
			if (pr->keys)
			{
				RV_VoteKeys(p, "off");
				RV_SettingsShow(i, SET_MAIN);
			}
			else
			{
				s.setPage = 0; // the binds window opens instead
				RV_VoteKeys(p, "");
			}
			return true;
		case 2:
			pr->confirm = RV_Confirm(i) ? 0 : 1;
			RV_PrefsSave();
			RV_Say(p, "%s Press twice to vote: %s.", RV_TAG, pr->confirm ? "on" : "off - one press votes in the number menu");
			RV_SettingsShow(i, SET_MAIN);
			return true;
		case 3: RV_SettingsShow(i, SET_MENUCOLORS); return true;
		case 4:
			s.setPage = 0;
			RV_ShowBindWindow(p);
			return true;
		case 5:
			s.reopenPage = SET_MAIN;
			RV_MenuPreview(i, p);
			return true;
		case 6:
			pr->keys = 0;
			pr->confirm = -1;
			memset(pr->mc, 0, sizeof(pr->mc));
			RV_PrefsSave();
			RV_Say(p, "%s Vote settings back to the server's.", RV_TAG);
			RV_SettingsShow(i, SET_MAIN);
			return true;
		}
		RV_SettingsShow(i, SET_MAIN);
		return true;
	}
	if (key == 9)
	{
		RV_SettingsShow(i, SET_MAIN);
		return true;
	}
	if (page == SET_MENUCOLORS)
	{
		if (key >= 1 && key <= MC_PARTS)
		{
			static const char order[] = "wyrd";
			const char *at = strchr(order, RV_MCol(i, key - 1));
			pr->mc[key - 1] = order[at ? ((at - order) + 1) % 4 : 0];
			RV_PrefsSave();
			RV_SettingsShow(i, SET_MENUCOLORS);
			return true;
		}
		if (key == 7)
		{
			s.reopenPage = SET_MENUCOLORS;
			RV_MenuPreview(i, p);
			return true;
		}
	}
	RV_SettingsShow(i, page);
	return true;
}

// Reopens the settings after a preview.
static void RV_SettingsFrame(float now)
{
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		RVPlayer &s = g_pl[i];
		if (s.reopenAt > 0 && now >= s.reopenAt)
		{
			s.reopenAt = 0;
			if (s.inGame && g_phase != PH_VOTING)
				RV_SettingsShow(i, s.reopenPage);
		}
		if (s.setPage && now > s.setUntil)
			s.setPage = 0;
	}
}

bool RV_ConsoleVote(edict_t *p, int n)
{
	int i = ENTINDEX(p);
	if (i < 1 || i > RV_MAX_PLAYERS)
		return false;
	g_pl[i].lastActive = gpGlobals->time;
	if (g_phase != PH_VOTING)
	{
		RV_Say(p, "%s There is no map vote right now.", RV_TAG);
		return true;
	}
	RVPlayer &s = g_pl[i];
	if (n < 0 || n > g_optCount)
	{
		RV_Say(p, "%s This vote has maps 1-%d: vote1-vote%d (vote0 = no vote).", RV_TAG, g_optCount, g_optCount);
		return true;
	}
	if (!RV_ListMenu(i))
	{
		if (s.menuShown == g_serial)
			RV_CloseMenu(p);
		s.menuShown = 0;
	}
	s.pendingKey = 0;
	g_nextHud = 0; // the list shows the change
	if (n == 0)
	{
		if (s.voted >= 0)
		{
			g_votes[s.voted]--;
			s.voted = -1;
		}
		s.declined = true;
		RV_Say(p, "%s No vote.", RV_TAG);
		return true;
	}
	RV_CastVote(p, n - 1, "key");
	return true;
}

static void RV_VoteKeys(edict_t *p, const char *arg)
{
	int i = ENTINDEX(p);
	RVPref *pr = RV_PrefOf(i, true);
	if (!pr)
	{
		RV_Say(p, "%s Your ID isn't known yet - try again in a moment.", RV_TAG);
		return;
	}
	if (arg && !strcasecmp(arg, "off"))
	{
		pr->keys = 0;
		RV_PrefsSave();
		RV_Say(p, "%s Vote keys off - back to the number menu%s. !votekeys to sign up again.", RV_TAG,
			RV_Confirm(i) ? " (press a number twice to vote)" : "");
		RV_Trace("Vote keys: %s off", STRING(p->v.netname));
		g_nextHud = 0;
		if (g_phase == PH_VOTING && g_pl[i].voted < 0 && !g_pl[i].declined && !g_pl[i].setPage)
			RV_SendVoteMenu(i);
		return;
	}
	pr->keys = 1;
	RV_PrefsSave();
	g_pl[i].menuOnce = false;
	if (g_phase == PH_VOTING && RV_ListMenu(i))
		RV_SendVoteMenu(i);
	else if (g_phase == PH_VOTING && g_pl[i].menuShown == g_serial)
	{
		RV_CloseMenu(p);
		g_pl[i].menuShown = 0;
	}
	g_nextHud = 0;
	RV_Trace("Vote keys: %s on", STRING(p->v.netname));
	RV_Say(p, "%s Vote keys on. Bind your own keys to vote1-vote%d (vote0 = no vote) - see the window. !votemenu to opt out.",
		RV_TAG, RV_MAX_OPTIONS);
	RV_ShowBindWindow(p);
}

static void RV_ShowBindWindow(edict_t *p)
{
	char buf[1400];
	size_t used = 0;
	RV_Append(buf, sizeof(buf), &used, "VOTE KEYS\n\n");
	RV_Append(buf, sizeof(buf), &used, "With vote keys on (!rtvmenu, option 1, or !votekeys) no menu takes over your number "
		"keys in a map vote - they stay on your weapons. The maps show numbered where the menu usually is, and you vote "
		"with keys of your own - one press.\n\n");
	RV_Append(buf, sizeof(buf), &used, "Bind any keys you like, in the console or your autoexec.cfg:\n\n");
	for (int k = 1; k <= RV_MAX_OPTIONS; k++)
		RV_Append(buf, sizeof(buf), &used, "bind <key> vote%d     - map %d\n", k, k);
	RV_Append(buf, sizeof(buf), &used, "bind <key> vote0     - no vote\n\n");
	RV_Append(buf, sizeof(buf), &used, "The number pad fits well:\n"
		"bind KP_END vote1; bind KP_DOWNARROW vote2; bind KP_PGDN vote3\n"
		"bind KP_LEFTARROW vote4; bind KP_5 vote5; bind KP_RIGHTARROW vote6\n"
		"bind KP_HOME vote7; bind KP_UPARROW vote8; bind KP_PGUP vote9\n"
		"bind KP_INS vote0\n\n");
	RV_Append(buf, sizeof(buf), &used, "Press another key to change your vote. !revote opens the number menu for one vote.\n"
		"All your vote settings: say !rtvmenu (bind a key to rtvmenu if you like).");
	RV_ShowWindow(p, buf);
}

void RV_MenuGone(edict_t *p)
{
	if (!p)
	{
		for (int i = 1; i <= RV_MAX_PLAYERS; i++)
			g_pl[i].menuShown = g_pl[i].setPage = 0;
		return;
	}
	int i = ENTINDEX(p);
	if (i >= 1 && i <= RV_MAX_PLAYERS)
		g_pl[i].menuShown = g_pl[i].setPage = 0;
}

// ---------------------------------------------------------------------------
// Frame
static void RV_AfkFrame(void)
{
	float now = gpGlobals->time;
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		RVPlayer &s = g_pl[i];
		if (!e)
		{
			if (s.inGame)
			{
				if (s.nominated >= 0)
					RV_DropNomination(i, NULL);
				RV_ResetSlot(i);
				RV_CheckRtv("someone left");
			}
			continue;
		}
		if (!s.inGame)
		{
			RV_ResetSlot(i);
			s.inGame = true;
			s.connectedAt = now;
			s.lastActive = now;
		}
		if (RV_IsBot(e) || RV_IsHLTV(e))
			continue;
		int buttons = e->v.button;
		float yaw = e->v.v_angle.y, pitch = e->v.v_angle.x;
		if (buttons != s.lastButtons || fabs(yaw - s.lastYaw) > 0.5f || fabs(pitch - s.lastPitch) > 0.5f)
			s.lastActive = now;
		s.lastButtons = buttons;
		s.lastYaw = yaw;
		s.lastPitch = pitch;

		bool afk = g_rv.afkTime > 0 && now - s.lastActive > g_rv.afkTime;
		if (afk && !s.afk)
		{
			s.afk = true;
			RV_Trace("AFK: %s (idle %.0fs)%s", STRING(e->v.netname), now - s.lastActive, s.rtv ? " - rtv removed" : "");
			if (s.rtv)
			{
				s.rtv = false;
				s.rtvLostToAfk = true;
			}
			RV_CheckRtv("someone went AFK");
		}
		else if (!afk && s.afk)
		{
			s.afk = false;
			RV_Trace("AFK: %s is back", STRING(e->v.netname));
			if (s.rtvLostToAfk)
			{
				s.rtvLostToAfk = false;
				if (g_phase == PH_IDLE && !g_nextDecided)
					RV_Say(e, "%s Your rock the vote was removed while you were away. Say rtv to vote again.", RV_TAG);
			}
			RV_CheckRtv("someone came back");
		}
	}
}

// ---------------------------------------------------------------------------
// Chat tips: how to rtv and nominate. Every advert_interval seconds (the first
// at advert_first into the map) a line to everyone, taking turns through the
// tips; and once per server session, a tip to each player advert_join seconds
// after they join. "advert" lines in rtv.ini replace the built-in tips.
static float g_nextAdvert = 0;
static int   g_advertTurn = 0;
#define RV_MAX_GREETED 512
static char  g_greeted[RV_MAX_GREETED][40];   // who has had the join tip (by SteamID / name)
static int   g_greetedCount = 0;

static void RV_JoinTip(int i, edict_t *e)
{
	RVPlayer &s = g_pl[i];
	s.joinTipDone = true;
	const char *auth = GETPLAYERAUTHID(e);
	char id[40];
	if (auth && auth[0] && strcasecmp(auth, "STEAM_ID_LAN") && strcasecmp(auth, "STEAM_ID_PENDING"))
		_snprintf_wc(id, sizeof(id) - 1, "%s", auth);
	else
		_snprintf_wc(id, sizeof(id) - 1, "NAME:%s", STRING(e->v.netname));
	id[sizeof(id) - 1] = 0;
	for (int k = 0; k < g_greetedCount; k++)
		if (!strcasecmp(g_greeted[k], id))
			return; // already told this session
	if (g_greetedCount < RV_MAX_GREETED)
		strcpy(g_greeted[g_greetedCount++], id);
	RV_Say(e, "%s This server has rock the vote: say rtv to vote for a new map.", RV_TAG);
	RV_Say(e, "%s Nominate a map: say nominate <map>, or just type the map's name. !nominate list shows them, nextmap shows what's next.", RV_TAG);
}

static void RV_AdvertFrame(float now)
{
	// Join tips.
	if (g_rv.advertJoin > 0)
		for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
		{
			RVPlayer &s = g_pl[i];
			if (!s.inGame || s.joinTipDone || now - s.connectedAt < g_rv.advertJoin)
				continue;
			edict_t *e = RV_Player(i);
			if (!e || RV_IsBot(e) || RV_IsHLTV(e))
			{
				s.joinTipDone = true;
				continue;
			}
			RV_JoinTip(i, e);
		}
	// The tip for everyone.
	if (g_rv.advertInterval <= 0 || now < g_nextAdvert)
		return;
	g_nextAdvert = now + g_rv.advertInterval;
	if (g_phase != PH_IDLE)
		return; // a vote is on - the screen says enough
	bool humans = false;
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (e && !RV_IsBot(e) && !RV_IsHLTV(e))
			humans = true;
	}
	if (!humans)
		return;
	if (g_rv.advertCount)
	{
		RV_SayAll("%s", g_rv.adverts[g_advertTurn++ % g_rv.advertCount]);
		return;
	}
	char nx[RV_MAX_NAME];
	switch (g_advertTurn++ % 3)
	{
	case 2:
		RV_SayAll("%s Say !rtvmenu for your vote settings: number menu or your own vote keys, press twice on/off, colours.", RV_TAG);
		break;
	case 0:
		if (g_nextDecided)
			RV_SayAll("%s Next map: %s (voted). Say rtv to go there now.", RV_TAG, g_next);
		else
			RV_SayAll("%s Say rtv to rock the vote for a new map. Nominate a map: say nominate <map>, or just type the map's name. !nominate list shows the maps.", RV_TAG);
		break;
	default:
		if (!RV_NextMapDecided(nx, sizeof(nx)))
			RV_CycleNext(nx, sizeof(nx));
		RV_SayAll("%s Next map: %s. Say nominate <map> to put a map in the vote, or !votekeys to vote with keys you bind yourself.", RV_TAG,
			nx[0] ? nx : "?");
		break;
	}
}

static const char *g_vox[11] = { "", "vox/one", "vox/two", "vox/three", "vox/four", "vox/five",
	"vox/six", "vox/seven", "vox/eight", "vox/nine", "vox/ten" };

void RV_Frame(void)
{
	if (!g_rv.enabled)
		return;
	float now = gpGlobals->time;
	RV_AfkFrame();
	RV_AdvertFrame(now);
	RV_SettingsFrame(now);

	if (g_phase == PH_COUNTDOWN)
	{
		int left = (int)ceilf(g_phaseEnd - now);
		if (left != g_lastTick && left > 0)
		{
			g_lastTick = left;
			char msg[64];
			_snprintf_wc(msg, sizeof(msg) - 1, "Map vote in %d", left);
			msg[sizeof(msg) - 1] = 0;
			RV_Hud(NULL, 2, -1.0f, 0.30f, 255, 176, 0, 0, 0.0f, 0.2f, 1.1f, msg);
			if (left <= 10)
				RV_Sound(NULL, g_vox[left]);
		}
		if (now >= g_phaseEnd)
			RV_BeginVoting(g_rv.voteDuration);
	}
	else if (g_phase == PH_VOTING)
	{
		bool allIn = true;
		int voters = 0;
		for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
		{
			edict_t *e = RV_Player(i);
			if (!e || RV_IsBot(e) || RV_IsHLTV(e))
				continue;
			voters++;
			RVPlayer &s = g_pl[i];
			if (s.voted < 0 && !s.declined)
				allIn = false;
			// Menu gone before they voted (another menu replaced it, or they
			// joined during the vote): show it again every few seconds.
			if (s.menuShown != g_serial && !s.setPage)
			{
				if (RV_ListMenu(i) && s.passHidden)
				{
					if (now >= s.passUntil)
						RV_SendVoteMenu(i); // back after a weapon key
				}
				else if (now >= g_nextReshow && s.voted < 0 && !s.declined)
					RV_SendVoteMenu(i);
			}
		}
		if (now >= g_nextReshow)
			g_nextReshow = now + 3.0f;
		if (now >= g_nextHud)
		{
			g_nextHud = now + 0.2f;
			RV_UpdateLists();
		}
		if (now >= g_phaseEnd || (allIn && voters > 0))
			RV_EndVoting();
	}
	else if (g_phase == PH_CHANGING)
	{
		int left = (int)ceilf(g_phaseEnd - now);
		if (left != g_lastTick && left > 0)
		{
			g_lastTick = left;
			char msg[96];
			_snprintf_wc(msg, sizeof(msg) - 1, "Changing to %s in %d", g_next, left);
			msg[sizeof(msg) - 1] = 0;
			RV_Hud(NULL, 2, -1.0f, 0.30f, 255, 176, 0, 0, 0.0f, 0.2f, 1.1f, msg);
			if (left <= 10)
				RV_Sound(NULL, g_vox[left]);
		}
		if (now >= g_phaseEnd)
		{
			g_phase = PH_IDLE;
			RV_ChangeLevelNow(g_next);
		}
	}

	// End of the map.
	float left = RV_TimeLeft();
	if (g_rv.endmapVote && left >= 0 && now - g_mapStart > 10.0f)
	{
		// Time limit raised again: the warnings can come again.
		if (left > g_rv.endmapWarnTime + 5.0f)
			g_warned = false;
		if (left > g_rv.endmapVoteTime + 5.0f && !g_nextDecided && g_phase == PH_IDLE)
			g_endmapStarted = false;
		if (!g_warned && left <= g_rv.endmapWarnTime && left > g_rv.endmapVoteTime && !g_nextDecided)
		{
			g_warned = true;
			char t[16], v[16];
			RV_TimeText(left, t, sizeof(t));
			RV_TimeText(left - g_rv.endmapVoteTime, v, sizeof(v));
			RV_SayAll("%s %s left! The vote for the next map starts in %s - nominate maps now: say nominate <map> (!nominate list).",
				RV_TAG, t, v);
			char hud[128];
			_snprintf_wc(hud, sizeof(hud) - 1, "%s left - nominate maps now!\nsay  nominate <map>", t);
			hud[sizeof(hud) - 1] = 0;
			RV_Hud(NULL, 2, -1.0f, 0.25f, 255, 176, 0, 2, 0.03f, 1.0f, 6.0f, hud);
			RV_Sound(NULL, "buttons/blip2");
			RV_Trace("Endmap: %.0fs left - nominate warning", left);
		}
		if (!g_endmapStarted && !g_nextDecided && left <= g_rv.endmapVoteTime && g_phase == PH_IDLE)
		{
			RV_Trace("Endmap: %.0fs left - vote", left);
			RV_StartVote(VK_ENDMAP, "end of map");
		}
	}
}

// ---------------------------------------------------------------------------
// Map start, players
void RV_MapStart(void)
{
	g_mapStart = gpGlobals->time;
	g_phase = PH_IDLE;
	g_runoffRound = false;
	g_nextDecided = false;
	g_next[0] = 0;
	g_warned = g_endmapStarted = false;
	g_nextAdvert = gpGlobals->time + g_rv.advertFirst;
	for (int i = 0; i <= RV_MAX_PLAYERS; i++)
		RV_ResetSlot(i);
	RV_PrefsLoad();
	RV_MapsLoad();
	char nx[RV_MAX_NAME];
	if (RV_CycleNext(nx, sizeof(nx)))
		RV_SetAmxxNextmap(nx);
}

void RV_PlayerConnect(edict_t *p)
{
	int i = ENTINDEX(p);
	if (i < 1 || i > RV_MAX_PLAYERS)
		return;
	RV_ResetSlot(i);
	g_pl[i].inGame = true;
	g_pl[i].connectedAt = g_pl[i].lastActive = gpGlobals->time;
	RV_CheckRtv(NULL);
}

void RV_PlayerDisconnect(edict_t *p)
{
	int i = ENTINDEX(p);
	if (i < 1 || i > RV_MAX_PLAYERS)
		return;
	if (g_phase == PH_VOTING && g_pl[i].voted >= 0)
		g_votes[g_pl[i].voted]--;
	if (g_pl[i].nominated >= 0)
		RV_DropNomination(i, (p->v.netname && STRING(p->v.netname)[0]) ? STRING(p->v.netname) : NULL);
	RV_ResetSlot(i);
	// The frame after, when they are gone from the count.
	g_pl[i].inGame = true;
}

// ---------------------------------------------------------------------------
// Chat
static bool RV_Is(const char *word, const char *name)
{
	if (word[0] == '!' || word[0] == '/')
		word++;
	return !strcasecmp(word, name);
}

static void RV_TellNext(edict_t *p)
{
	char nx[RV_MAX_NAME];
	if (g_phase == PH_CHANGING)
		RV_Say(p, "%s Changing to %s now.", RV_TAG, g_next);
	else if (RV_NextMapDecided(nx, sizeof(nx)))
		RV_Say(p, "%s Next map: %s (%s).", RV_TAG, nx, g_nextHow[0] ? g_nextHow : "voted");
	else if (g_phase == PH_COUNTDOWN || g_phase == PH_VOTING)
		RV_Say(p, "%s The next map is being voted on right now.", RV_TAG);
	else if (RV_CycleNext(nx, sizeof(nx)))
	{
		float left = RV_TimeLeft();
		if (g_rv.endmapVote && left > g_rv.endmapVoteTime)
		{
			char v[16];
			RV_TimeText(g_rv.endmapVoteTime, v, sizeof(v));
			RV_Say(p, "%s Next map: %s (map cycle) - unless the vote at %s left picks another.", RV_TAG, nx, v);
		}
		else
			RV_Say(p, "%s Next map: %s (map cycle).", RV_TAG, nx);
	}
	else
		RV_Say(p, "%s The next map isn't known.", RV_TAG);
}

static void RV_Status(edict_t *p)
{
	int pool = RV_PoolSize();
	char left[16] = "no limit";
	float tl = RV_TimeLeft();
	if (tl >= 0)
		RV_TimeText(tl, left, sizeof(left));
	RV_Say(p, "%s Rock the vote: %d of %d (%d players here, %.0f%% needed). Time left: %s.", RV_TAG,
		RV_RtvCount(), RV_Needed(pool), pool, g_rv.rtvPercent, left);
}

bool RV_ChatCommand(edict_t *p, const char *raw)
{
	if (!g_rv.enabled || FNullEnt(p) || !raw)
		return false;
	int idx = ENTINDEX(p);
	if (idx < 1 || idx > RV_MAX_PLAYERS)
		return false;
	g_pl[idx].lastActive = gpGlobals->time; // typing means they're here

	char text[128];
	strncpy(text, raw, sizeof(text) - 1);
	text[sizeof(text) - 1] = 0;
	char *s = TrimInPlace(text);
	size_t l = strlen(s);
	if (l >= 2 && s[0] == '"' && s[l - 1] == '"')
	{
		s[l - 1] = 0;
		s = TrimInPlace(s + 1);
	}
	if (!s[0])
		return false;
	char word[64];
	char *rest = SplitFirstToken(s, word, sizeof(word));
	rest = TrimInPlace(rest);

	if (RV_Is(word, "rtv") || RV_Is(word, "rockthevote"))
	{
		// If they were AFK a moment ago, that is over now.
		if (g_pl[idx].afk)
			RV_AfkFrame();
		RV_Rtv(p);
		return false; // the line shows in chat, as on AMXX servers
	}
	if (RV_Is(word, "unrtv"))
	{
		RV_UnRtv(p);
		return false;
	}
	if (RV_Is(word, "nominate") || RV_Is(word, "nom"))
	{
		RV_Nominate(p, rest, false);
		return false;
	}
	if (RV_Is(word, "maps") || RV_Is(word, "nominations"))
	{
		RV_NominateList(p);
		return false;
	}
	if (RV_Is(word, "nextmap"))
	{
		RV_TellNext(p);
		return false;
	}
	if (RV_Is(word, "revote") || RV_Is(word, "vote"))
	{
		if (g_phase == PH_VOTING)
		{
			g_pl[idx].declined = false;
			g_pl[idx].pendingKey = 0;
			g_pl[idx].menuOnce = true; // vote-key players too: the number menu, this once
			RV_SendVoteMenu(idx);
		}
		else
			RV_Say(p, "%s There is no map vote right now.", RV_TAG);
		return false;
	}
	if ((word[0] == '!' || word[0] == '/') && RV_Is(word, "votekeys"))
	{
		RV_VoteKeys(p, rest);
		return true;
	}
	if ((word[0] == '!' || word[0] == '/') && (RV_Is(word, "rtvmenu") || RV_Is(word, "rtvsettings") || RV_Is(word, "votesettings")))
	{
		RV_SettingsOpen(p);
		return true;
	}
	if ((word[0] == '!' || word[0] == '/') && (RV_Is(word, "rtvcolor") || RV_Is(word, "rtvcolour") || RV_Is(word, "votecolor")))
	{
		RV_Say(p, "%s The vote menu's colours are in !rtvmenu (option 3, Menu colours).", RV_TAG);
		RV_SettingsOpen(p);
		return true;
	}
	// Opting out: back to the number menu.
	if ((word[0] == '!' || word[0] == '/') && (RV_Is(word, "votemenu") || RV_Is(word, "novotekeys")))
	{
		RV_VoteKeys(p, "off");
		return true;
	}
	if ((word[0] == '!' || word[0] == '/') && RV_Is(word, "rtvstatus"))
	{
		RV_Status(p);
		return false;
	}
	if ((word[0] == '!' || word[0] == '/') && (RV_Is(word, "forcertv") || RV_Is(word, "cancelvote")))
	{
		if (!RV_IsAdmin(p))
		{
			RV_Say(p, "%s Admins only.", RV_TAG);
			return true;
		}
		if (RV_Is(word, "cancelvote"))
			RV_Cancel(STRING(p->v.netname));
		else if (g_phase != PH_IDLE)
			RV_Say(p, "%s A map vote is already under way.", RV_TAG);
		else
		{
			RV_SayAll("%s Admin %s started a map vote.", RV_TAG, STRING(p->v.netname));
			RV_ClearRtv();
			g_nextDecided = false;
			RV_StartVote(VK_RTV, "admin force");
		}
		return true;
	}
	// Just a map's name, on its own.
	if (g_rv.nominateTyped && !rest[0] && word[0] != '!' && word[0] != '/' && RV_FindMap(word) >= 0)
	{
		RV_Nominate(p, word, true);
		return false;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Server console / rcon
static void RV_Cmd_Force(void)
{
	if (g_phase != PH_IDLE)
	{
		SERVER_PRINT("[RTV] A vote is already under way.\n");
		return;
	}
	RV_ClearRtv();
	g_nextDecided = false;
	RV_SayAll("%s The server started a map vote.", RV_TAG);
	RV_StartVote(VK_RTV, "rtv_force");
}

static void RV_Cmd_Cancel(void)
{
	RV_Cancel("the server");
}

static void RV_Cmd_Status(void)
{
	char buf[1024], nx[RV_MAX_NAME], hist[600];
	int pool = RV_PoolSize();
	float tl = RV_TimeLeft();
	RV_HistoryText(hist, sizeof(hist));
	bool dec = RV_NextMapDecided(nx, sizeof(nx));
	if (!dec)
		RV_CycleNext(nx, sizeof(nx));
	_snprintf_wc(buf, sizeof(buf) - 1, "[RTV] rtv %d/%d (players here %d) phase %d timeleft %.0fs next %s (%s) recent: %s\n",
		RV_RtvCount(), RV_Needed(pool), pool, g_phase, tl, nx, dec ? "voted" : "cycle", hist);
	buf[sizeof(buf) - 1] = 0;
	SERVER_PRINT(buf);
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (!e)
			continue;
		_snprintf_wc(buf, sizeof(buf) - 1, "  %-24s %s%s%s%s nominated %s\n", STRING(e->v.netname),
			RV_IsBot(e) ? "bot " : "", g_pl[i].afk ? "AFK " : "", g_pl[i].rtv ? "RTV " : "",
			RV_IsAdmin(e) ? "admin " : "", g_pl[i].nominated >= 0 ? g_maps[g_pl[i].nominated].name : "-");
		buf[sizeof(buf) - 1] = 0;
		SERVER_PRINT(buf);
	}
}

static void RV_Cmd_Reload(void)
{
	RV_ConfigLoad();
	RV_MapsLoad();
	SERVER_PRINT("[RTV] config and map list reloaded.\n");
}

// rtv_nextmap <map>: set the next map by hand.
static void RV_Cmd_Nextmap(void)
{
	const char *m = CMD_ARGV(1);
	if (!m || !m[0])
	{
		char nx[RV_MAX_NAME];
		if (!RV_NextMapDecided(nx, sizeof(nx)))
			RV_CycleNext(nx, sizeof(nx));
		char buf[96];
		_snprintf_wc(buf, sizeof(buf) - 1, "[RTV] next map: %s\n", nx);
		buf[sizeof(buf) - 1] = 0;
		SERVER_PRINT(buf);
		return;
	}
	if (!IS_MAP_VALID((char *)m))
	{
		SERVER_PRINT("[RTV] no such map on the server.\n");
		return;
	}
	strncpy(g_next, m, RV_MAX_NAME - 1);
	g_next[RV_MAX_NAME - 1] = 0;
	strcpy(g_nextHow, "set by the server");
	g_nextDecided = true;
	RV_SetAmxxNextmap(g_next);
	RV_SayAll("%s Next map set to %s by the server.", RV_TAG, g_next);
	RV_Trace("Next map set by rtv_nextmap: %s", g_next);
}

void RV_RegisterServerCommands(void)
{
	REG_SVR_COMMAND("rtv_force", RV_Cmd_Force);
	REG_SVR_COMMAND("rtv_cancel", RV_Cmd_Cancel);
	REG_SVR_COMMAND("rtv_status", RV_Cmd_Status);
	REG_SVR_COMMAND("rtv_reload", RV_Cmd_Reload);
	REG_SVR_COMMAND("rtv_nextmap", RV_Cmd_Nextmap);
}
