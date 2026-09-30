#define _POSIX_C_SOURCE 200809L

#include "src/platform/switch/switch_platform.h"

#include <fcntl.h>
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

static int IsDirectory(const char *path)
{
    struct stat status;

    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static int CheckRejectedPaths(const char *capture_path, char *empty, char *dotdot, char *colons)
{
    char actual[2048];
    char expected[2048];
    FILE *capture;
    int capture_fd;
    int saved_stderr;
    int expected_length;
    int restore_failed;
    size_t actual_length;
    int rejected;

    if (fflush(stderr) != 0 || (saved_stderr = dup(STDERR_FILENO)) < 0)
        return Check(0, "could not save stderr");
    capture_fd = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (capture_fd < 0)
    {
        close(saved_stderr);
        return Check(0, "could not open rejected-path capture");
    }
    if (dup2(capture_fd, STDERR_FILENO) < 0)
    {
        close(capture_fd);
        close(saved_stderr);
        return Check(0, "could not capture stderr");
    }

    rejected = FS_CreatePath(NULL) != 0 &&
        FS_CreatePath(empty) != 0 &&
        FS_CreatePath(dotdot) != 0 &&
        FS_CreatePath(colons) != 0;
    restore_failed = fflush(stderr) != 0;
    if (dup2(saved_stderr, STDERR_FILENO) < 0)
        restore_failed = 1;
    if (close(capture_fd) != 0)
        restore_failed = 1;
    if (close(saved_stderr) != 0)
        restore_failed = 1;
    if (restore_failed)
        return Check(0, "could not restore stderr");
    if (!rejected)
        return Check(0, "a rejected path was accepted");

    capture = fopen(capture_path, "r");
    if (capture == NULL)
        return Check(0, "could not read rejected-path capture");
    actual_length = fread(actual, 1, sizeof(actual) - 1, capture);
    if (ferror(capture) || fclose(capture) != 0)
        return Check(0, "could not read rejected-path capture");
    actual[actual_length] = '\0';
    expected_length = snprintf(expected, sizeof(expected),
        "WARNING: refusing to create relative path \"(null)\"\n"
        "WARNING: refusing to create relative path \"\"\n"
        "WARNING: refusing to create relative path \"%s\"\n"
        "WARNING: refusing to create relative path \"%s\"\n", dotdot, colons);
    if (expected_length < 0 || (size_t)expected_length >= sizeof(expected) ||
        actual_length != (size_t)expected_length || memcmp(actual, expected, actual_length) != 0)
        return Check(0, "rejected-path warnings did not match");
    return 0;
}

int main(void)
{
    const char *state_dir = getenv("KISAK_TEST_STATE_DIR");
    char normalized[] = "alpha\\\\//beta///gamma";
    char sdmc_path[] = "sdmc:\\\\switch///kisakcod";
    char empty[] = "";
    char fixture[4000];
    char capture[4000];
    char nested[4096];
    char expected_nested[4096];
    char parent_one[4096];
    char parent_two[4096];
    char rejected_dotdot[4096];
    char rejected_dotdot_dir[4096];
    char rejected_colons[4096];
    char rejected_colons_dir[4096];

    if (Check(state_dir != NULL && state_dir[0] != '\0', "KISAK_TEST_STATE_DIR is not set") ||
        Check(snprintf(fixture, sizeof(fixture), "%s/path-fixture", state_dir) >= 0 &&
            strlen(fixture) < sizeof(fixture) - 1, "fixture path is too long") ||
        Check(snprintf(capture, sizeof(capture), "%s/path-rejections.log", state_dir) >= 0 &&
            strlen(capture) < sizeof(capture) - 1, "capture path is too long"))
        return 1;

    snprintf(parent_two, sizeof(parent_two), "%s/one/two", fixture);
    snprintf(parent_one, sizeof(parent_one), "%s/one", fixture);
    rmdir(parent_two);
    rmdir(parent_one);
    rmdir(fixture);
    Sys_Mkdir(fixture);

    FS_ReplaceSeparators(normalized);
    FS_ReplaceSeparators(sdmc_path);
    FS_ReplaceSeparators(NULL);
    FS_ReplaceSeparators(empty);
    if (Check(strcmp(normalized, "alpha/beta/gamma") == 0, "mixed separators were not normalized") ||
        Check(strcmp(sdmc_path, "sdmc:/switch/kisakcod") == 0, "sdmc prefix was not preserved") ||
        Check(empty[0] == '\0', "empty path changed during normalization"))
        return 1;

    snprintf(nested, sizeof(nested), "%s\\one//two\\output.cfg", fixture);
    snprintf(expected_nested, sizeof(expected_nested), "%s/one/two/output.cfg", fixture);
    if (Check(FS_CreatePath(nested) == 0, "nested parent path was rejected") ||
        Check(strcmp(nested, expected_nested) == 0, "CreatePath did not normalize input") ||
        Check(IsDirectory(parent_one), "first parent was not created") ||
        Check(IsDirectory(parent_two), "nested parent was not created") ||
        Check(access(nested, F_OK) != 0, "final filename component was created") ||
        Check(FS_CreatePath(nested) == 0, "existing parents were rejected"))
        return 1;

    snprintf(rejected_dotdot, sizeof(rejected_dotdot), "%s/reject..path/child/file", fixture);
    snprintf(rejected_colons, sizeof(rejected_colons), "%s/reject::path/child/file", fixture);
    snprintf(rejected_dotdot_dir, sizeof(rejected_dotdot_dir), "%s/reject..path", fixture);
    snprintf(rejected_colons_dir, sizeof(rejected_colons_dir), "%s/reject::path", fixture);
    if (CheckRejectedPaths(capture, empty, rejected_dotdot, rejected_colons) ||
        Check(!IsDirectory(rejected_dotdot_dir), "dotdot path created a directory") ||
        Check(!IsDirectory(rejected_colons_dir), "double-colon path created a directory"))
        return 1;

    rmdir(parent_two);
    rmdir(parent_one);
    rmdir(fixture);
    return 0;
}
