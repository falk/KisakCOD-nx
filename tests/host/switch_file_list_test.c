#define _POSIX_C_SOURCE 200809L

#include "src/platform/switch/switch_platform.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static int CreateFile(const char *path)
{
    FILE *file = fopen(path, "w");

    if (file == NULL)
        return 0;
    fclose(file);
    return 1;
}

static int Contains(char **list, int count, const char *name)
{
    int index;

    for (index = 0; index < count; ++index)
    {
        if (strcmp(list[index], name) == 0)
            return 1;
    }
    return 0;
}

static int CheckList(char **list, int count, int expected_count, const char *included, const char *excluded)
{
    return Check(count == expected_count, "unexpected file count")
        || Check((count == 0) == (list == NULL), "empty list return shape is wrong")
        || Check(list == NULL || list[count] == NULL, "list is not null terminated")
        || Check(included == NULL || Contains(list, count, included), "expected name is missing")
        || Check(excluded == NULL || !Contains(list, count, excluded), "unexpected name is present");
}

int main(void)
{
    const char *state_dir = getenv("KISAK_TEST_STATE_DIR");
    char fixture[4000];
    char empty[4000];
    char diagnostic[4000];
    char path[4096];
    char output[256];
    char **list;
    const char *untagged[1];
    FILE *file;
    int saved_stderr;
    int diagnostic_fd;
    int count;

    if (Check(state_dir != NULL && state_dir[0] != '\0', "KISAK_TEST_STATE_DIR is not set") ||
        Check(snprintf(fixture, sizeof(fixture), "%s/file-list-fixture", state_dir) >= 0 &&
            strlen(fixture) < sizeof(fixture) - 1, "fixture path is too long") ||
        Check(snprintf(empty, sizeof(empty), "%s/file-list-empty", state_dir) >= 0 &&
            strlen(empty) < sizeof(empty) - 1, "empty fixture path is too long") ||
        Check(snprintf(diagnostic, sizeof(diagnostic), "%s/file-list-diagnostic", state_dir) >= 0 &&
            strlen(diagnostic) < sizeof(diagnostic) - 1, "diagnostic path is too long"))
        return 1;

    snprintf(path, sizeof(path), "%s/archive.IwD", fixture); unlink(path);
    snprintf(path, sizeof(path), "%s/archive.iwd.bak", fixture); unlink(path);
    snprintf(path, sizeof(path), "%s/plain", fixture); unlink(path);
    snprintf(path, sizeof(path), "%s/CVS", fixture); unlink(path);
    snprintf(path, sizeof(path), "%s/subdir", fixture); rmdir(path);
    snprintf(path, sizeof(path), "%s/match.IWD", fixture); rmdir(path);
    snprintf(path, sizeof(path), "%s/folder.txt", fixture); rmdir(path);
    snprintf(path, sizeof(path), "%s/CVS", fixture); rmdir(path);
    snprintf(path, sizeof(path), "%s/cVs", fixture); rmdir(path);
    rmdir(fixture);
    rmdir(empty);

    if (mkdir(fixture, 0777) != 0 || mkdir(empty, 0777) != 0)
        return Check(0, "could not create file-list fixtures");
    snprintf(path, sizeof(path), "%s/archive.IwD", fixture); if (!CreateFile(path)) return Check(0, "could not create IWD fixture");
    snprintf(path, sizeof(path), "%s/archive.iwd.bak", fixture); if (!CreateFile(path)) return Check(0, "could not create suffix fixture");
    snprintf(path, sizeof(path), "%s/plain", fixture); if (!CreateFile(path)) return Check(0, "could not create ordinary fixture");
    snprintf(path, sizeof(path), "%s/CVS", fixture); if (!CreateFile(path)) return Check(0, "could not create CVS file fixture");
    snprintf(path, sizeof(path), "%s/subdir", fixture); if (mkdir(path, 0777) != 0) return Check(0, "could not create directory fixture");
    snprintf(path, sizeof(path), "%s/match.IWD", fixture); if (mkdir(path, 0777) != 0) return Check(0, "could not create extension directory fixture");
    snprintf(path, sizeof(path), "%s/folder.txt", fixture); if (mkdir(path, 0777) != 0) return Check(0, "could not create directory suffix fixture");
    list = Sys_ListFiles(fixture, NULL, NULL, &count, 0);
    if (CheckList(list, count, 4, "CVS", "subdir")) return 1;
    if (Switch_TryFreeFileList((const char **)list) != 1) return Check(0, "regular file list was not freed");

    list = Sys_ListFiles(fixture, "iwd", NULL, &count, 0);
    if (CheckList(list, count, 1, "archive.IwD", "archive.iwd.bak")) return 1;
    list[0][0] = 'A';
    if (Check(strcmp(list[0], "Archive.IwD") == 0, "returned names are not mutable") || Check(Switch_TryFreeFileList((const char **)list) == 1, "tagged list was not freed")) return 1;

    snprintf(path, sizeof(path), "%s/CVS", fixture); unlink(path);
    if (mkdir(path, 0777) != 0) return Check(0, "could not create CVS directory fixture");
    snprintf(path, sizeof(path), "%s/cVs", fixture);
    if (mkdir(path, 0777) != 0) return Check(0, "could not create mixed-case CVS directory fixture");

    list = Sys_ListFiles(fixture, NULL, NULL, &count, 0);
    if (CheckList(list, count, 3, "plain", "subdir")) return 1;
    if (Switch_TryFreeFileList((const char **)list) != 1) return Check(0, "ordinary list was not freed");
    list = Sys_ListFiles(fixture, NULL, NULL, &count, 1);
    if (CheckList(list, count, 3, "subdir", "CVS")) return 1;
    if (Check(Contains(list, count, "match.IWD") && Contains(list, count, "folder.txt") && !Contains(list, count, "cVs"), "directory selection or CVS exclusion is wrong")) return 1;
    if (Switch_TryFreeFileList((const char **)list) != 1) return Check(0, "directory list was not freed");
    list = Sys_ListFiles(fixture, "iwd", NULL, &count, 1);
    if (CheckList(list, count, 1, "match.IWD", NULL)) return 1;
    if (Switch_TryFreeFileList((const char **)list) != 1) return Check(0, "extension directory list was not freed");
    list = Sys_ListFiles(fixture, "/", NULL, &count, 0);
    if (CheckList(list, count, 3, "folder.txt", "plain")) return 1;
    if (Switch_TryFreeFileList((const char **)list) != 1) return Check(0, "slash directory list was not freed");

    count = 7;
    if (Check(Sys_ListFiles("/definitely/not/a/kisak/file-list", NULL, NULL, &count, 0) == NULL && count == 0, "missing directory did not return zero semantics")) return 1;
    count = 7;
    if (Check(Sys_ListFiles("", NULL, NULL, &count, 0) == NULL && count == 0, "empty directory did not return zero semantics")) return 1;
    count = 7;
    if (Check(Sys_ListFiles(empty, NULL, NULL, &count, 0) == NULL && count == 0, "empty fixture did not return zero semantics")) return 1;
    count = 7;
    if (Check(Sys_ListFiles(fixture, "cfg", NULL, &count, 0) == NULL && count == 0, "no match did not return zero semantics")) return 1;
    if (Check(Sys_ListFiles(NULL, NULL, NULL, &count, 0) == NULL && count == 0, "null directory did not return zero semantics") || Check(Sys_ListFiles(fixture, NULL, NULL, NULL, 0) == NULL, "null count was accepted")) return 1;

    saved_stderr = dup(STDERR_FILENO);
    diagnostic_fd = open(diagnostic, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (saved_stderr < 0 || diagnostic_fd < 0 || dup2(diagnostic_fd, STDERR_FILENO) < 0)
        return Check(0, "could not capture filter diagnostic");
    close(diagnostic_fd);
    count = 7;
    list = Sys_ListFiles(fixture, NULL, "*", &count, 0);
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0)
        return Check(0, "could not restore stderr");
    close(saved_stderr);
    file = fopen(diagnostic, "r");
    if (file == NULL || fgets(output, sizeof(output), file) == NULL)
        return Check(0, "filter rejection did not emit a diagnostic");
    fclose(file);
    if (Check(list == NULL && count == 0, "filter rejection did not return zero semantics") || Check(strstr(output, "filters are unsupported") != NULL, "filter rejection diagnostic changed")) return 1;

    untagged[0] = (const char *)(uintptr_t)2;
    if (Check(Switch_TryFreeFileList(NULL) == 1, "NULL free was not handled") || Check(Switch_TryFreeFileList(untagged + 1) == 0, "untagged aligned owner was freed")) return 1;

    return 0;
}
