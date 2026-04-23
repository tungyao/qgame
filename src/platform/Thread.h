#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <string>
#if defined(_MSC_VER)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif
#if defined(__linux__)
#  include <pthread.h>
#endif

namespace platform {

class Thread {
public:
    using Task = std::function<void()>;

    Thread() = default;
    ~Thread() { join(); }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void start(Task task, const std::string& name = "") {
        thread_ = std::thread([task = std::move(task), name]() {
#if defined(_MSC_VER)
            // Windows：通过抛异常方式给调试器命名线程
            if (!name.empty()) {
                auto wname = std::wstring(name.begin(), name.end());
                ::SetThreadDescription(::GetCurrentThread(), wname.c_str());
            }
#elif defined(__linux__)
            if (!name.empty()) {
                pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
            }
#elif defined(__APPLE__)
            if (!name.empty()) {
                pthread_setname_np(name.substr(0, 63).c_str());
            }
#endif
            task();
        });
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }

    bool running() const { return thread_.joinable(); }

private:
    std::thread thread_;
};

// 信号量 — 用于线程间同步（如 AudioThread 唤醒）
class Semaphore {
public:
    explicit Semaphore(int initial = 0) : count_(initial) {}

    void post() {
        std::unique_lock lock(mutex_);
        ++count_;
        cv_.notify_one();
    }

    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

    bool tryWait() {
        std::unique_lock lock(mutex_);
        if (count_ > 0) { --count_; return true; }
        return false;
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    int                     count_;
};

} // namespace platform
