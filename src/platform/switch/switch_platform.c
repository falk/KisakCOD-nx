#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "switch_platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#ifdef __cplusplus
#include "src/qcommon/com_error.h"
#endif

#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/mman.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#endif

void __cdecl Sys_LoadingKeepAlive(void) {}
/* Sys_SuspendOtherThreads used to be an abort() trap here with C linkage.
 * Nothing in C ever called it, so it was dead: the engine's C++ callers bind
 * switch_thread_sync.cpp's real implementation, which suspends the other
 * threads by handle. */
void __cdecl Sys_OpenURL(const char *url, int doexit) { (void)url; (void)doexit; abort(); }

static int switch_time_base_initialized;
static uint32_t switch_time_base_ms;

#define SWITCH_FILE_LIST_MAX 0x1FFF
#define SWITCH_FILE_LIST_TAG ((uintptr_t)1)

typedef struct
{
    char **allocation;
    int count;
} SwitchFileListOwner;

#define SWITCH_VIRTUAL_PAGE_SIZE ((size_t)4096)

typedef struct SwitchVirtualReservation
{
    void *base;
    size_t bytes;
    unsigned char *committed_pages;
#ifdef __SWITCH__
    void *backing;
#endif
    struct SwitchVirtualReservation *next;

} SwitchVirtualReservation;

static SwitchVirtualReservation *switch_virtual_reservations;

#if !defined(__SWITCH__)
uint64_t (*Switch_HostMonotonicNanoseconds)(void);
#endif

static int Switch_VirtualBytesAreValid(size_t bytes)
{
    return bytes != 0 && bytes % SWITCH_VIRTUAL_PAGE_SIZE == 0;
}

static SwitchVirtualReservation *Switch_FindVirtualSubrange(void *address, size_t bytes, size_t *first_page, size_t *page_count)
{
    SwitchVirtualReservation *reservation;
    uintptr_t base;
    uintptr_t target;
    size_t offset;

    if (address == NULL || !Switch_VirtualBytesAreValid(bytes))
        return NULL;
    target = (uintptr_t)address;
    if (target % SWITCH_VIRTUAL_PAGE_SIZE != 0)
        return NULL;

    for (reservation = switch_virtual_reservations; reservation != NULL; reservation = reservation->next)
    {
        base = (uintptr_t)reservation->base;
        if (target < base)
            continue;
        offset = (size_t)(target - base);
        if (offset > reservation->bytes || bytes > reservation->bytes - offset)
            continue;
        *first_page = offset / SWITCH_VIRTUAL_PAGE_SIZE;
        *page_count = bytes / SWITCH_VIRTUAL_PAGE_SIZE;
        return reservation;
    }
    return NULL;
}

void *Switch_VirtualReserve(size_t bytes)
{
    SwitchVirtualReservation *reservation;
    void *base;

    if (!Switch_VirtualBytesAreValid(bytes))
        return NULL;

#ifdef __SWITCH__
    void *backing;

    backing = aligned_alloc(SWITCH_VIRTUAL_PAGE_SIZE, bytes);
    if (backing == NULL)
        return NULL;
    memset(backing, 0, bytes);

    base = backing;
#else
    base = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
#endif

    if ((uintptr_t)base % SWITCH_VIRTUAL_PAGE_SIZE != 0 || (uintptr_t)base > UINTPTR_MAX - bytes)
    {
#ifdef __SWITCH__
        free(backing);
#else
        munmap(base, bytes);
#endif
        return NULL;
    }

    reservation = (SwitchVirtualReservation *)calloc(1, sizeof(*reservation));
    if (reservation == NULL)
    {
#ifdef __SWITCH__
        free(backing);
#else
        munmap(base, bytes);
#endif
        return NULL;
    }
    reservation->committed_pages = (unsigned char *)calloc(bytes / SWITCH_VIRTUAL_PAGE_SIZE, 1);
    if (reservation->committed_pages == NULL)
    {
        free(reservation);
#ifdef __SWITCH__
        free(backing);
#else
        munmap(base, bytes);
#endif
        return NULL;
    }

    reservation->base = base;
    reservation->bytes = bytes;
#ifdef __SWITCH__
    reservation->backing = backing;
#endif
    reservation->next = switch_virtual_reservations;
    switch_virtual_reservations = reservation;
    return base;
}

int Switch_VirtualCommit(void *address, size_t bytes)
{
    SwitchVirtualReservation *reservation;
    size_t first_page;
    size_t page_count;
#ifdef __SWITCH__
    size_t page;
#endif

    reservation = Switch_FindVirtualSubrange(address, bytes, &first_page, &page_count);
    if (reservation == NULL)
        return 0;

#ifdef __SWITCH__
    for (page = 0; page < page_count; ++page)
        if (reservation->committed_pages[first_page + page] == 0)
        {
            memset((unsigned char *)reservation->base + (first_page + page) * SWITCH_VIRTUAL_PAGE_SIZE, 0,
                   SWITCH_VIRTUAL_PAGE_SIZE);
            reservation->committed_pages[first_page + page] = 1;
        }
#else
    if (mprotect(address, bytes, PROT_READ | PROT_WRITE) != 0)
        return 0;
    memset(reservation->committed_pages + first_page, 1, page_count);
#endif
    return 1;
}

int Switch_VirtualDecommit(void *address, size_t bytes)
{
    SwitchVirtualReservation *reservation;
    size_t first_page;
    size_t page_count;
#ifdef __SWITCH__
    size_t page;
#endif

    reservation = Switch_FindVirtualSubrange(address, bytes, &first_page, &page_count);
    if (reservation == NULL)
        return 0;

#ifdef __SWITCH__
    for (page = 0; page < page_count; ++page)
        if (reservation->committed_pages[first_page + page] != 0)
        {
            memset((unsigned char *)reservation->base + (first_page + page) * SWITCH_VIRTUAL_PAGE_SIZE, 0,
                   SWITCH_VIRTUAL_PAGE_SIZE);
            reservation->committed_pages[first_page + page] = 0;
        }
#else
    if (madvise(address, bytes, MADV_DONTNEED) != 0 || mprotect(address, bytes, PROT_NONE) != 0)
        return 0;
    memset(reservation->committed_pages + first_page, 0, page_count);
#endif
    return 1;
}

int Switch_VirtualRelease(void *base)
{
    SwitchVirtualReservation **link;
    SwitchVirtualReservation *reservation;

    if (base == NULL)
        return 0;
    for (link = &switch_virtual_reservations; *link != NULL; link = &(*link)->next)
    {
        if ((*link)->base == base)
            break;
    }
    if (*link == NULL)
        return 0;
    reservation = *link;

#ifdef __SWITCH__
    /* The page-tracked backing arena is the reservation itself; commit and
       decommit are bookkeeping plus zeroing, so release is a plain free. */
    free(reservation->backing);
#else
    if (munmap(reservation->base, reservation->bytes) != 0)
        return 0;
#endif

    *link = reservation->next;
    free(reservation->committed_pages);
    free(reservation);
    return 1;
}

static uint64_t Switch_MonotonicNanoseconds(void)
{
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick());
#else
    struct timespec time;

    if (Switch_HostMonotonicNanoseconds != NULL)
        return Switch_HostMonotonicNanoseconds();

    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
        return 0;

    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
#endif
}

static uint64_t Switch_MonotonicMilliseconds(void)
{
    return Switch_MonotonicNanoseconds() / UINT64_C(1000000);
}

static int Switch_AsciiCaseEqual(const char *left, const char *right)
{
    unsigned char left_char;
    unsigned char right_char;

    do
    {
        left_char = (unsigned char)*left++;
        right_char = (unsigned char)*right++;
        if (left_char >= 'A' && left_char <= 'Z')
            left_char += 'a' - 'A';
        if (right_char >= 'A' && right_char <= 'Z')
            right_char += 'a' - 'A';
    } while (left_char == right_char && left_char != '\0');

    return left_char == right_char;
}

static int Switch_HasExtension(const char *name, const char *extension)
{
    size_t name_length = strlen(name);
    size_t extension_length = strlen(extension);

    return name_length > extension_length
        && name[name_length - extension_length - 1] == '.'
        && Switch_AsciiCaseEqual(name + name_length - extension_length, extension);
}

static char *Switch_JoinPath(const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    size_t separator = directory_length != 0 && directory[directory_length - 1] != '/';
    size_t length;
    char *path;

    if (directory_length > SIZE_MAX - name_length - separator - 1)
        return NULL;

    length = directory_length + separator + name_length + 1;
    path = (char *)malloc(length);
    if (path == NULL)
        return NULL;

    memcpy(path, directory, directory_length);
    if (separator)
        path[directory_length++] = '/';
    memcpy(path + directory_length, name, name_length + 1);
    return path;
}

int Switch_TryFreeFileList(const char **list)
{
    uintptr_t tagged_owner;
    SwitchFileListOwner *owner;
    int index;

    if (list == NULL)
        return 1;

    tagged_owner = (uintptr_t)list[-1];
    if ((tagged_owner & SWITCH_FILE_LIST_TAG) == 0)
        return 0;

    owner = (SwitchFileListOwner *)(tagged_owner & ~SWITCH_FILE_LIST_TAG);
    for (index = 0; index < owner->count; ++index)
        free((void *)list[index]);
    free(owner->allocation);
    free(owner);
    return 1;
}

char ** __cdecl Sys_ListFiles(const char *directory, const char *extension, const char *filter, int *numfiles, int wantsubs)
{
    DIR *dir;
    struct dirent *entry;
    char **names = NULL;
    int count = 0;
    int capacity = 0;
    int directories_only;

    if (numfiles != NULL)
        *numfiles = 0;
    if (directory == NULL || *directory == '\0' || numfiles == NULL)
        return NULL;
    if (filter != NULL)
    {
        Sys_Print("Sys_ListFiles: filters are unsupported on Switch\n");
        return NULL;
    }

    if (extension == NULL)
        extension = "";
    directories_only = strcmp(extension, "/") == 0;
    if (directories_only)
        extension = "";

    dir = opendir(directory);
    if (dir == NULL)
        return NULL;

    while (count < SWITCH_FILE_LIST_MAX && (entry = readdir(dir)) != NULL)
    {
        struct stat status;
        char *path;
        char *name;
        int is_directory;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        path = Switch_JoinPath(directory, entry->d_name);
        if (path == NULL)
            goto failure;
        if (stat(path, &status) != 0)
        {
            free(path);
            continue;
        }
        free(path);

        is_directory = S_ISDIR(status.st_mode);
        if (!is_directory && !S_ISREG(status.st_mode))
            continue;
        if (is_directory && Switch_AsciiCaseEqual(entry->d_name, "CVS"))
            continue;
        if ((directories_only || wantsubs) != is_directory)
            continue;
        if (*extension != '\0' && !Switch_HasExtension(entry->d_name, extension))
            continue;

        if (count == capacity)
        {
            int new_capacity = capacity == 0 ? 16 : capacity * 2;
            char **new_names;

            if (new_capacity > SWITCH_FILE_LIST_MAX)
                new_capacity = SWITCH_FILE_LIST_MAX;
            new_names = (char **)realloc(names, (size_t)new_capacity * sizeof(*names));
            if (new_names == NULL)
                goto failure;
            names = new_names;
            capacity = new_capacity;
        }
        name = (char *)malloc(strlen(entry->d_name) + 1);
        if (name == NULL)
            goto failure;
        strcpy(name, entry->d_name);
        names[count++] = name;
    }
    closedir(dir);

    if (count != 0)
    {
        SwitchFileListOwner *owner;
        char **allocation;
        char **list;

        allocation = (char **)malloc((size_t)(count + 2) * sizeof(*allocation));
        owner = (SwitchFileListOwner *)malloc(sizeof(*owner));
        if (allocation == NULL || owner == NULL)
        {
            free(allocation);
            free(owner);
            goto failure_without_directory;
        }
        list = allocation + 1;
        owner->allocation = allocation;
        owner->count = count;
        allocation[0] = (char *)((uintptr_t)owner | SWITCH_FILE_LIST_TAG);
        memcpy(list, names, (size_t)count * sizeof(*names));
        list[count] = NULL;
        free(names);
        *numfiles = count;
        return list;
    }

    free(names);
    return NULL;

failure:
    closedir(dir);
failure_without_directory:
    while (count > 0)
        free(names[--count]);
    free(names);
    return NULL;
}

void __cdecl Sys_Print(const char *msg)
{
    if (msg == NULL)
        return;

#ifdef __SWITCH__
    static int at_line_start = 1;
    const char *cursor = msg;
    /* Weak by design: the applet entry point (switch_sp_main.cpp) defines the
     * real probe.  The standalone aarch64 proof ELFs in `./test switch-build`
     * (skeleton, thread, event, ...) link this file without that entry point
     * and have no nxlink stdio at all, so an absent symbol has to mean "not
     * active" rather than an undefined-reference link error.  The NULL
     * comparison keeps the compiler from folding this call to a constant in
     * the real build, where the strong definition wins at link time. */
    extern int Switch_NxlinkStdioActive(void) __attribute__((weak));

    /* Without nxlink, stdout goes nowhere: emulator logs (and the verifiers that
     * parse them) only see svcOutputDebugString, so that stays the path
     * until nxlinkStdio() has redirected stdout to a host. */
    if (Switch_NxlinkStdioActive == NULL || !Switch_NxlinkStdioActive())
    {
        svcOutputDebugString(msg, strlen(msg));
        return;
    }

    /* stdout is redirected by nxlinkStdio().  Sending the same bytes through
     * svcOutputDebugString as well made every engine line appear twice and
     * doubled the cost of diagnostic-heavy runs.  Prefix complete logical
     * lines here so fragmented Com_Printf calls still form readable output. */
    while (*cursor)
    {
        const char *newline;
        size_t bytes;

        if (at_line_start)
        {
            char prefix[32];
            uint32_t milliseconds = Sys_Milliseconds();
            int prefix_bytes = snprintf(
                prefix, sizeof(prefix), "[%10u.%03u] ",
                milliseconds / 1000u, milliseconds % 1000u);
            if (prefix_bytes > 0)
                fwrite(prefix, 1, (size_t)prefix_bytes, stdout);
            at_line_start = 0;
        }

        newline = strchr(cursor, '\n');
        bytes = newline ? (size_t)(newline - cursor + 1) : strlen(cursor);
        fwrite(cursor, 1, bytes, stdout);
        cursor += bytes;
        if (newline)
            at_line_start = 1;
    }
    fflush(stdout);
#else
    fwrite(msg, 1, strlen(msg), stderr);
#endif
}

/* Default-off: Switch_BootLog is the sink for dense, unconditional
 * per-frame/per-draw narration traces added during renderer bring-up
 * (RDIP, R_HW_SetPixelShader/SetVertexShader, RHWSVSC/RHWSPXC,
 * SCR_UpdateFrame/SCR_UpdateScreen/SCR_DrawScreenField, and similar).
 * Left on, those dominate guest log volume (hundreds of thousands of
 * lines per run) with no cost/benefit for a normal run. Flip to true only
 * while actively debugging one of those paths; unrelated diagnostics
 * (PASS:/FAIL: markers, KILLHOUSE_* proofs) go through Com_Printf/
 * Sys_Print instead and are unaffected by this flag. */
static int s_switchBootLogVerbose = 0;

/* Weak: most host-test/proof binaries link this file alone, without
 * switch_port_log.cpp; an absent symbol means "not linked", not a link
 * error (same convention as Switch_NxlinkStdioActive above). */
extern void Port_Log(const char *msg) __attribute__((weak));

void __cdecl Switch_BootLog(const char *msg)
{
    if (msg == NULL || !s_switchBootLogVerbose)
        return;

    if (Port_Log)
        Port_Log(msg);
}

char * __cdecl Sys_Cwd(void)
{
#ifdef __SWITCH__
    /* libnx mounts sdmc here; never inherit an incidental process cwd. */
    static char cwd[] = "sdmc:/switch/kisakcod";

    return cwd;
#else
    static char cwd[4096];

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        cwd[0] = '\0';

    return cwd;
#endif
}

const char * __cdecl Sys_DefaultCDPath(void)
{
    return "";
}

void __cdecl Sys_Mkdir(const char *path)
{
    if (path == NULL || *path == '\0')
        return;

    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        return;
}

void __cdecl FS_ReplaceSeparators(char *path)
{
    char *source;
    char *destination;
    int was_separator = 0;

    if (path == NULL)
        return;

    source = path;
    destination = path;
    while (*source != '\0')
    {
        if (*source == '/' || *source == '\\')
        {
            if (!was_separator)
            {
                *destination++ = '/';
                was_separator = 1;
            }
        }
        else
        {
            *destination++ = *source;
            was_separator = 0;
        }
        ++source;
    }
    *destination = '\0';
}

static void Switch_PrintInvalidPath(const char *path)
{
    Sys_Print("WARNING: refusing to create relative path \"");
    Sys_Print(path != NULL ? path : "(null)");
    Sys_Print("\"\n");
}

int __cdecl FS_CreatePath(char *OSPath)
{
    char *last_separator;
    char *separator;

    if (OSPath == NULL || *OSPath == '\0')
    {
        Switch_PrintInvalidPath(OSPath);
        return 1;
    }

    FS_ReplaceSeparators(OSPath);
    if (strstr(OSPath, "..") != NULL || strstr(OSPath, "::") != NULL)
    {
        Switch_PrintInvalidPath(OSPath);
        return 1;
    }

    last_separator = strrchr(OSPath, '/');
    if (last_separator == NULL)
        return 0;

    for (separator = OSPath; separator <= last_separator; ++separator)
    {
        char saved;

        if (*separator != '/' || separator == OSPath || separator[-1] == ':')
            continue;
        saved = *separator;
        *separator = '\0';
        Sys_Mkdir(OSPath);
        *separator = saved;
    }
    return 0;
}

int __cdecl Sys_DirectoryHasContents(const char *directory)
{
    DIR *dir;
    struct dirent *entry;

    if (directory == NULL || *directory == '\0')
        return 0;

    dir = opendir(directory);
    if (dir == NULL)
        return 0;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 && !Switch_AsciiCaseEqual(entry->d_name, "CVS"))
        {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

uint32_t __cdecl Sys_Milliseconds(void)
{
    uint32_t now = (uint32_t)Switch_MonotonicMilliseconds();

    if (!switch_time_base_initialized)
    {
        switch_time_base_ms = now;
        switch_time_base_initialized = 1;
    }

    return now - switch_time_base_ms;
}

uint32_t __cdecl Sys_MillisecondsRaw(void)
{
    return (uint32_t)Switch_MonotonicMilliseconds();
}
