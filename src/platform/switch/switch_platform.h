#ifndef SWITCH_PLATFORM_H
#define SWITCH_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifndef __cdecl
#define __cdecl
#endif

#ifdef __cplusplus
extern "C" {
#endif

void __cdecl Sys_Print(const char *msg);
void __cdecl Switch_BootLog(const char *msg);
#ifndef __cplusplus
uint32_t __cdecl Sys_Milliseconds(void);
uint32_t __cdecl Sys_MillisecondsRaw(void);
#endif
char * __cdecl Sys_Cwd(void);
const char * __cdecl Sys_DefaultCDPath(void);
void __cdecl Sys_Mkdir(const char *path);
void __cdecl Sys_LoadingKeepAlive(void);
void __cdecl Sys_OpenURL(const char *url, int doexit);
int __cdecl Sys_DirectoryHasContents(const char *directory);
char ** __cdecl Sys_ListFiles(const char *directory, const char *extension, const char *filter, int *numfiles, int wantsubs);
int Switch_TryFreeFileList(const char **list);
void __cdecl FS_ReplaceSeparators(char *path);
int __cdecl FS_CreatePath(char *OSPath);
void *Switch_VirtualReserve(size_t bytes);
int Switch_VirtualCommit(void *address, size_t bytes);
int Switch_VirtualDecommit(void *address, size_t bytes);
int Switch_VirtualRelease(void *reservation);

#if !defined(__SWITCH__)
/* Host tests replace the same free-running source used by the platform code. */
extern uint64_t (*Switch_HostMonotonicNanoseconds)(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
