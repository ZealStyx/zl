#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace zl {

class RuntimeTaskExecutor {
public:
    static RuntimeTaskExecutor& instance();
    RuntimeTaskExecutor(const RuntimeTaskExecutor&) = delete;
    RuntimeTaskExecutor& operator=(const RuntimeTaskExecutor&) = delete;
    void enqueue(std::function<void()> work);

private:
    RuntimeTaskExecutor();
    ~RuntimeTaskExecutor();
    void workerLoop();

    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> queue_;
    bool stopping_{false};
    std::vector<std::thread> workers_;
};

} // namespace zl
