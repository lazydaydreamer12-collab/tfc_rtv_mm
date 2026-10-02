// dllapi.cpp - the game-DLL hooks. All pre-hooks; every one ends in
// RETURN_META (a hook that returns without setting a result leaves the
// previous call's in place, which can make metamod skip the game's own
// function).

#include <extdll.h>
#include <dllapi.h>
#include <meta_api.h>

#include <stdlib.h>
#include <string.h>

#include "rtv_common.h"

void RV_OnMapStart(void)
{
	RV_ConfigLoad();
	RV_MapStart();
}

static void RV_ServerActivate(edict_t *pEdictList, int edictCount, int clientMax)
{
	RV_Trace("==== ServerActivate: %s ====", STRING(gpGlobals->mapname));
	RV_OnMapStart();
	RETURN_META(MRES_IGNORED);
}

static void RV_ClientPutInServer(edict_t *pEntity)
{
	RV_PlayerConnect(pEntity);
	RETURN_META(MRES_IGNORED);
}

static void RV_ClientDisconnect(edict_t *pEntity)
{
	RV_PlayerDisconnect(pEntity);
	RETURN_META(MRES_IGNORED);
}

static void RV_ClientCommand(edict_t *pEntity)
{
	if (!g_rv.enabled || FNullEnt(pEntity))
		RETURN_META(MRES_IGNORED);
	const char *cmd = CMD_ARGV(0);
	if (!cmd)
		RETURN_META(MRES_IGNORED);
	// A number picked in our vote menu (RV_ShowMenu).
	if (!strcasecmp(cmd, "menuselect") && CMD_ARGC() > 1)
	{
		if (RV_MenuSelect(pEntity, atoi(CMD_ARGV(1))))
			RETURN_META(MRES_SUPERCEDE);
		RETURN_META(MRES_IGNORED);
	}
	// A player's own vote keys (see !votekeys): vote1 ... vote9, vote0 = no
	// vote. The older "rtv_vote <n>" still works.
	if (!strncasecmp(cmd, "vote", 4) && cmd[4] >= '0' && cmd[4] <= '9' && !cmd[5])
	{
		RV_ConsoleVote(pEntity, cmd[4] - '0');
		RETURN_META(MRES_SUPERCEDE);
	}
	if (!strcasecmp(cmd, "rtvmenu"))
	{
		RV_SettingsOpen(pEntity);
		RETURN_META(MRES_SUPERCEDE);
	}
	if (!strcasecmp(cmd, "rtv_vote"))
	{
		RV_ConsoleVote(pEntity, CMD_ARGC() > 1 ? atoi(CMD_ARGV(1)) : 0);
		RETURN_META(MRES_SUPERCEDE);
	}
	if (!strcasecmp(cmd, "say") || !strcasecmp(cmd, "say_team"))
	{
		if (RV_ChatCommand(pEntity, CMD_ARGS()))
			RETURN_META(MRES_SUPERCEDE);
		RETURN_META(MRES_IGNORED);
	}
	RETURN_META(MRES_IGNORED);
}

static void RV_StartFrame(void)
{
	RV_Frame();
	RETURN_META(MRES_IGNORED);
}

static DLL_FUNCTIONS gFunctionTable;

C_DLLEXPORT FORCE_STACK_ALIGN int GetEntityAPI2(DLL_FUNCTIONS *pFunctionTable,
		int *interfaceVersion)
{
	if (!pFunctionTable)
	{
		UTIL_LogPrintf("GetEntityAPI2 called with null pFunctionTable");
		return FALSE;
	}
	else if (*interfaceVersion != INTERFACE_VERSION)
	{
		UTIL_LogPrintf("GetEntityAPI2 version mismatch; requested=%d ours=%d", *interfaceVersion, INTERFACE_VERSION);
		*interfaceVersion = INTERFACE_VERSION;
		return FALSE;
	}
	memset(&gFunctionTable, 0, sizeof(gFunctionTable));
	gFunctionTable.pfnServerActivate    = RV_ServerActivate;
	gFunctionTable.pfnClientPutInServer = RV_ClientPutInServer;
	gFunctionTable.pfnClientDisconnect  = RV_ClientDisconnect;
	gFunctionTable.pfnClientCommand     = RV_ClientCommand;
	gFunctionTable.pfnStartFrame        = RV_StartFrame;
	memcpy(pFunctionTable, &gFunctionTable, sizeof(DLL_FUNCTIONS));
	return TRUE;
}
