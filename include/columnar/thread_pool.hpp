#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace columnar {

// A fixed-size pool of worker threads pulling tasks off one shared queue --
// the standard "N threads, one queue, condition-variable wakeup" shape.
// Each accepted server connection becomes one task; a connection that
// blocks on a slow client doesn't starve others as long as num_threads >
// concurrently-slow-connections (a real limitation of this simple a pool,
// noted rather than solved -- a production server would use async I/O
// instead of one thread per in-flight connection).
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t num_threads);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void submit(std::function<void()> task);

 private:
  void worker_loop();

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
};

}  // namespace columnar
