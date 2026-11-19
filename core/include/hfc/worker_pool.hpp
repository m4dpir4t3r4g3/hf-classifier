// A fixed set of threads that run the jobs of one call to run() and return
// when all of them are done; the calling thread takes jobs too. For the
// decoder host's sessions, which are independent of one another.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hfc {

class WorkerPool {
 public:
  // `threads` besides the caller's (0: run() does everything itself).
  explicit WorkerPool(int threads);
  ~WorkerPool();
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  int threads() const { return static_cast<int>(threads_.size()); }
  // job(0) ... job(n - 1), each once, in any order and on any thread; returns
  // when all have returned. Not reentrant.
  void run(std::size_t n, const std::function<void(std::size_t)>& job);
  // The threads for `wanted` (0: the processors less one, at most 16).
  static int threadsFor(int wanted);

 private:
  // One call to run(): a thread that wakes late keeps the run it saw, whose
  // jobs are all taken by then.
  struct Run {
    const std::function<void(std::size_t)>* job = nullptr;
    std::size_t n = 0;
    std::atomic<std::size_t> next{0};
    std::size_t finished = 0;  // under mu_
  };
  void loop();
  void work(Run& r);

  std::vector<std::thread> threads_;
  std::mutex mu_;
  std::condition_variable start_, done_;
  std::shared_ptr<Run> current_;  // under mu_
  uint64_t generation_ = 0;       // under mu_
  bool stop_ = false;
};

}  // namespace hfc
