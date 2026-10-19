// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "device.hpp"

namespace dc {

// Owns all I/O to one Device on a background thread. Writes are coalesced
// per VCP code, so dragging a slider only sends the latest value.
class Worker {
 public:
  struct Result {
    std::map<uint8_t, std::pair<int, int>> reads;  // code -> (value, max)
    std::vector<std::string> errors;
  };
  // Called on the worker thread after each batch.
  using Callback = std::function<void(Result)>;

  Worker(DevicePtr dev, Callback cb);
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void write(uint8_t code, int value);
  void read(const std::vector<uint8_t>& codes);
  // Block until every queued request has been executed.
  void flush();

 private:
  void run();

  DevicePtr dev_;
  Callback cb_;
  std::mutex m_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;
  std::map<uint8_t, int> writes_;
  std::set<uint8_t> reads_;
  bool busy_ = false;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace dc
