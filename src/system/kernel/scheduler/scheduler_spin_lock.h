#ifndef _SCHEDULER_SPIN_LOCK_H
#define _SCHEDULER_SPIN_LOCK_H

#include <atomic>

namespace Scheduler {

// Architecture-independent spinlock implementation
class SpinLock {
private:
    std::atomic_flag locked = ATOMIC_FLAG_INIT;

public:
    void lock() {
        while (locked.test_and_set(std::memory_order_acquire)) {
            // Use architecture-specific pause/yield instruction
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield" ::: "memory");
#else
            // Generic fallback for other architectures
#if __cplusplus >= 202002L
            // C++20 has a standard way to yield
            std::this_thread::yield();
#endif
#endif
        }
    }

    void unlock() {
        locked.clear(std::memory_order_release);
    }

    bool try_lock() {
        return !locked.test_and_set(std::memory_order_acquire);
    }
};

// RAII lock guard for spinlock
class SpinLockGuard {
private:
    SpinLock& lock_;

public:
    explicit SpinLockGuard(SpinLock& lock) : lock_(lock) {
        lock_.lock();
    }

    ~SpinLockGuard() {
        lock_.unlock();
    }

    // Non-copyable
    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;
};

} // namespace Scheduler

#endif // _SCHEDULER_SPIN_LOCK_H
