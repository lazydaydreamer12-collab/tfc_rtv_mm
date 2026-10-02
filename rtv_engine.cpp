// rtv_engine.cpp - engine-side glue: the map-change redirect, messages to
// players (chat, centre text, HUD text, the numbered menu, the MOTD window,
// sounds) and the trace log.
//
// THE MAP CHANGE. When mp_timelimit runs out, TFC's CHalfLifeMultiplay::Think
// (tfc.so 0x8ccb0) goes to intermission, waits mp_chattime, then calls
// CHalfLifeMultiplay::ChangeLevel (0x8dcc0). That reads the mapcycle file,
// picks the next map and calls the engine's pfnChangeLevel (g_engfuncs slot
// 6, at 0x8dfdf). We hook pfnChangeLevel: if the vote decided a different
// map, the call is made with ours instead and TFC's is dropped. Everything
// before it - intermission, the scoreboard, other plugins' end-of-map work -
// happens exactly as without us. The engine only acts on the first
// changelevel of a level, so ours is the one that counts.

#include <extdll.h>
#include <meta_api.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "rtv_common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ---------------------------------------------------------------------------
// Messages we send ourselves pass our own MessageBegin hook too; this keeps
// our menu from counting as "another menu".
static bool g_sendingOwn = false;
static bool g_ourChange = false;

static int RV_MsgId(const char *name, int *cache)
{
	if (*cache <= 0)
		*cache = GET_USER_MSG_ID(PLID, name, NULL);
	return *cache;
}
static int g_idShowMenu = 0, g_idVGUIMenu = 0, g_idTextMsg = 0, g_idMOTD = 0;

static void RV_MessageBegin(int msg_dest, int msg_type, const float *pOrigin, edict_t *ed)
{
	// Someone else's menu (TFC's class menu, an AMX Mod X menu) replaces ours
	// on that player's screen - from then on the number keys are theirs.
	if (!g_sendingOwn && (msg_type == RV_MsgId("ShowMenu", &g_idShowMenu)
		|| msg_type == RV_MsgId("VGUIMenu", &g_idVGUIMenu)))
	{
		if (ed && (msg_dest == MSG_ONE || msg_dest == MSG_ONE_UNRELIABLE))
			RV_MenuGone(ed);
		else if (msg_dest == MSG_ALL || msg_dest == MSG_BROADCAST)
			RV_MenuGone(NULL);
	}
	RETURN_META(MRES_IGNORED);
}

static void RV_ChangeLevel(char *s1, char *s2)
{
	if (g_ourChange || !g_rv.enabled || !s1)
		RETURN_META(MRES_IGNORED);
	char want[RV_MAX_NAME] = "";
	const char *why = "voted";
	if (!RV_NextMapDecided(want, sizeof(want)))
	{
		want[0] = 0;
		if (g_rv.controlNextmap)
		{
			RV_CycleNext(want, sizeof(want));
			why = "next in the map list";
		}
	}
	if (!want[0] || !strcasecmp(want, s1) || !IS_MAP_VALID(want))
	{
		RV_Trace("ChangeLevel: %s (the game's choice%s)", s1, want[0] ? " - same as ours" : "");
		RETURN_META(MRES_IGNORED);
	}
	RV_Trace("ChangeLevel: the game asked for %s - sending %s instead (%s)", s1, want, why);
	g_ourChange = true;
	CHANGE_LEVEL(want, NULL);
	g_ourChange = false;
	RETURN_META(MRES_SUPERCEDE);
}

void RV_ChangeLevelNow(const char *map)
{
	RV_Trace("ChangeLevel: %s now", map);
	g_ourChange = true;
	CHANGE_LEVEL((char *)map, NULL);
	g_ourChange = false;
}

// AMX Mod X's timeleft.amxx / nextmap.amxx show amx_nextmap; keep it true.
void RV_SetAmxxNextmap(const char *map)
{
	if (map && map[0] && CVAR_GET_POINTER("amx_nextmap"))
		CVAR_SET_STRING("amx_nextmap", map);
}

static enginefuncs_t gEngineFunctions;

C_DLLEXPORT FORCE_STACK_ALIGN int GetEngineFunctions(enginefuncs_t *pengfuncsFromEngine,
		int *interfaceVersion)
{
	if (!pengfuncsFromEngine)
	{
		LOG_ERROR(PLID, "GetEngineFunctions called with null pengfuncsFromEngine");
		return FALSE;
	}
	else if (*interfaceVersion != ENGINE_INTERFACE_VERSION)
	{
		LOG_ERROR(PLID, "GetEngineFunctions version mismatch; requested=%d ours=%d",
			*interfaceVersion, ENGINE_INTERFACE_VERSION);
		*interfaceVersion = ENGINE_INTERFACE_VERSION;
		return FALSE;
	}
	memset(&gEngineFunctions, 0, sizeof(gEngineFunctions));
	gEngineFunctions.pfnMessageBegin = RV_MessageBegin;
	gEngineFunctions.pfnChangeLevel  = RV_ChangeLevel;
	memcpy(pengfuncsFromEngine, &gEngineFunctions, sizeof(enginefuncs_t));
	return TRUE;
}

// ---------------------------------------------------------------------------
// Output
#define RV_HUD_PRINTTALK   3
#define RV_HUD_PRINTCENTER 4

static void RV_SendText(edict_t *p, int dest, const char *text)
{
	int id = RV_MsgId("TextMsg", &g_idTextMsg);
	if (id <= 0 || !text || !text[0])
		return;
	char buf[190];
	// A leading '#' would make the client look it up as a titles.txt key.
	_snprintf_wc(buf, sizeof(buf) - 1, "%s%s%s", text[0] == '#' ? " " : "", text,
		dest == RV_HUD_PRINTTALK ? "\n" : "");
	buf[sizeof(buf) - 1] = 0;
	if (p && (FNullEnt(p) || RV_IsBot(p)))
		return;
	g_sendingOwn = true;
	if (p)
		MESSAGE_BEGIN(MSG_ONE, id, NULL, p);
	else
		MESSAGE_BEGIN(MSG_ALL, id, NULL, (edict_t *)NULL);
	WRITE_BYTE(dest);
	WRITE_STRING(buf);
	MESSAGE_END();
	g_sendingOwn = false;
}

void RV_Say(edict_t *p, const char *fmt, ...)
{
	char msg[180];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;
	if (!p)
	{
		SERVER_PRINT(msg);
		SERVER_PRINT("\n");
		return;
	}
	RV_SendText(p, RV_HUD_PRINTTALK, msg);
}

void RV_SayAll(const char *fmt, ...)
{
	char msg[180];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;
	RV_SendText(NULL, RV_HUD_PRINTTALK, msg);
	if (gpMetaUtilFuncs)
		LOG_MESSAGE(PLID, "%s", msg);
}

void RV_Center(edict_t *p, const char *fmt, ...)
{
	char msg[180];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;
	RV_SendText(p, RV_HUD_PRINTCENTER, msg);
}

// A numbered menu on the left of the screen: the game's ShowMenu message
// (CHudMenu::MsgFunc_ShowMenu, client.so 0x674d0). SHORT valid keys (bit 0 =
// key 1 ... bit 9 = key 0), CHAR seconds shown (-1 = until closed), BYTE
// "more to come", STRING text - joined into a 512-byte buffer. \y \w \r \d
// set the colour. While it is up, the number keys pick from it instead of
// switching weapons; a pick sends "menuselect <n>" (dllapi.cpp).
void RV_ShowMenu(edict_t *p, int keys, int seconds, const char *text)
{
	int id = RV_MsgId("ShowMenu", &g_idShowMenu);
	if (id <= 0 || FNullEnt(p) || RV_IsBot(p) || !text)
		return;
	size_t len = strlen(text);
	if (len > 500)
		len = 500;
	size_t pos = 0;
	char chunk[176];
	g_sendingOwn = true;
	do
	{
		size_t n = len - pos;
		if (n > sizeof(chunk) - 1)
			n = sizeof(chunk) - 1;
		memcpy(chunk, text + pos, n);
		chunk[n] = 0;
		pos += n;
		MESSAGE_BEGIN(MSG_ONE, id, NULL, p);
		WRITE_SHORT(keys);
		WRITE_CHAR(seconds);
		WRITE_BYTE(pos < len ? 1 : 0);
		WRITE_STRING(chunk);
		MESSAGE_END();
	} while (pos < len);
	g_sendingOwn = false;
}

static unsigned short RV_Fixed16(float value, float scale)
{
	int v = (int)(value * scale);
	if (v < 0) v = 0;
	if (v > 0xFFFF) v = 0xFFFF;
	return (unsigned short)v;
}

// HUD text (TE_TEXTMESSAGE, as the HL SDK's UTIL_HudMessage builds it).
// x or y of -1 centres it. p = NULL for everyone.
void RV_Hud(edict_t *p, int channel, float x, float y, int r, int g, int b,
	int effect, float fadeIn, float fadeOut, float hold, const char *text)
{
	if (p && (FNullEnt(p) || RV_IsBot(p)))
		return;
	g_sendingOwn = true;
	if (p)
		MESSAGE_BEGIN(MSG_ONE, SVC_TEMPENTITY, NULL, p);
	else
		MESSAGE_BEGIN(MSG_ALL, SVC_TEMPENTITY, NULL, (edict_t *)NULL);
	WRITE_BYTE(TE_TEXTMESSAGE);
	WRITE_BYTE(channel & 0xFF);
	WRITE_SHORT((short)(x * 8192));
	WRITE_SHORT((short)(y * 8192));
	WRITE_BYTE(effect);
	WRITE_BYTE(r); WRITE_BYTE(g); WRITE_BYTE(b); WRITE_BYTE(255);
	WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(255);
	WRITE_SHORT(RV_Fixed16(fadeIn, 1 << 8));
	WRITE_SHORT(RV_Fixed16(fadeOut, 1 << 8));
	WRITE_SHORT(RV_Fixed16(hold, 1 << 8));
	if (effect == 2)
		WRITE_SHORT(RV_Fixed16(0.5f, 1 << 8));
	WRITE_STRING(text);
	MESSAGE_END();
	g_sendingOwn = false;
}

// TFC's MOTD window (TeamFortressViewport::MsgFunc_MOTD, client.so 0x8af40):
// chunks of WRITE_BYTE(final) + WRITE_STRING, up to 1536 characters.
void RV_ShowWindow(edict_t *p, const char *text)
{
	int id = RV_MsgId("MOTD", &g_idMOTD);
	if (id <= 0 || FNullEnt(p) || RV_IsBot(p) || !text)
		return;
	size_t len = strlen(text);
	if (len > 1500)
		len = 1500;
	size_t pos = 0;
	char chunk[176];
	g_sendingOwn = true;
	do
	{
		size_t n = len - pos;
		if (n > sizeof(chunk) - 1)
			n = sizeof(chunk) - 1;
		memcpy(chunk, text + pos, n);
		chunk[n] = 0;
		pos += n;
		MESSAGE_BEGIN(MSG_ONE, id, NULL, p);
		WRITE_BYTE(pos >= len ? 1 : 0);
		WRITE_STRING(chunk);
		MESSAGE_END();
	} while (pos < len);
	g_sendingOwn = false;
}

// "spk <sound>" on the client: sound/vox/*.wav and buttons/*.wav ship with
// every Half-Life install, so nothing has to be precached or downloaded.
void RV_Sound(edict_t *p, const char *sound)
{
	if (!g_rv.sounds || !sound)
		return;
	char cmd[96];
	_snprintf_wc(cmd, sizeof(cmd) - 1, "spk %s\n", sound);
	cmd[sizeof(cmd) - 1] = 0;
	if (p)
	{
		if (!FNullEnt(p) && !RV_IsBot(p))
			CLIENT_COMMAND(p, cmd);
		return;
	}
	for (int i = 1; i <= gpGlobals->maxClients && i <= RV_MAX_PLAYERS; i++)
	{
		edict_t *e = RV_Player(i);
		if (e && !RV_IsBot(e) && !RV_IsHLTV(e))
			CLIENT_COMMAND(e, cmd);
	}
}

// ---------------------------------------------------------------------------
// Trace log, beside the DLL.
bool RV_ModuleDir(char *out, size_t outLen)
{
#ifdef _WIN32
	HMODULE hModule = NULL;
	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&RV_ModuleDir, &hModule))
		return false;
	char path[MAX_PATH];
	DWORD len = GetModuleFileNameA(hModule, path, sizeof(path));
	if (len == 0 || len >= sizeof(path))
		return false;
	char *slash = strrchr(path, '\\');
	if (!slash)
		return false;
	*slash = 0;
#else
	Dl_info info;
	if (!dladdr((void *)&RV_ModuleDir, &info) || !info.dli_fname)
		return false;
	char path[512];
	strncpy(path, info.dli_fname, sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	char *slash = strrchr(path, '/');
	if (!slash)
		return false;
	*slash = 0;
#endif
	strncpy(out, path, outLen - 1);
	out[outLen - 1] = 0;
	return true;
}

void RV_Trace(const char *fmt, ...)
{
	if (!g_rv.debug)
		return;
	char msg[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	msg[sizeof(msg) - 1] = 0;

	float t = gpGlobals ? gpGlobals->time : -1.0f;
	if (gpMetaUtilFuncs)
		LOG_MESSAGE(PLID, "[RTV t=%.2f] %s", t, msg);

	char dir[400], path[450];
	if (!RV_ModuleDir(dir, sizeof(dir)))
		return;
	_snprintf_wc(path, sizeof(path) - 1, "%s/rtv_trace.log", dir);
	path[sizeof(path) - 1] = 0;
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	fprintf(f, "[t=%.2f] %s\n", t, msg);
	fclose(f);
}

void RV_LogModuleIdentity(void)
{
#ifdef _WIN32
	HMODULE hModule = NULL;
	char path[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&RV_LogModuleIdentity, &hModule)
		|| !GetModuleFileNameA(hModule, path, sizeof(path))
		|| !GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
	{
		RV_Trace("==== build identity unavailable ====");
		return;
	}
	SYSTEMTIME utc, local;
	FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
	SystemTimeToTzSpecificLocalTime(NULL, &utc, &local);
	RV_Trace("==== build identity: %s written %04d-%02d-%02d %02d:%02d:%02d local, %lu bytes ====",
		path, (int)local.wYear, (int)local.wMonth, (int)local.wDay,
		(int)local.wHour, (int)local.wMinute, (int)local.wSecond,
		(unsigned long)fad.nFileSizeLow);
#else
	char dir[400];
	RV_Trace("==== build identity: %s ====", RV_ModuleDir(dir, sizeof(dir)) ? dir : "?");
#endif
}
