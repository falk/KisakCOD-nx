#if !defined(KISAK_SP)
#error "The Switch skeleton requires KISAK_SP"
#endif

#if defined(KISAK_MP)
#error "The Switch skeleton must not enable KISAK_MP"
#endif

#if defined(KISAK_NO_FASTFILES)
#error "The Switch skeleton must not enable KISAK_NO_FASTFILES"
#endif

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "src/platform/switch/switch_platform.h"

int main(void)
{
    char **files;
    char path[] = "switch-skeleton";
    int numfiles;

#ifdef __SWITCH__
    consoleInit(NULL);
    consoleUpdate(NULL);
#endif
    Sys_Print("Kisak Switch skeleton logging\n");
    Sys_Print("");
    Sys_Print(NULL);
    (void)Sys_Cwd();
    (void)Sys_DefaultCDPath();
    (void)Sys_DirectoryHasContents(Sys_Cwd());
    files = Sys_ListFiles(Sys_Cwd(), NULL, NULL, &numfiles, 0);
    (void)numfiles;
    (void)Switch_TryFreeFileList((const char **)files);
    (void)Sys_MillisecondsRaw();
    (void)Sys_Milliseconds();
    FS_ReplaceSeparators(path);
    (void)FS_CreatePath(path);
    (void)Switch_VirtualReserve;
    (void)Switch_VirtualCommit;
    (void)Switch_VirtualDecommit;
    (void)Switch_VirtualRelease;
    return 0;
}
