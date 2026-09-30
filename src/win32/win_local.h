// win_local.h: Win32-specific Quake3 header file
#pragma once // addition

#if !defined(_WIN32)
#include <mutex>
#endif

#if defined (_MSC_VER) && (_MSC_VER >= 1200)
#pragma warning(disable : 4201)
#pragma warning( push )
#endif
//#include <windows.h>
//#include "../qcommon/platform.h"
#if defined (_MSC_VER) && (_MSC_VER >= 1200)
#pragma warning( pop )
#endif

// DirectInput and Winsock are Win32 shell dependencies.  Horizon has its own
// controller and network seams, so including these headers from a shared
// declaration header would make otherwise portable database/UI owners fail
// before they reach their Switch implementation.
#if defined(_WIN32) && !defined(_XBOX)
#define DIRECTINPUT_VERSION 0x0800  //[ 0x0300 | 0x0500 | 0x0700 | 0x0800 ]
#include <dinput.h>
//#include <dsound.h>
#include <winsock.h>
#include <wsipx.h>
#endif
#include <qcommon/qcommon.h>
#include <qcommon/sys_event.h>
#include <universal/critical_section.h>
#ifdef KISAK_MP
#include <qcommon/net_chan_mp.h>
#elif KISAK_SP
#include <qcommon/net_chan.h>
#elif defined(KISAK_RADIANT)
// ─────────────────────────────────────────────────────────────────────────────
// The Radiant tools build cannot include qcommon/net_chan.h or qcommon/msg.h —
// both #error on anything but KISAK_SP. These local defs stand in, and their
// layouts are pinned to the real engine structs with static_asserts (below) so
// they can never silently drift from what compiled engine code expects.
//
//  netadr_t : 20 bytes, identical to win_net.h's KISAK_RADIANT branch — the
//             cod4-accurate Q3-lineage layout WITH the legacy ipx[10] tail.
//             Deliberately NOT net_chan.h's reduced 12-byte SP variant (no ipx);
//             win_local.h and win_net.h share the KISAK_RADIANT_NETADR_DEFINED
//             guard so the two definitions stay byte-identical.
//  msg_t    : full 40-byte (0x28) layout copied verbatim from qcommon/msg.h, so
//             that net / client code eventually pulled into the editor
//             (remote-compile / live-reload) sees the correct field offsets.
//             No compiled radiant TU references msg_t today — the
//             Sys_GetPacket / Sys_GetBroadcastPacket decls that would are
//             SP/MP-gated below.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef KISAK_RADIANT_NETADR_DEFINED
#define KISAK_RADIANT_NETADR_DEFINED
struct netadr_t { int type; unsigned char ip[4]; unsigned short port; unsigned char ipx[10]; };
#endif
#ifndef KISAK_RADIANT_MSG_DEFINED
#define KISAK_RADIANT_MSG_DEFINED
struct msg_t   // == qcommon/msg.h (sizeof 0x28)
{
    int overflowed;
    int readOnly;
    unsigned char *data;
    unsigned char *splitData;
    int maxsize;
    int cursize;
    int splitSize;
    int readcount;
    int bit;
    int lastEntityRef;
};
#endif
// Permanent layout regression net — fail the build if either struct drifts from
// the real engine layout documented above.
static_assert(sizeof(netadr_t) == 20,            "radiant netadr_t must match win_net.h (cod4) layout");
static_assert(offsetof(netadr_t, ip)   == 4,     "netadr_t.ip offset");
static_assert(offsetof(netadr_t, port) == 8,     "netadr_t.port offset");
static_assert(offsetof(netadr_t, ipx)  == 10,    "netadr_t.ipx offset");
static_assert(sizeof(msg_t) == 40,               "radiant msg_t must match qcommon/msg.h layout");
static_assert(offsetof(msg_t, data)      == 8,   "msg_t.data offset");
static_assert(offsetof(msg_t, maxsize)   == 16,  "msg_t.maxsize offset");
static_assert(offsetof(msg_t, readcount) == 28,  "msg_t.readcount offset");
#endif

void	IN_MouseEvent (int mstate);

void	Sys_CreateConsole( void );
void	Sys_DestroyConsole( void );
void __cdecl Sys_ShowConsole();

char	*Sys_ConsoleInput (void);

#if defined(KISAK_MP) || defined(KISAK_SP)
void Sys_ShowIP();
bool Sys_IsLANAddress(netadr_t adr);
bool Sys_IsLANAddress_IgnoreSubnet(netadr_t adr);

struct netadr_t;
struct msg_t;

qboolean	Sys_GetPacket ( netadr_t *net_from, msg_t *net_message );
qboolean	Sys_GetBroadcastPacket( msg_t *net_message );
#endif

// Input subsystem

void	IN_Init (void);
void	IN_Shutdown (void);
void	IN_JoystickCommands (void);

void __cdecl IN_ShowSystemCursor(BOOL show);

// KISAKTODO void	IN_Move (usercmd_s *cmd); // usercmd_t -> usercmd_s
// add additional non keyboard / non mouse movement on top of the keyboard move cmd

void	IN_DeactivateWin32Mouse( void);

void	IN_Activate (qboolean active);
// Real implementation is switch_input_lifecycle.cpp, a C-linkage (extern "C")
// TU; match that linkage here too, or C++ callers that only see this
// declaration look for a mangled symbol that never exists.
#if defined(__SWITCH__) && defined(__cplusplus)
extern "C" {
#endif
void	IN_Frame (void);
#if defined(__SWITCH__) && defined(__cplusplus)
}
#endif

bool IN_IsTalkKeyHeld();

// window procedure: a real Win32 message loop, never reached on Horizon.
#if defined(_WIN32) && !defined(_XBOX)
LRESULT WINAPI MainWndProc (
    HWND    hWnd,
    UINT    uMsg,
    WPARAM  wParam,
    LPARAM  lParam);
#endif

void Conbuf_AppendText( const char *msg );
void Conbuf_AppendTextInMainThread(const char* msg);

#if defined(_WIN32) && !defined(_XBOX)
// LWSS: Accurate to cod4
typedef struct
{
	HINSTANCE		reflib_library;		// Handle to refresh DLL
	qboolean		reflib_active;

	HWND			hWnd;
	HINSTANCE		hInstance;
	qboolean		activeApp;
	qboolean		isMinimized;
	qboolean		recenterMouse;

	OSVERSIONINFO	osversion;

	// when we get a windows message, we store the time off so keyboard processing
	// can know the exact time of an event
	unsigned		sysMsgTime;
} WinVars_t;

extern WinVars_t	g_wv;
#endif

struct __declspec(align(8)) SysInfo // sizeof=0x260
{                                       // ...
	double cpuGHz;                 // ...
	double configureGHz;           // ...
	int logicalCpuCount;                // ...
	int physicalCpuCount;               // ...
	int sysMB;                          // ...
	char gpuDescription[512];           // ...
	bool SSE;                           // ...
	char cpuVendor[13];                 // ...
	char cpuName[49];                   // ...
	// padding byte
	// padding byte
	// padding byte
	// padding byte
	// padding byte
};

#define	MAX_QUED_EVENTS		256
#define	MASK_QUED_EVENTS	( MAX_QUED_EVENTS - 1 )

// LWSS add

#if defined(_WIN32)
extern _RTL_CRITICAL_SECTION s_criticalSections[];
#else
extern std::mutex s_criticalSections[];
#endif

extern int client_state; // LWSS ADD. This looks similar to signonstate
extern HWND g_splashWnd;

// Fast reader/writer lock moved to a platform-neutral header so the database
// and dvar closures can use it without the Win32 shell.
#include <universal/fast_critical_section.h>

void Sys_SetErrorText(const char* buf);
void Sys_Error(const char *error, ...);
void __cdecl Sys_OutOfMemErrorInternal(const char* filename, int line);
void __cdecl Sys_NormalExit();

void __cdecl Sys_OpenURL(const char *url, int doexit);
void __cdecl  Sys_Quit();
// Real implementation is switch_platform.c's Sys_Print, a C-linkage TU;
// match that linkage here too for the same reason as Sys_Milliseconds in
// q_shared.h.
#if defined(__SWITCH__) && defined(__cplusplus)
extern "C" {
#endif
void __cdecl Sys_Print(const char *msg);
#if defined(__SWITCH__) && defined(__cplusplus)
}
#endif
char *__cdecl Sys_GetClipboardData();
int __cdecl Sys_SetClipboardData(const char *text);
void Sys_ShutdownEvents();
void __cdecl Sys_LoadingKeepAlive();
void __cdecl Sys_Init();

void Sys_In_Restart_f();
#ifdef KISAK_MP
void Sys_Net_Restart_f();
void __cdecl Sys_Listen_f();
#endif

void __cdecl Sys_Mkdir(const char *path);
BOOL __cdecl Sys_RemoveDirTree(const char *path);
int __cdecl Sys_CountFileList(char **list);
char **__cdecl Sys_ListFiles(
	const char *directory,
	const char *extension,
	const char *filter,
	int *numfiles,
	int wantsubs);
char *__cdecl Sys_Cwd();
const char *__cdecl Sys_DefaultCDPath();
char *__cdecl Sys_DefaultInstallPath();
void __cdecl Sys_QuitAndStartProcess(const char *exeName, const char *parameters);


// win_voice
bool __cdecl Voice_SendVoiceData();
bool __cdecl Voice_Init();
void __cdecl Voice_Shutdown();
double __cdecl Voice_GetVoiceLevel();
void __cdecl Voice_Playback();
int __cdecl Voice_GetLocalVoiceData();
void __cdecl Voice_IncomingVoiceData(unsigned __int8 talker, unsigned __int8 *data, int packetDataSize);
bool __cdecl Voice_IsClientTalking(uint32_t clientNum);
char __cdecl Voice_StartRecording();
char __cdecl Voice_StopRecording();

extern SysInfo sys_info;
