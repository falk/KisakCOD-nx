#ifndef SWITCH_DIRTREE_H
#define SWITCH_DIRTREE_H

// Recursive directory removal for profile deletion. POSIX only (devkitPro
// newlib and the host both provide it), so it is testable without libnx.

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Returns 1 when `path` and everything below it is gone, 0 on any failure.
// Symlinks are removed, never followed.
static inline int Switch_RemoveDirTree(const char *path)
{
    if (!path || !*path)
        return 0;

    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;

    DIR *dir = opendir(path);
    if (!dir)
        return 0;

    int ok = 1;
    size_t length = strlen(path);
    while (length > 1 && (path[length - 1] == '/' || path[length - 1] == '\\'))
        --length;

    while (const struct dirent *entry = readdir(dir))
    {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%.*s/%s", (int)length, path, name);
        if (n < 0 || n >= (int)sizeof(child) || !Switch_RemoveDirTree(child))
            ok = 0;
    }
    closedir(dir);
    return ok && rmdir(path) == 0;
}

#endif
