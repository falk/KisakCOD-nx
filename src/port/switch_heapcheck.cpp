// Hardware heap checker (KISAK_SWITCH_HEAP_CHECK=ON builds only).
//
// Hardware-only newlib heap corruption (a free chunk's header overwritten,
// then _malloc_r/_free_r abort in whichever caller allocates next) cannot be
// watched with GDB on hardware and does not reproduce in emulators.  This
// links over newlib's
// reentrant allocator (-Wl,--wrap=_malloc_r,...) and gives every block
//
//   [real malloc chunk ... | Header (alloc/free stacks, 16-byte front
//    redzone) | user bytes | 32-byte tail redzone]
//
// Freed blocks are filled with kFreedByte and held in a FIFO quarantine;
// on eviction a changed byte is a use-after-free write.  A scan thread
// checks every live block's redzones every kScanMs.  Each finding prints
//
//   FAIL:HEAPCHK_<KIND> ptr=... size=... off=... bytes=... alloc=<stack> free=<stack>
//
// with absolute return addresses; HEAPCHK base=<addr of HeapCheck_Start>
// lets offline tooling rebase and symbolize them.  The
// stacks walk frame pointers, so build with KISAK_SWITCH_EXTRA_CFLAGS=
// "-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer".
//
// The damaged bytes are repaired after reporting so one run can report
// several writers.  Nothing here masks a bug in a normal build: without
// the option this file is not linked.

#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct _reent;

extern "C" {
void *__real__malloc_r(struct _reent *r, size_t size);
void __real__free_r(struct _reent *r, void *ptr);
void *__wrap__malloc_r(struct _reent *r, size_t size);
void __wrap__free_r(struct _reent *r, void *ptr);
void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size);
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size);
void *__wrap__memalign_r(struct _reent *r, size_t align, size_t size);
size_t __wrap__malloc_usable_size_r(struct _reent *r, void *ptr);
int Switch_NxlinkStdioActive(void) __attribute__((weak));
}

void HeapCheck_Start();

namespace
{
constexpr uint64_t kMagicLive = 0x4b4845415056494cull;  // "LIVPAEHK"
constexpr uint64_t kMagicFreed = 0x4b484541504445ffull;
constexpr uint8_t kFrontByte = 0xa5;
constexpr uint8_t kTailByte = 0xb6;
constexpr uint8_t kFreedByte = 0xfd;
constexpr size_t kFrontBytes = 16;
constexpr size_t kTailBytes = 32;
constexpr int kStack = 8; // the first ~3 frames are the wrappers themselves
constexpr size_t kQuarantineBytes = 24u << 20;
constexpr size_t kQuarantineMaxBlock = 1u << 20; // bigger blocks are checked and freed at once
constexpr uint64_t kScanMs = 2000;

struct Header
{
    Header *prev, *next;     // live list, or quarantine FIFO
    void *real;              // what __real__malloc_r returned
    uint64_t size;           // user bytes
    uint64_t allocStack[kStack];
    uint64_t freeStack[kStack];
    uint64_t pad;
    uint64_t magic;
    uint8_t front[kFrontBytes];
};
static_assert(sizeof(Header) % 16 == 0, "user pointer must stay 16-aligned");

Mutex s_lock;
Header s_live{};       // circular list sentinels
Header s_quarantine{};
size_t s_quarantined;
bool s_ready;
uint64_t s_findings;
uint64_t s_liveBlocks;

// Findings are formatted into a static ring under s_lock and printed by the
// scan thread, never from inside an allocation (printing allocates).
constexpr int kRing = 64;
char s_ring[kRing][512];
int s_ringHead, s_ringTail;
uint64_t s_ringLost;

Header *HeaderOf(void *user) { return reinterpret_cast<Header *>(static_cast<uint8_t *>(user) - sizeof(Header)); }
uint8_t *UserOf(Header *h) { return reinterpret_cast<uint8_t *>(h + 1); }

void Link(Header *list, Header *h)
{
    h->prev = list->prev;
    h->next = list;
    list->prev->next = h;
    list->prev = h;
}

void Unlink(Header *h)
{
    h->prev->next = h->next;
    h->next->prev = h->prev;
    h->prev = h->next = nullptr;
}

void CaptureStack(uint64_t *out)
{
    // Skip the wrapper's own frame: start at its caller's record.
    // Frame records only move up the stack from this function's own.
    const uint64_t *fp = static_cast<const uint64_t *>(__builtin_frame_address(0));
    const uintptr_t sp = reinterpret_cast<uintptr_t>(fp);
    int n = 0;
    for (int guard = 0; n < kStack && guard < kStack + 2; ++guard)
    {
        const uintptr_t f = reinterpret_cast<uintptr_t>(fp);
        if (!f || (f & 15) || f < sp || f > sp + (1u << 20))
            break;
        const uint64_t ret = fp[1];
        if (!ret)
            break;
        out[n++] = ret;
        const uint64_t *next = reinterpret_cast<const uint64_t *>(fp[0]);
        if (next <= fp)
            break;
        fp = next;
    }
    for (; n < kStack; ++n)
        out[n] = 0;
}

int FormatStack(char *buf, size_t len, const uint64_t *stack)
{
    int n = 0;
    for (int i = 0; i < kStack && stack[i] && n < (int)len; ++i)
        n += snprintf(buf + n, len - n, i ? ",%llx" : "%llx", (unsigned long long)stack[i]);
    if (!n && len)
        n = snprintf(buf, len, "-");
    return n;
}

// Caller holds s_lock.
void Report(const char *kind, Header *h, long long off, const uint8_t *bytes, size_t count)
{
    ++s_findings;
    const int next = (s_ringHead + 1) % kRing;
    if (next == s_ringTail)
    {
        ++s_ringLost;
        return;
    }
    char *line = s_ring[s_ringHead];
    const size_t len = sizeof(s_ring[0]);
    int n = snprintf(line, len, "FAIL:HEAPCHK_%s ptr=%p size=%llu off=%lld bytes=", kind, (void *)UserOf(h),
                     (unsigned long long)h->size, off);
    for (size_t i = 0; i < count && n < (int)len - 4; ++i)
        n += snprintf(line + n, len - n, "%02x", bytes[i]);
    n += snprintf(line + n, len - n, " alloc=");
    n += FormatStack(line + n, len - n, h->allocStack);
    n += snprintf(line + n, len - n, " free=");
    n += FormatStack(line + n, len - n, h->freeStack);
    snprintf(line + n, len - n, "\n");
    s_ringHead = next;
}

// First byte in [p, p+len) that is not `value`, or len.
size_t FirstMismatch(const uint8_t *p, size_t len, uint8_t value)
{
    size_t i = 0;
    const uint64_t word = 0x0101010101010101ull * value;
    while (i < len && ((uintptr_t)(p + i) & 7))
    {
        if (p[i] != value)
            return i;
        ++i;
    }
    for (; i + 8 <= len; i += 8)
    {
        if (*reinterpret_cast<const uint64_t *>(p + i) != word)
            break;
    }
    for (; i < len; ++i)
    {
        if (p[i] != value)
            return i;
    }
    return len;
}

// Checks one block's redzones (and, if freed, its body); repairs and
// reports damage.  Caller holds s_lock.
void CheckBlock(Header *h, bool freed)
{
    size_t bad = FirstMismatch(h->front, kFrontBytes, kFrontByte);
    if (bad < kFrontBytes)
    {
        Report("FRONT", h, (long long)bad - (long long)kFrontBytes, h->front + bad,
               kFrontBytes - bad < 16 ? kFrontBytes - bad : 16);
        memset(h->front, kFrontByte, kFrontBytes);
    }
    uint8_t *tail = UserOf(h) + h->size;
    bad = FirstMismatch(tail, kTailBytes, kTailByte);
    if (bad < kTailBytes)
    {
        Report("TAIL", h, h->size + bad, tail + bad, kTailBytes - bad < 16 ? kTailBytes - bad : 16);
        memset(tail, kTailByte, kTailBytes);
    }
    if (freed)
    {
        bad = FirstMismatch(UserOf(h), h->size, kFreedByte);
        if (bad < h->size)
        {
            Report("UAF", h, bad, UserOf(h) + bad, h->size - bad < 16 ? h->size - bad : 16);
            memset(UserOf(h), kFreedByte, h->size);
        }
    }
}

void *Allocate(struct _reent *r, size_t align, size_t size)
{
    if (align < 16)
        align = 16;
    const size_t total = sizeof(Header) + align + size + kTailBytes;
    if (total < size)
        return nullptr;
    uint8_t *real = static_cast<uint8_t *>(__real__malloc_r(r, total));
    if (!real)
        return nullptr;
    uintptr_t user = reinterpret_cast<uintptr_t>(real) + sizeof(Header);
    user = (user + align - 1) & ~(uintptr_t)(align - 1);
    Header *h = HeaderOf(reinterpret_cast<void *>(user));
    h->real = real;
    h->size = size;
    CaptureStack(h->allocStack);
    memset(h->freeStack, 0, sizeof(h->freeStack));
    memset(h->front, kFrontByte, kFrontBytes);
    memset(UserOf(h) + size, kTailByte, kTailBytes);
    h->magic = kMagicLive;
    mutexLock(&s_lock);
    if (!s_ready)
    {
        s_live.prev = s_live.next = &s_live;
        s_quarantine.prev = s_quarantine.next = &s_quarantine;
        s_ready = true;
    }
    Link(&s_live, h);
    ++s_liveBlocks;
    mutexUnlock(&s_lock);
    return UserOf(h);
}

void Release(struct _reent *r, void *ptr, const uint64_t *stack)
{
    Header *h = HeaderOf(ptr);
    mutexLock(&s_lock);
    if (h->magic == kMagicFreed)
    {
        uint8_t none = 0;
        memcpy(h->freeStack, stack, sizeof(h->freeStack));
        Report("DOUBLE_FREE", h, 0, &none, 0);
        mutexUnlock(&s_lock);
        return;
    }
    CheckBlock(h, false);
    Unlink(h);
    --s_liveBlocks;
    h->magic = kMagicFreed;
    memcpy(h->freeStack, stack, sizeof(h->freeStack));
    Header *evict = nullptr;
    if (h->size <= kQuarantineMaxBlock)
    {
        memset(ptr, kFreedByte, h->size);
        Link(&s_quarantine, h);
        s_quarantined += h->size;
        h = nullptr;
    }
    // Evict oldest until under budget; check each as it leaves.
    while (s_quarantined > kQuarantineBytes && s_quarantine.next != &s_quarantine)
    {
        evict = s_quarantine.next;
        CheckBlock(evict, true);
        Unlink(evict);
        s_quarantined -= evict->size;
        evict->magic = 0;
        mutexUnlock(&s_lock);
        __real__free_r(r, evict->real);
        mutexLock(&s_lock);
    }
    mutexUnlock(&s_lock);
    if (h)
    {
        h->magic = 0;
        __real__free_r(r, h->real);
    }
}

void Emit(const char *line)
{
    if (Switch_NxlinkStdioActive && Switch_NxlinkStdioActive())
    {
        fputs(line, stdout);
        fflush(stdout);
    }
    else
    {
        svcOutputDebugString(line, strlen(line));
    }
}

void ScanMain(void *)
{
    char line[512];
    snprintf(line, sizeof(line), "HEAPCHK base=%p quarantine=%zu scan_ms=%llu\n",
             reinterpret_cast<void *>(&HeapCheck_Start), kQuarantineBytes, (unsigned long long)kScanMs);
    Emit(line);
    uint64_t scans = 0;
    for (;;)
    {
        svcSleepThread((int64_t)kScanMs * 1000000);
        mutexLock(&s_lock);
        if (s_ready)
        {
            for (Header *h = s_live.next; h != &s_live; h = h->next)
                CheckBlock(h, false);
        }
        const uint64_t live = s_liveBlocks, findings = s_findings, lost = s_ringLost;
        const size_t quarantined = s_quarantined;
        mutexUnlock(&s_lock);
        for (;;)
        {
            mutexLock(&s_lock);
            const bool any = s_ringTail != s_ringHead;
            if (any)
            {
                memcpy(line, s_ring[s_ringTail], sizeof(line));
                s_ringTail = (s_ringTail + 1) % kRing;
            }
            mutexUnlock(&s_lock);
            if (!any)
                break;
            Emit(line);
        }
        if (++scans % 15 == 0)
        {
            snprintf(line, sizeof(line), "HEAPCHK scan live=%llu quarantined=%zu findings=%llu lost=%llu\n",
                     (unsigned long long)live, quarantined, (unsigned long long)findings,
                     (unsigned long long)lost);
            Emit(line);
        }
    }
}

Thread s_scanThread;
} // namespace

extern "C" {

void *__wrap__malloc_r(struct _reent *r, size_t size)
{
    return Allocate(r, 16, size);
}

void __wrap__free_r(struct _reent *r, void *ptr)
{
    if (!ptr)
        return;
    const uint64_t magic = HeaderOf(ptr)->magic;
    if (magic != kMagicLive && magic != kMagicFreed)
    {
        // newlib's own internal frees (mallocr.o releasing an old top
        // chunk) hand the real allocator's pointers straight back here.
        __real__free_r(r, ptr);
        return;
    }
    uint64_t stack[kStack];
    CaptureStack(stack);
    Release(r, ptr, stack);
}

void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size)
{
    if (!ptr)
        return Allocate(r, 16, size);
    if (!size)
    {
        __wrap__free_r(r, ptr);
        return nullptr;
    }
    void *fresh = Allocate(r, 16, size);
    if (!fresh)
        return nullptr;
    const size_t old = HeaderOf(ptr)->size;
    memcpy(fresh, ptr, old < size ? old : size);
    __wrap__free_r(r, ptr);
    return fresh;
}

void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size)
        return nullptr;
    void *p = Allocate(r, 16, n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void *__wrap__memalign_r(struct _reent *r, size_t align, size_t size)
{
    if (align & (align - 1))
        return nullptr;
    return Allocate(r, align, size);
}

size_t __wrap__malloc_usable_size_r(struct _reent *, void *ptr)
{
    return ptr ? HeaderOf(ptr)->size : 0;
}

} // extern "C"

void HeapCheck_Start()
{
    if (R_FAILED(threadCreate(&s_scanThread, ScanMain, nullptr, nullptr, 0x8000, 0x2E, 1)) ||
        R_FAILED(threadStart(&s_scanThread)))
        Emit("FAIL:HEAPCHK_START scan thread not started\n");
}
