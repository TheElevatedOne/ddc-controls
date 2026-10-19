// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
// `ddc-controls waybar`: long-running custom module for waybar
// ("return-type": "json"). Prints one JSON line whenever the state changes
// and serves scroll/click commands over a Unix socket.
#include <unistd.h>

#include <cmath>
#include <iostream>

#include <glib-unix.h>
#include <giomm/init.h>
#include <glibmm/main.h>

#include <csignal>
#include <map>

#include "commands.hpp"
#include "session.hpp"
#include "util.hpp"

namespace dc {

namespace {

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
    s.replace(pos, from.size(), to);
  return s;
}

int avg(const std::vector<int>& v) {
  if (v.empty()) return 0;
  double sum = 0;
  for (int x : v) sum += x;
  return static_cast<int>(std::lround(sum / v.size()));
}

std::string render(Session& session, const std::string& selector, const std::string& format) {
  auto targets = session.select(selector);
  if (targets.empty())
    return R"({"text":"","tooltip":"No DDC/CI or backlight displays","class":["ddc","unavailable"],"percentage":0})";

  std::vector<int> bright, contrast;
  bool any_locked = false;
  std::string tooltip;
  for (auto& s : targets) {
    if (!tooltip.empty()) tooltip += "\n";
    tooltip += "<b>" + markup_escape(s->label()) + "</b> " +
               markup_escape("(" + s->location() + ")");
    if (const FeatureState* b = s->feature(s->brightness_code())) {
      int p = percent_of(b->value, b->max);
      bright.push_back(p);
      tooltip += "\n  Brightness " + std::to_string(p) + "%";
    }
    if (const FeatureState* c = s->feature(s->contrast_code())) {
      int p = percent_of(c->value, c->max);
      contrast.push_back(p);
      tooltip += "\n  Contrast " + std::to_string(p) + "%";
      if (s->has_custom_limits(c->code)) {
        Limits l = s->limits(c->code);
        tooltip += " (limit " + std::to_string(l.min) + "–" + std::to_string(l.max) + ")";
      }
    }
    if (s->locked()) {
      any_locked = true;
      tooltip += "\n  Contrast linked to brightness";
    }
  }

  int b = avg(bright), c = avg(contrast);
  std::string name = targets.size() == 1 ? targets[0]->label() : "All displays";
  std::string text = format;
  text = replace_all(text, "{brightness}", std::to_string(b));
  text = replace_all(text, "{contrast}", std::to_string(c));
  text = replace_all(text, "{name}", name);
  text = replace_all(text, "{lock}", any_locked ? "\xef\x80\xa3" : "");  // nf-fa-lock

  std::string cls = any_locked ? "locked" : "unlocked";
  return "{\"text\":\"" + json_escape(text) + "\",\"alt\":\"" + cls +
         "\",\"tooltip\":\"" + json_escape(tooltip) + "\",\"class\":[\"ddc\",\"" + cls +
         "\"],\"percentage\":" + std::to_string(b) + "}";
}

}  // namespace

int run_waybar(const std::string& selector, std::string format, int poll) {
  Gio::init();
  auto loop = Glib::MainLoop::create();
  Session session;
  if (format.empty()) format = session.config.waybar_format();
  if (poll < 0) poll = session.config.poll_interval();

  std::string last;
  auto emit = [&]() {
    std::string line = render(session, selector, format);
    if (line == last) return;
    last = line;
    std::cout << line << std::endl;  // waybar reads line-buffered stdout
    if (!std::cout) loop->quit();   // bar went away
  };

  // A panel in standby fails every poll; log each distinct error once a
  // minute at most instead of flooding waybar's log.
  std::map<std::string, gint64> last_logged;
  auto log_error = [&](const std::string& e) {
    gint64 now = g_get_monotonic_time();
    auto it = last_logged.find(e);
    if (it != last_logged.end() && now - it->second < 60 * G_USEC_PER_SEC) return;
    last_logged[e] = now;
    std::cerr << "ddc-controls: " << e << std::endl;
  };

  std::vector<sigc::connection> conns;
  auto hook_screens = [&]() {
    for (auto& c : conns) c.disconnect();
    conns.clear();
    for (auto& s : session.screens) {
      conns.push_back(s->signal_changed().connect([&](const Changes&) { emit(); }));
      conns.push_back(s->signal_error().connect(log_error));
    }
    emit();
  };
  session.signal_screens_changed().connect(hook_screens);
  session.signal_config_changed().connect(emit);

  std::string log;
  session.discover_sync(&log);
  if (!log.empty()) std::cerr << log;
  session.watch();

  ControlServer server(session);
  server.try_serve();
  // Only the daemon owning the socket polls hardware; the others learn
  // about changes from the shared state file and take over if it exits.
  session.start_polling(poll, [&]() { return server.serving(); });
  if (!server.serving())
    Glib::signal_timeout().connect_seconds([&]() { return !server.try_serve(); }, 2);

  // waybar sends SIGTERM on reload/exit: leave via the main loop so the
  // socket is removed and the workers are joined.
  auto quit = [](gpointer data) -> gboolean {
    static_cast<Glib::MainLoop*>(data)->quit();
    return G_SOURCE_CONTINUE;
  };
  for (int sig : {SIGTERM, SIGINT, SIGHUP}) g_unix_signal_add(sig, quit, loop.get());

  emit();
  loop->run();
  return 0;
}

}  // namespace dc
