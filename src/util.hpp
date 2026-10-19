// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#pragma once

#include <string>

namespace dc {

// $XDG_RUNTIME_DIR/ddc-controls, created on first use.
std::string runtime_dir();
// $XDG_CONFIG_HOME/ddc-controls, created on first use.
std::string config_dir();
std::string config_path();
std::string state_path();
// Empty if no safe location exists.
std::string socket_path();

int clampi(int v, int lo, int hi);
int percent_of(int value, int max);

std::string json_escape(const std::string& s);
std::string markup_escape(const std::string& s);
std::string to_lower(std::string s);
std::string hex2(unsigned v);

// RAII exclusive flock() on a file (created if missing).
class FileLock {
 public:
  // block=false: try once; check locked() for the outcome.
  explicit FileLock(const std::string& path, bool block = true);
  ~FileLock();
  bool locked() const { return fd_ >= 0; }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  int fd_ = -1;
};

}  // namespace dc
