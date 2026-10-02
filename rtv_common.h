// rtv_common.h - TFC Rock the Vote (metamod-p): shared declarations.
//
// WHAT IT DOES
//   - "rtv" / "!rtv" in chat is a vote to change the map now. It passes at
//     rtv_percent of the players who are here (connected, not AFK, humans;
//     spectators count). AFK players' votes are taken away and they can vote
//     again once they are back; people leaving or going AFK shrink the total.
//   - Maps are nominated with "nominate <map>", "!nominate <map>", or by
//     typing just the map's name. The last map_history maps played (this one
//     included) cannot be nominated or voted for.
//   - A passed RTV runs the map vote: a 5 second countdown, then a 15 second
//     vote in a numbered menu - the most-nominated maps first, filled up with
//     random maps from the list. A number has to be pressed TWICE to vote, so
//     switching weapons with the same keys never casts a vote by accident. If
//     the top two finish within runoff_percent of each other, a runoff between
//     just those two decides. The map then changes a few seconds later.
//   - Near the end of a timed map: a warning at endmap_warn_time seconds left
//     to nominate, the same vote at endmap_vote_time left, and its winner is
//     what the map changes to when time runs out - TFC's own end of map
//     (intermission, scoreboard) runs as normal; the ChangeLevel call it makes
//     at the end is redirected to the voted map (rtv_engine.cpp).
//   - "nextmap" says what comes next.
//
// Config: configs/rtv.ini next to the DLL. Trace log: rtv_trace.log.

#ifndef TFC_RTV_COMMON_H
#define TFC_RTV_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <extdll.h>
#include <meta_api.h>

#include "sdk_compat.h"

#define RV_MAX_PLAYERS  32
#define RV_MAX_MAPS     256
#define RV_MAX_NAME     32
#define RV_MAX_OPTIONS  9      // number keys 1-9; 0 is "no vote"
#define RV_MAX_HISTORY  64     // maps kept in rtv_history.txt
#define RV_MAX_BAN      50     // highest map_history
#define RV_MAX_IMMUNE   64
#define RV_MAX_ADMINS   64
#define RV_TAG          "[RTV]"

struct RVConfig
{
	int   enabled;
	int   debug;

	float rtvPercent;          // % of the players here needed to pass
	int   rtvMinPlayers;       // RTV needs at least this many players here
	int   includeBots;         // bots count as players (they never vote)
	float mapStartDelay;       // no RTV in the first seconds of a map
	int   adminInstant;        // an admin's !rtv passes it outright
	float afkTime;             // seconds without input before someone is AFK

	int   mapHistory;          // this many recent maps (current included) are banned
	char  immune[RV_MAX_IMMUNE][RV_MAX_NAME];   // map_immune: never banned, and don't use up a ban slot (* wildcards)
	int   immuneCount;
	char  mapsFile[128];       // map list; empty = the server's mapcyclefile
	int   nominateTyped;       // typing a map's name nominates it

	int   voteMaps;            // maps in the vote menu
	float voteDuration;        // seconds a vote lasts
	float voteCountdown;       // countdown before a vote
	int   voteConfirm;         // press the number twice
	int   runoff;              // runoff between the top two when close
	float runoffPercent;       // "close" = within this % of the votes cast
	float runoffDuration;
	float changeDelay;         // after an RTV vote: seconds until the map changes

	int   endmapVote;          // the timed vote near the end of the map
	float endmapWarnTime;      // seconds left: "nominate now" warning
	float endmapVoteTime;      // seconds left: the vote starts
	int   controlNextmap;      // always send the map change to our next map

	int   sounds;              // countdown voice and vote bell



	float advertInterval;      // seconds between "how to rtv / nominate" chat tips (0 = off)
	float advertFirst;         // first tip this many seconds into a map
	float advertJoin;          // a tip to each player this many seconds after they first join (0 = off)
	char  adverts[8][192];     // advert "text" lines: replace the built-in tips
	int   advertCount;

	int   amxxUsers;           // admins from AMX Mod X users.ini
	char  amxxFlag;            // ...with this access flag
	char  admins[RV_MAX_ADMINS][40];
	int   adminCount;
};
extern RVConfig g_rv;

struct RVPlayer
{
	bool  inGame;
	float connectedAt;
	float lastActive;
	int   lastButtons;
	float lastYaw, lastPitch;
	bool  afk;
	bool  rtv;             // has rocked the vote
	bool  rtvLostToAfk;    // ...and lost it by going AFK (told they can vote again)
	int   nominated;       // map index, -1 = none
	bool  adminKnown, admin;
	// vote menu
	int   menuShown;       // vote serial the menu was last shown for
	int   pendingKey;      // first press of a number (0 = none)
	int   voted;           // option voted for (-1 = none)
	bool  declined;        // closed the menu with 0
	bool  prefKnown;       // pref looked up (rtv_prefs.txt)
	int   pref;            // index into the saved preferences, -1 = none saved
	bool  menuOnce;        // asked for the number menu again in this vote (!revote)
	bool  listHidden;      // closed the results list with 0 (number-menu players)
	bool  joinTipDone;     // the join tip was sent (or isn't needed)
	int   setPage;         // !rtvmenu page on their screen (0 = none)
	float setUntil;        // ...until then
	float reopenAt;        // reopen it after a colour preview (0 = no)
	bool  passHidden;      // vote keys + game menu: the menu stepped aside for a weapon key...
	float passUntil;       // ...until then
	int   reopenPage;
};
extern RVPlayer g_pl[RV_MAX_PLAYERS + 1];

// rtv_engine.cpp
void RV_Trace(const char *fmt, ...);
bool RV_ModuleDir(char *out, size_t outLen);
void RV_LogModuleIdentity(void);
void RV_Say(edict_t *p, const char *fmt, ...);        // chat line to one (NULL = server console)
void RV_SayAll(const char *fmt, ...);                 // chat line to everyone
void RV_Center(edict_t *p, const char *fmt, ...);     // centre print (NULL = everyone)
void RV_ShowMenu(edict_t *p, int keys, int seconds, const char *text);
void RV_Hud(edict_t *p, int channel, float x, float y, int r, int g, int b,
	int effect, float fadeIn, float fadeOut, float hold, const char *text);
void RV_ShowWindow(edict_t *p, const char *text);
void RV_Sound(edict_t *p, const char *sound);          // "spk" on the client (NULL = everyone)
void RV_ChangeLevelNow(const char *map);
void RV_SetAmxxNextmap(const char *map);

// rtv_config.cpp
void RV_ConfigDefaults(void);
void RV_ConfigLoad(void);
bool RV_IsAdminAuth(const char *auth);

// rtv_maps.cpp
struct RVMap
{
	char name[RV_MAX_NAME];
};
extern RVMap g_maps[RV_MAX_MAPS];
extern int   g_mapCount;
extern char  g_curMap[RV_MAX_NAME];
void RV_MapsLoad(void);                // map list + history; at every map start
int  RV_FindMap(const char *name);     // exact (case-insensitive), -1 = none
int  RV_MatchMaps(const char *part, int *out, int max);   // partial matches
bool RV_MapBanned(const char *name);   // recently played (current map included), not immune
bool RV_MapImmune(const char *name);   // map_immune: never banned
bool RV_MapEligible(int idx);          // valid, in the list, not banned
int  RV_CycleNext(char *out, size_t len);   // the map after this one in the list
void RV_HistoryText(char *out, size_t len);

// rtv_vote.cpp
void RV_MapStart(void);
void RV_Frame(void);
void RV_PlayerConnect(edict_t *p);
void RV_PlayerDisconnect(edict_t *p);
bool RV_ChatCommand(edict_t *p, const char *raw);   // true = hide the line
bool RV_MenuSelect(edict_t *p, int key);            // true = our menu
bool RV_ConsoleVote(edict_t *p, int n);
void RV_SettingsOpen(edict_t *p);                   // !rtvmenu / "rtvmenu": a player's own vote settings             // "vote1".."vote9", "vote0" = no vote: a player's own vote keys
void RV_MenuGone(edict_t *p);                       // another menu took the screen (NULL = all)
bool RV_NextMapDecided(char *out, size_t len);
void RV_RegisterServerCommands(void);

edict_t *RV_Player(int idx);
bool RV_IsBot(edict_t *p);
bool RV_IsHLTV(edict_t *p);
bool RV_IsAdmin(edict_t *p);

#endif
