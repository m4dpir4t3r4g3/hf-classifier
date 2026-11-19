// Lock-free single-producer / single-consumer ring buffer. The SDRuno plugin
// uses it to hand decoded characters, message events and spectrum frames from
// the audio thread to the UI thread without ever blocking the audio thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace navtex {

template <typename T>
class SpscQueue {
 public:
  explicit SpscQueue(std::size_t capacity) : buf_(capacity + 1) {}

  // Producer side. Returns false (dropping the item) when full.
  bool push(const T& item) {
    std::size_t head = head_.load(std::memory_order_relaxed);
    std::size_t next = (head + 1) % buf_.size();
    if (next == tail_.load(std::memory_order_acquire)) return false;
    buf_[head] = item;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side.
  bool pop(T& item) {
    std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    item = buf_[tail];
    tail_.store((tail + 1) % buf_.size(), std::memory_order_release);
    return true;
  }

 private:
  std::vector<T> buf_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace navtex
