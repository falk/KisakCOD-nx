#pragma once

// Non-blocking log transport. Producers (any thread, any context that may
// print) copy into a bounded ring and return; one drain thread owns every
// slow sink (nxlink socket, SD card). A stalled sink costs dropped lines
// (counted, reported as LOG_DROPPED) and never a stalled game thread.
// Header-only and platform-neutral so the host tests run the real code.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class LogRing
{
public:
    LogRing(char *storage, size_t capacity) : m_buf(storage), m_cap(capacity) {}

    // Whole message or nothing. The lock covers one memcpy; a producer that
    // cannot take it within a few spins drops instead of waiting, so a
    // descheduled holder can never stall a game thread.
    bool Push(const char *data, size_t len)
    {
        if (len == 0)
            return true;
        if (len > m_cap / 2)
            len = m_cap / 2;
        if (!Lock())
        {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const bool fits = m_cap - m_used >= len;
        if (fits)
        {
            const size_t first = m_cap - m_head < len ? m_cap - m_head : len;
            std::memcpy(m_buf + m_head, data, first);
            if (first < len)
                std::memcpy(m_buf, data + first, len - first);
            m_head = (m_head + len) % m_cap;
            m_used += len;
        }
        Unlock();
        if (!fits)
            m_dropped.fetch_add(1, std::memory_order_relaxed);
        return fits;
    }

    size_t Pop(char *out, size_t max)
    {
        Lock(~0u);
        size_t n = m_used < max ? m_used : max;
        const size_t first = m_cap - m_tail < n ? m_cap - m_tail : n;
        std::memcpy(out, m_buf + m_tail, first);
        if (first < n)
            std::memcpy(out + first, m_buf, n - first);
        m_tail = (m_tail + n) % m_cap;
        m_used -= n;
        Unlock();
        return n;
    }

    uint64_t TakeDropped() { return m_dropped.exchange(0, std::memory_order_relaxed); }

private:
    bool Lock(uint32_t spins = 64)
    {
        while (m_lock.test_and_set(std::memory_order_acquire))
            if (spins-- == 0)
                return false;
        return true;
    }
    void Unlock() { m_lock.clear(std::memory_order_release); }

    char *m_buf;
    size_t m_cap;
    size_t m_head = 0, m_tail = 0, m_used = 0;
    std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> m_dropped{0};
};

// Moves ring bytes to the file sink (always) and the network sink (when
// connected). The network send never blocks: it reports bytes taken, 0 for
// would-block. After the link makes no progress for stallMs the pump drops
// what it holds and every chunk after it until one send goes through, then
// announces the loss.
class LogPump
{
public:
    explicit LogPump(uint32_t stallMs = 250) : m_stallMs(stallMs) {}

    // Returns true when it did work (the caller sleeps only on false/blocked).
    template <class NetFn, class FileFn>
    bool Step(LogRing &ring, uint64_t nowMs, bool netUp, NetFn net, FileFn file)
    {
        bool did = false;
        if (m_len == 0)
        {
            const uint64_t ringDrops = ring.TakeDropped();
            m_netLost += ringDrops;
            m_fileLost += ringDrops;
            if (m_fileLost)
            {
                char mark[48];
                const int n = std::snprintf(mark, sizeof(mark), "LOG_DROPPED %llu\n",
                                            (unsigned long long)m_fileLost);
                file(mark, (size_t)n);
                m_fileLost = 0;
            }
            m_len = ring.Pop(m_chunk, sizeof(m_chunk));
            m_off = 0;
            m_stallSince = 0;
            if (m_len == 0)
                return false;
            file(m_chunk, m_len);
            did = true;
            if (!netUp)
            {
                m_len = 0;
                return true;
            }
            if (m_netLost && !m_stalled && !SendMarker(net))
                Drop();
        }
        if (m_len == 0)
            return did;
        const long sent = net(m_chunk + m_off, m_len - m_off);
        if (sent > 0)
        {
            m_off += (size_t)sent;
            m_stallSince = 0;
            m_stalled = false;
            if (m_off >= m_len)
                m_len = 0;
            return true;
        }
        if (m_stalled)
        {
            Drop();
            return did;
        }
        if (m_stallSince == 0)
            m_stallSince = nowMs ? nowMs : 1;
        else if (nowMs - m_stallSince >= m_stallMs)
        {
            m_stalled = true;
            Drop();
        }
        return did;
    }

    // True while a chunk is held waiting for the network.
    bool Blocked() const { return m_len != 0; }
    bool Stalled() const { return m_stalled; }

private:
    template <class NetFn> bool SendMarker(NetFn net)
    {
        char mark[48];
        const int n = std::snprintf(mark, sizeof(mark), "LOG_DROPPED %llu\n", (unsigned long long)m_netLost);
        size_t off = 0;
        while (off < (size_t)n)
        {
            const long s = net(mark + off, (size_t)n - off);
            if (s <= 0)
                return false;
            off += (size_t)s;
        }
        m_netLost = 0;
        return true;
    }

    void Drop()
    {
        uint64_t lines = 0;
        for (size_t i = m_off; i < m_len; ++i)
            lines += m_chunk[i] == '\n';
        m_netLost += lines ? lines : 1;
        m_len = 0;
        m_off = 0;
        m_stallSince = 0;
    }

    uint32_t m_stallMs;
    char m_chunk[8192];
    size_t m_len = 0, m_off = 0;
    uint64_t m_stallSince = 0;
    uint64_t m_netLost = 0, m_fileLost = 0;
    bool m_stalled = false;
};

// Two alternating capped files: the last one to two caps of log survive on
// the card, and a reader orders them by the seq in each file's first line.
class LogFileRing
{
public:
    LogFileRing() = default;
    ~LogFileRing() { Close(); }

    // Keeps the previous run's pair as <prefix>_prev_{a,b}.txt.
    void Open(const std::string &prefix, size_t capBytes)
    {
        m_prefix = prefix;
        m_cap = capBytes;
        for (int i = 0; i < 2; ++i)
        {
            const std::string cur = Name(i, false), prev = Name(i, true);
            std::remove(prev.c_str());
            std::rename(cur.c_str(), prev.c_str());
        }
        m_slot = 1;
        Rotate();
    }

    void Write(const char *data, size_t len)
    {
        if (!m_file)
            return;
        if (m_bytes + len > m_cap)
            Rotate();
        if (!m_file)
            return;
        std::fwrite(data, 1, len, m_file);
        m_bytes += len;
    }

    void Flush()
    {
        if (m_file)
            std::fflush(m_file);
    }

    void Close()
    {
        if (m_file)
            std::fclose(m_file);
        m_file = nullptr;
    }

    bool Active() const { return m_file != nullptr; }

private:
    std::string Name(int slot, bool prev) const
    {
        return m_prefix + (prev ? "_prev_" : "_") + (slot ? "b" : "a") + ".txt";
    }

    void Rotate()
    {
        Close();
        m_slot ^= 1;
        m_file = std::fopen(Name(m_slot, false).c_str(), "wb");
        m_bytes = 0;
        if (m_file)
        {
            char head[64];
            const int n = std::snprintf(head, sizeof(head), "LOG_RING_FILE seq=%u\n", (unsigned)++m_seq);
            std::fwrite(head, 1, (size_t)n, m_file);
            m_bytes = (size_t)n;
        }
    }

    std::string m_prefix;
    size_t m_cap = 0, m_bytes = 0;
    unsigned m_seq = 0;
    int m_slot = 0;
    FILE *m_file = nullptr;
};
