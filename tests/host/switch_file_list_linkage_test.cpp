#include "src/platform/switch/switch_platform.h"

int main()
{
    int count = 1;
    char path[] = "linkage\\//proof";
    char filename[] = "linkage-proof";
    char **list = Sys_ListFiles("/definitely/not/a/kisak/file-list", nullptr, nullptr, &count, 0);

    FS_ReplaceSeparators(path);
    if (list != nullptr || count != 0 || path[7] != '/' || FS_CreatePath(filename) != 0)
        return 1;
    return Switch_TryFreeFileList((const char **)list) ? 0 : 1;
}
