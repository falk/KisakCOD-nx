#define _POSIX_C_SOURCE 200809L

#include "src/platform/switch/switch_platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int Check(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "%s\n", message);
        return 1;
    }

    return 0;
}

int main(void)
{
    const char *state_dir = getenv("KISAK_TEST_STATE_DIR");
    char expected_cwd[4096];
    char fixture[4000];
    char path[4096];
    char *cwd;
    struct stat status;
    FILE *file;

    if (Check(state_dir != NULL && state_dir[0] != '\0', "KISAK_TEST_STATE_DIR is not set") ||
        Check(snprintf(fixture, sizeof(fixture), "%s/filesystem-fixture", state_dir) >= 0 &&
            strlen(fixture) < sizeof(fixture) - 1, "fixture path is too long"))
        return 1;

    snprintf(path, sizeof(path), "%s/file", fixture);
    unlink(path);
    snprintf(path, sizeof(path), "%s/child", fixture);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/CVS", fixture);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/cVs", fixture);
    rmdir(path);
    rmdir(fixture);

    Sys_Mkdir(NULL);
    Sys_Mkdir("");
    Sys_Mkdir(fixture);
    if (Check(stat(fixture, &status) == 0 && S_ISDIR(status.st_mode), "Sys_Mkdir did not create fixture"))
        return 1;
    Sys_Mkdir(fixture);

    cwd = Sys_Cwd();
    if (Check(getcwd(expected_cwd, sizeof(expected_cwd)) != NULL, "could not read host cwd") ||
        Check(cwd != NULL && cwd[0] == '/', "Sys_Cwd is not absolute") ||
        Check(strcmp(cwd, expected_cwd) == 0, "Sys_Cwd is not the current host directory") ||
        Check(strcmp(Sys_DefaultCDPath(), "") == 0, "Sys_DefaultCDPath is not empty") ||
        Check(Sys_DirectoryHasContents(NULL) == 0, "null directory was not empty") ||
        Check(Sys_DirectoryHasContents("") == 0, "empty directory was not empty") ||
        Check(Sys_DirectoryHasContents("/definitely/not/a/kisak/filesystem/directory") == 0, "missing directory was not empty") ||
        Check(Sys_DirectoryHasContents(fixture) == 0, "empty directory had contents"))
        return 1;

    snprintf(path, sizeof(path), "%s/CVS", fixture);
    Sys_Mkdir(path);
    if (Check(Sys_DirectoryHasContents(fixture) == 0, "CVS-only directory had contents"))
        return 1;
    rmdir(path);

    snprintf(path, sizeof(path), "%s/cVs", fixture);
    Sys_Mkdir(path);
    if (Check(Sys_DirectoryHasContents(fixture) == 0, "mixed-case CVS-only directory had contents"))
        return 1;
    rmdir(path);

    snprintf(path, sizeof(path), "%s/file", fixture);
    file = fopen(path, "w");
    if (Check(file != NULL, "could not create fixture file"))
        return 1;
    fclose(file);
    if (Check(Sys_DirectoryHasContents(fixture) == 1, "normal file was not contents"))
        return 1;
    unlink(path);

    snprintf(path, sizeof(path), "%s/child", fixture);
    Sys_Mkdir(path);
    if (Check(Sys_DirectoryHasContents(fixture) == 1, "normal subdirectory was not contents"))
        return 1;

    rmdir(path);
    rmdir(fixture);
    return 0;
}
