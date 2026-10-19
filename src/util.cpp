// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#include "util.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <glib.h>

namespace dc {

std::string runtime_dir() {
  std::string base = g_get_user_runtime_dir();
  std::string dir = base + "/ddc-controls";
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir;
}

std::string config_dir() {
  std::string dir = std::string(g_get_user_config_dir()) + "/ddc-controls";
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir;
}

std::string config_path() { return config_dir() + "/config.ini"; }
std::string state_path() { return runtime_dir() + "/state.ini"; }
std::string socket_path() {
  std::string p = runtime_dir() + "/control.sock";
  // sockaddr_un.sun_path is 108 bytes; fall back to a short private dir.
  if (p.size() >= 100) {
    std::string dir = std::string(g_get_tmp_dir()) + "/ddc-controls-" + std::to_string(getuid());
    ::mkdir(dir.c_str(), 0700);
    // /tmp is shared: only use the directory if it is really ours.
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077) != 0)
      return {};
    p = dir + "/control.sock";
  }
  return p;
}

int clampi(int v, int lo, int hi) {
  if (hi < lo) std::swap(lo, hi);
  return std::max(lo, std::min(hi, v));
}

int percent_of(int value, int max) {
  if (max <= 0) return 0;
  return static_cast<int>(std::lround(value * 100.0 / max));
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string markup_escape(const std::string& s) {
  gchar* e = g_markup_escape_text(s.c_str(), -1);
  std::string out = e;
  g_free(e);
  return out;
}

std::string to_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string hex2(unsigned v) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02X", v & 0xFFu);
  return buf;
}

FileLock::FileLock(const std::string& path, bool block) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd_ < 0) return;
  int rc;
  while ((rc = ::flock(fd_, LOCK_EX | (block ? 0 : LOCK_NB))) != 0 && errno == EINTR) {
  }
  if (rc != 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

FileLock::~FileLock() {
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }
}

}  // namespace dc
