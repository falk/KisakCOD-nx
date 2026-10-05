#pragma once

// Background file writer for save games. The caller serialises on its own
// thread into a private buffer and returns; one worker writes
// <tmp>, closes it and renames it over <final>, so a crash or power loss
// leaves either the old file or the whole new one, never a torn save. Jobs run
// in submission order. Header-only and platform-neutral so the host test runs
// the real code.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AsyncFileWriter
{
public:
    struct Job
    {
        std::string tmpPath;
        std::string finalPath;
        std::vector<uint8_t> data;
    };

    struct Result
    {
        bool ok;
        uint32_t ms;
        size_t bytes;
        std::string finalPath;
    };

    // Called on the worker after each job, before WaitIdle can return.
    typedef void (*ReportFn)(const Result &result);

    // Creates the parent directories of a path (called on the worker).
    typedef void (*MakeDirsFn)(const std::string &path);

    explicit AsyncFileWriter(MakeDirsFn makeDirs = nullptr, size_t maxInFlight = 2)
        : m_makeDirs(makeDirs), m_maxInFlight(maxInFlight)
    {
    }

    ~AsyncFileWriter()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_quit = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    AsyncFileWriter(const AsyncFileWriter &) = delete;
    AsyncFileWriter &operator=(const AsyncFileWriter &) = delete;

    void SetReporter(ReportFn report)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_report = report;
    }

    // A recycled buffer (empty, capacity kept). Taking it never waits.
    std::vector<uint8_t> AcquireBuffer()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_pool.empty())
            return std::vector<uint8_t>();
        std::vector<uint8_t> buf = std::move(m_pool.back());
        m_pool.pop_back();
        buf.clear();
        return buf;
    }

    // Queues a write. Waits only when maxInFlight jobs are already pending,
    // which a save every few minutes never reaches.
    void Submit(Job &&job)
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_space.wait(lock, [&] { return m_inFlight < m_maxInFlight; });
        ++m_inFlight;
        m_queue.push_back(std::move(job));
        if (!m_thread.joinable())
            m_thread = std::thread([this] { Run(); });
        lock.unlock();
        m_wake.notify_one();
    }

    // Returns once every submitted job has finished; a reader that must see
    // the newest file calls this first.
    void WaitIdle()
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_space.wait(lock, [&] { return m_inFlight == 0; });
    }

    bool Busy()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_inFlight != 0;
    }

private:
    static bool WriteOne(const Job &job, MakeDirsFn makeDirs)
    {
        if (makeDirs)
        {
            makeDirs(job.tmpPath);
            makeDirs(job.finalPath);
        }
        std::FILE *f = std::fopen(job.tmpPath.c_str(), "wb");
        if (!f)
            return false;
        bool ok = job.data.empty() || std::fwrite(job.data.data(), 1, job.data.size(), f) == job.data.size();
        ok = (std::fclose(f) == 0) && ok;
        if (ok && std::rename(job.tmpPath.c_str(), job.finalPath.c_str()) != 0)
        {
            // Some SD filesystems refuse to rename over an existing file.
            std::remove(job.finalPath.c_str());
            ok = std::rename(job.tmpPath.c_str(), job.finalPath.c_str()) == 0;
        }
        if (!ok)
            std::remove(job.tmpPath.c_str());
        return ok;
    }

    void Run()
    {
        std::unique_lock<std::mutex> lock(m_lock);
        for (;;)
        {
            m_wake.wait(lock, [&] { return m_quit || !m_queue.empty(); });
            if (m_queue.empty())
                return;
            Job job = std::move(m_queue.front());
            m_queue.pop_front();
            lock.unlock();

            const auto t0 = std::chrono::steady_clock::now();
            Result r;
            r.ok = WriteOne(job, m_makeDirs);
            r.ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
            r.bytes = job.data.size();
            r.finalPath = job.finalPath;

            if (m_report)
                m_report(r);

            lock.lock();
            if (m_pool.size() < m_maxInFlight)
                m_pool.push_back(std::move(job.data));
            --m_inFlight;
            m_space.notify_all();
        }
    }

    MakeDirsFn m_makeDirs;
    ReportFn m_report = nullptr;
    size_t m_maxInFlight;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::condition_variable m_space;
    std::deque<Job> m_queue;
    std::vector<std::vector<uint8_t>> m_pool;
    size_t m_inFlight = 0;
    bool m_quit = false;
    std::thread m_thread;
};
