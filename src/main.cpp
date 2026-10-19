// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <giomm/init.h>
#include <glibmm/main.h>

#include "commands.hpp"
#include "session.hpp"

#ifndef DDC_CONTROLS_VERSION
#define DDC_CONTROLS_VERSION "dev"
#endif

namespace dc {
int run_gui(int argc, char** argv);
int run_waybar(const std::string& selector, std::string format, int poll);
}  // namespace dc

namespace {

const char* kUsage =
    "Usage: ddc-controls [-d DISPLAY] [COMMAND [ARGS...]]\n"
    "\n"
    "Monitor control over DDC/CI (libddcci), with brightnessctl for\n"
    "backlight panels that do not speak DDC/CI.\n"
    "\n"
    "Commands:\n"
    "  gui                      open the control panel (default)\n"
    "  list                     show displays, controls, limits, lock\n"
    "  get [FEATURE...]         print id, feature, value, max, percent\n"
    "  set FEATURE VALUE        VALUE: N, N%, +N, -N, +N%, -N%\n"
    "  up [STEP] / down [STEP]  brightness by STEP percent (default: config step)\n"
    "  lock on|off|toggle       link contrast to brightness\n"
    "  refresh                  re-read values from hardware\n"
    "  rescan                   re-detect displays (waybar daemon)\n"
    "  waybar [--format FMT] [--poll SEC]\n"
    "                           run as a waybar custom module (JSON output)\n"
    "\n"
    "Options:\n"
    "  -d, --display DISPLAY    all (default), index, id, or a substring of the\n"
    "                           name / connector, e.g. DP-3 or AOC\n"
    "  -h, --help, -V, --version\n"
    "\n"
    "FEATURE: brightness, contrast, volume, a name like red-gain, or 0xNN.\n"
    "Limits and the lock are set in the GUI or ~/.config/ddc-controls/config.ini.\n"
    "Commands go to a running `waybar` daemon when there is one, otherwise\n"
    "they talk to the hardware directly.\n";

// Run a command in this process (no daemon listening).
int run_direct(const dc::Command& cmd) {
  Gio::init();
  dc::Session session;
  std::string log;
  session.discover_sync(&log);
  if (session.screens.empty() && !log.empty()) std::cerr << log;
  std::string out;
  bool ok = dc::execute(session, cmd, out);
  for (auto& s : session.screens) s->flush();
  // Deliver worker results (write errors) queued on the main context.
  auto ctx = Glib::MainContext::get_default();
  bool failed_io = false;
  std::vector<sigc::connection> conns;
  for (auto& s : session.screens)
    conns.push_back(s->signal_error().connect([&](const std::string& e) {
      std::cerr << e << "\n";
      failed_io = true;
    }));
  while (ctx->pending()) ctx->iteration(false);
  session.publish_now();
  (ok ? std::cout : std::cerr) << out;
  return ok && !failed_io ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::string selector;
  std::vector<std::string> pos;
  std::string format;
  int poll = -1;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    bool needs_value = a == "-d" || a == "--display" || a == "--format" || a == "--poll";
    if (needs_value && i + 1 >= argc) {
      std::cerr << "option " << a << " needs a value\n";
      return 2;
    }
    if (a == "-d" || a == "--display") {
      selector = argv[++i];
    } else if (a.rfind("--display=", 0) == 0) {
      selector = a.substr(10);
    } else if (a == "--format") {
      format = argv[++i];
    } else if (a == "--poll") {
      poll = std::atoi(argv[++i]);
    } else if (a == "-h" || a == "--help" || a == "help") {
      std::cout << kUsage;
      return 0;
    } else if (a == "-V" || a == "--version") {
      std::cout << "ddc-controls " DDC_CONTROLS_VERSION "\n";
      return 0;
    } else {
      pos.push_back(a);
    }
  }

  if (pos.empty() || pos[0] == "gui") {
    char* gui_argv[] = {argv[0], nullptr};
    return dc::run_gui(1, gui_argv);
  }

  const std::string verb = pos[0];
  if (verb == "waybar") return dc::run_waybar(selector, format, poll);

  dc::Command cmd;
  cmd.selector = selector;
  cmd.verb = verb;
  cmd.args.assign(pos.begin() + 1, pos.end());

  if (verb == "up" || verb == "down") {
    std::string step;
    if (!cmd.args.empty()) {
      step = cmd.args[0];
      if (!step.empty() && step.back() == '%') step.pop_back();
      if (step.empty() || step.find_first_not_of("0123456789.") != std::string::npos) {
        std::cerr << "STEP must be a positive number of percent\n";
        return 2;
      }
    } else {
      dc::Config cfg;
      cfg.load();
      step = std::to_string(cfg.step());
    }
    cmd.verb = "set";
    cmd.args = {"brightness", (verb == "up" ? "+" : "-") + step + "%"};
  }

  static const char* known[] = {"list", "get", "set", "lock", "refresh", "rescan"};
  bool valid = false;
  for (auto* k : known) valid |= cmd.verb == k;
  if (!valid) {
    std::cerr << "unknown command '" << verb << "'\n\n" << kUsage;
    return 2;
  }

  Gio::init();
  bool ok = false;
  std::string out;
  if (dc::send_to_daemon(cmd, ok, out)) {
    (ok ? std::cout : std::cerr) << out;
    return ok ? 0 : 1;
  }
  if (cmd.verb == "rescan") return 0;  // nothing cached to rescan
  return run_direct(cmd);
}
