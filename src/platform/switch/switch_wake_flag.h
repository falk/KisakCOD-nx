#pragma once

// A consumable wake flag: Notify() sets it, Wait() blocks until it is set and
// clears it (the retail auto-reset wakeDatabaseEvent: Sys_NotifyDatabase /
// Sys_WaitStartDatabase). Notifies before the waiter consumes coalesce into
// one wake, as with an auto-reset event.
//
// It replaces a 1 ms sleep-poll of an atomic flag: the database thread idles
// in Sys_WaitStartDatabase for the whole game, and the poll woke it 1000
// times a second on the main thread's core. The waiter now sleeps in the
// kernel until notified.
//
// Parameterized on the lock and condition variable so the same logic runs on
// libnx (switch_misc_stubs.cpp) and, in switch_hot_engine2_test.cpp, on the
// standard library under ASan/TSan-style stress.

template <typename Lock, typename Cond> class WakeFlag
{
public:
    void Notify()
    {
        m_lock.lock();
        m_set = true;
        m_cond.notify_one();
        m_lock.unlock();
    }

    void Wait()
    {
        m_lock.lock();
        while (!m_set)
            m_cond.wait(m_lock);
        m_set = false;
        m_lock.unlock();
    }

private:
    Lock m_lock;
    Cond m_cond;
    bool m_set = false;
};
