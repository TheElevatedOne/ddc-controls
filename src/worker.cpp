// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#include "worker.hpp"

namespace dc {

Worker::Worker(DevicePtr dev, Callback cb)
    : dev_(std::move(dev)), cb_(std::move(cb)), thread_([this] { run(); }) {}

Worker::~Worker() {
  {
    std::lock_guard<std::mutex> g(m_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void Worker::write(uint8_t code, int value) {
  {
    std::lock_guard<std::mutex> g(m_);
    writes_[code] = value;
  }
  cv_.notify_all();
}

void Worker::read(const std::vector<uint8_t>& codes) {
  {
    std::lock_guard<std::mutex> g(m_);
    reads_.insert(codes.begin(), codes.end());
  }
  cv_.notify_all();
}

void Worker::flush() {
  std::unique_lock<std::mutex> lk(m_);
  idle_cv_.wait(lk, [this] {
    return stop_ || (!busy_ && writes_.empty() && reads_.empty());
  });
}

void Worker::run() {
  std::unique_lock<std::mutex> lk(m_);
  for (;;) {
    cv_.wait(lk, [this] { return stop_ || !writes_.empty() || !reads_.empty(); });
    if (stop_) break;
    busy_ = true;

    Result res;
    std::set<uint8_t> resync;
    // Writes first: they are what the user is waiting for.
    while (!writes_.empty() && !stop_) {
      auto batch = std::move(writes_);
      writes_.clear();
      lk.unlock();
      for (auto& [code, value] : batch) {
        std::string err;
        if (!dev_->write(code, value, err)) {
          res.errors.push_back(dev_->label() + ": " + err);
          resync.insert(code);
        }
      }
      lk.lock();
    }
    reads_.insert(resync.begin(), resync.end());

    if (!reads_.empty() && writes_.empty() && !stop_) {
      auto codes = std::move(reads_);
      reads_.clear();
      lk.unlock();
      for (uint8_t code : codes) {
        int v = 0, m = 0;
        std::string err;
        if (dev_->read(code, v, m, err))
          res.reads[code] = {v, m};
        else
          res.errors.push_back(dev_->label() + ": " + err);
      }
      lk.lock();
    }

    if (!res.reads.empty() || !res.errors.empty()) {
      lk.unlock();
      cb_(std::move(res));
      lk.lock();
    }
    busy_ = false;
    idle_cv_.notify_all();
  }
  busy_ = false;
  idle_cv_.notify_all();
}

}  // namespace dc
