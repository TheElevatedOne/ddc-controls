// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <giomm/socketservice.h>

#include "session.hpp"
#include "util.hpp"

namespace dc {

// A control request: selector, verb, arguments.
//   set   FEATURE VALUE   VALUE = N | N% | +N | -N | +N% | -N%
//   get   [FEATURE...]
//   lock  on|off|toggle
//   list | refresh | rescan
struct Command {
  std::string selector;
  std::string verb;
  std::vector<std::string> args;
};

// Runs on the main thread. Returns false on failure; output goes to `out`.
bool execute(Session& session, const Command& cmd, std::string& out);

// Try the running waybar daemon. Returns false when none is listening.
bool send_to_daemon(const Command& cmd, bool& ok, std::string& out);

// Unix-socket listener owned by one waybar daemon. Extra daemons (one per
// bar/output) stay passive and take over if the owner exits.
class ControlServer {
 public:
  explicit ControlServer(Session& s) : session_(s) {}
  ~ControlServer();
  bool serving() const { return static_cast<bool>(service_); }
  bool try_serve();

 private:
  Session& session_;
  Glib::RefPtr<Gio::SocketService> service_;
  std::unique_ptr<FileLock> owner_;
};

}  // namespace dc
