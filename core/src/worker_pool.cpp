#include "hfc/worker_pool.hpp"

#include <algorithm>

namespace hfc {

int WorkerPool::threadsFor(int wanted) {
  if (wanted > 0) return wanted - 1;
  const int cpus = static_cast<int>(std::thread::hardware_concurrency());
  return std::clamp(cpus - 1, 0, 16);
}

WorkerPool::WorkerPool(int threads) {
  for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { loop(); });
}

WorkerPool::~WorkerPool() {
  {
    std::lock_guard<std::mutex> l(mu_);
    stop_ = true;
  }
  start_.notify_all();
  for (auto& t : threads_) t.join();
}

// Takes jobs of `r` until none are left.
void WorkerPool::work(Run& r) {
  std::size_t done = 0;
  for (;;) {
    const std::size_t i = r.next.fetch_add(1);
    if (i >= r.n) break;
    (*r.job)(i);
    ++done;
  }
  if (!done) return;
  std::lock_guard<std::mutex> l(mu_);
  r.finished += done;
  if (r.finished == r.n) done_.notify_all();
}

void WorkerPool::loop() {
  uint64_t seen = 0;
  for (;;) {
    std::shared_ptr<Run> r;
    {
      std::unique_lock<std::mutex> l(mu_);
      start_.wait(l, [&] { return stop_ || generation_ != seen; });
      if (stop_) return;
      seen = generation_;
      r = current_;
    }
    if (r) work(*r);
  }
}

void WorkerPool::run(std::size_t n, const std::function<void(std::size_t)>& job) {
  if (n == 0) return;
  if (threads_.empty() || n == 1) {
    for (std::size_t i = 0; i < n; ++i) job(i);
    return;
  }
  auto r = std::make_shared<Run>();
  r->job = &job;
  r->n = n;
  {
    std::lock_guard<std::mutex> l(mu_);
    current_ = r;
    ++generation_;
  }
  start_.notify_all();
  work(*r);
  std::unique_lock<std::mutex> l(mu_);
  done_.wait(l, [&] { return r->finished == r->n; });
  current_.reset();
}

}  // namespace hfc
