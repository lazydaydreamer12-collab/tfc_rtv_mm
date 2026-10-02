// meta_api.cpp - plugin identity and metamod attach/detach, from metamod-p's
// stub_plugin.

#include <extdll.h>
#include <meta_api.h>

#include "sdk_util.h"
#include "rtv_common.h"


#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

// ONE COPY ONLY. Two copies of an RTV plugin would each count votes and each
// change the map. The first copy to attach writes its module address into a
// process environment variable (the OS's, shared by every DLL); a later copy
// refuses to attach while that address is still a loaded module with our
// name that is not itself. Any other file with tfc_rtv_mm in its name that is
// loaded gets a loud warning (RV_WarnForeignCopies).
#define RV_INSTANCE_ENV "TFC_RTV_MM_MODULE"
static bool g_ownInstance = false;

static bool RV_OtherCopyLoaded(unsigned long long other)
{
#ifdef _WIN32
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&RV_OtherCopyLoaded, &self);
	HMODULE h = (HMODULE)(uintptr_t)other;
	if (!h || h == self)
		return false;
	char path[MAX_PATH];
	if (!GetModuleFileNameA(h, path, sizeof(path)))
		return false; // gone
	return ContainsI(path, "tfc_rtv_mm");
#else
	Dl_info mine, theirs;
	if (!dladdr((void *)&RV_OtherCopyLoaded, &mine) || !mine.dli_fname)
		return false;
	if (!other || !dladdr((void *)(uintptr_t)other, &theirs) || !theirs.dli_fname)
		return false; // gone
	if (!strcmp(mine.dli_fname, theirs.dli_fname))
		return false;
	return ContainsI(theirs.dli_fname, "tfc_rtv_mm");
#endif
}

// Any other loaded file with tfc_rtv_mm in its name (an old build under
// another name, say) is reported loudly. Called at attach.
#ifndef _WIN32
static int RV_ForeignCopyCb(struct dl_phdr_info *info, size_t, void *selfBase)
{
	if (!info->dlpi_name || !info->dlpi_name[0] || (void *)info->dlpi_addr == selfBase)
		return 0;
	if (ContainsI(info->dlpi_name, "tfc_rtv_mm"))
	{
		RV_Trace("==== WARNING: a second TFC RTV library is loaded: %s - every vote would be counted twice. "
			"Remove its line from addons/metamod/plugins.ini and delete the file. ====", info->dlpi_name);
		LOG_ERROR(PLID, "WARNING: second TFC RTV library loaded (%s) - remove it from plugins.ini and delete it", info->dlpi_name);
		SERVER_PRINT("[TFC RTV] WARNING: a second copy is loaded - check addons/metamod/plugins.ini\n");
	}
	return 0;
}
#endif

void RV_WarnForeignCopies(void)
{
#ifdef _WIN32
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&RV_WarnForeignCopies, &self);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return;
	MODULEENTRY32 me;
	me.dwSize = sizeof(me);
	for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
	{
		if (me.hModule == self || !ContainsI(me.szModule, "tfc_rtv_mm"))
			continue;
		RV_Trace("==== WARNING: a second TFC RTV DLL is loaded: %s - every vote would be counted twice. "
			"Remove its line from addons/metamod/plugins.ini and delete the file. ====", me.szExePath);
		LOG_ERROR(PLID, "WARNING: second TFC RTV DLL loaded (%s) - remove it from plugins.ini and delete it", me.szExePath);
		SERVER_PRINT("[TFC RTV] WARNING: a second copy is loaded - check addons/metamod/plugins.ini\n");
	}
	CloseHandle(snap);
#else
	Dl_info mine;
	if (!dladdr((void *)&RV_WarnForeignCopies, &mine) || !mine.dli_fname)
		return;
	dl_iterate_phdr(RV_ForeignCopyCb, (void *)mine.dli_fbase);
#endif
}

static unsigned long long RV_SelfMarker(void)
{
#ifdef _WIN32
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&RV_SelfMarker, &self);
	return (unsigned long long)(uintptr_t)self;
#else
	return (unsigned long long)(uintptr_t)&RV_OtherCopyLoaded;
#endif
}

static bool RV_ClaimInstance(void)
{
	char buf[40] = "";
#ifdef _WIN32
	DWORD n = GetEnvironmentVariableA(RV_INSTANCE_ENV, buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf))
		buf[0] = 0;
#else
	const char *v = getenv(RV_INSTANCE_ENV);
	if (v)
	{
		strncpy(buf, v, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
	}
#endif
	if (buf[0] && RV_OtherCopyLoaded(strtoull(buf, NULL, 16)))
		return false;
	_snprintf_wc(buf, sizeof(buf) - 1, "%llx", RV_SelfMarker());
	buf[sizeof(buf) - 1] = 0;
#ifdef _WIN32
	SetEnvironmentVariableA(RV_INSTANCE_ENV, buf);
#else
	setenv(RV_INSTANCE_ENV, buf, 1);
#endif
	g_ownInstance = true;
	return true;
}

static void RV_ReleaseInstance(void)
{
	if (!g_ownInstance)
		return;
#ifdef _WIN32
	SetEnvironmentVariableA(RV_INSTANCE_ENV, NULL);
#else
	unsetenv(RV_INSTANCE_ENV);
#endif
	g_ownInstance = false;
}

void RV_OnMapStart(void); // dllapi.cpp

static META_FUNCTIONS gMetaFunctionTable =
{
	NULL,               // pfnGetEntityAPI
	NULL,               // pfnGetEntityAPI_Post
	GetEntityAPI2,      // pfnGetEntityAPI2 - dllapi.cpp
	NULL,               // pfnGetEntityAPI2_Post
	NULL,               // pfnGetNewDLLFunctions
	NULL,               // pfnGetNewDLLFunctions_Post
	GetEngineFunctions, // pfnGetEngineFunctions - rtv_engine.cpp
	NULL,               // pfnGetEngineFunctions_Post
};

plugin_info_t Plugin_info =
{
	META_INTERFACE_VERSION,
	"TFC RTV (rock the vote + map vote)",
	"1.0.0",
	"2026/09/30",
	"custom",
	"",
	"TFCRTV",
	PT_ANYTIME,
	PT_ANYPAUSE,
};

meta_globals_t *gpMetaGlobals;
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;

C_DLLEXPORT FORCE_STACK_ALIGN int Meta_Query(char * /*ifvers*/, plugin_info_t **pPlugInfo,
		mutil_funcs_t *pMetaUtilFuncs)
{
	*pPlugInfo = &Plugin_info;
	gpMetaUtilFuncs = pMetaUtilFuncs;
	return TRUE;
}

C_DLLEXPORT FORCE_STACK_ALIGN int Meta_Attach(PLUG_LOADTIME now,
		META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals,
		gamedll_funcs_t *pGamedllFuncs)
{
	RV_ConfigDefaults();
	RV_Trace("==== Meta_Attach (loadtime %d) ====", (int)now);
	RV_LogModuleIdentity();

	if (!RV_ClaimInstance())
	{
		RV_Trace("==== ANOTHER COPY IS ALREADY LOADED - this one refuses to attach. Check addons/metamod/plugins.ini for a second tfc_rtv_mm line. ====");
		LOG_ERROR(PLID, "TFC RTV is already loaded from another DLL - not attaching this copy (duplicate line in plugins.ini?)");
		return FALSE;
	}

	if (!pMGlobals)
	{
		LOG_ERROR(PLID, "Meta_Attach called with null pMGlobals");
		return FALSE;
	}
	gpMetaGlobals = pMGlobals;
	if (!pFunctionTable)
	{
		LOG_ERROR(PLID, "Meta_Attach called with null pFunctionTable");
		return FALSE;
	}
	memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));
	gpGamedllFuncs = pGamedllFuncs;

	RV_WarnForeignCopies();
	RV_RegisterServerCommands();

	// Loaded into a map that is already running ("meta load" mid-game):
	// ServerActivate has been and gone, so do its work now.
	if (now != PT_STARTUP && gpGlobals && gpGlobals->mapname && STRING(gpGlobals->mapname)[0])
		RV_OnMapStart();
	else
		RV_ConfigLoad();
	return TRUE;
}

C_DLLEXPORT FORCE_STACK_ALIGN int Meta_Detach(PLUG_LOADTIME /*now*/,
		PL_UNLOAD_REASON /*reason*/)
{
	// Nothing persistent to undo: no vtable hooks, no entities, no cvars.
	// metamod-p disables our server commands itself when we unload.
	RV_ReleaseInstance();
	return TRUE;
}
