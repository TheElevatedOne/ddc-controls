// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#include "commands.hpp"

#include <unistd.h>

#include <cmath>
#include <future>
#include <sstream>

#include <giomm/datainputstream.h>
#include <giomm/socketclient.h>
#include <giomm/threadedsocketservice.h>
#include <giomm/unixsocketaddress.h>

#include "util.hpp"

namespace dc {

namespace {

constexpr char kSep = '\x1f';

uint8_t resolve_feature(const Screen& s, const std::string& name) {
  std::string n = to_lower(name);
  if (n == "brightness" || n == "b" || n == "br") return s.brightness_code();
  if (n == "contrast" || n == "c") return s.contrast_code();
  if (n == "volume" || n == "vol") return s.feature(0x62) ? 0x62 : 0;
  // Hex opcode: 0x12, 12h
  std::string hex = n;
  if (hex.rfind("0x", 0) == 0) hex = hex.substr(2);
  else if (!hex.empty() && hex.back() == 'h') hex.pop_back();
  else hex.clear();
  if (!hex.empty() && hex.size() <= 2 &&
      hex.find_first_not_of("0123456789abcdef") == std::string::npos) {
    uint8_t c = static_cast<uint8_t>(std::stoul(hex, nullptr, 16));
    return s.feature(c) ? c : 0;
  }
  // Feature name with spaces written as - or _: red-gain
  for (const auto& f : s.features()) {
    std::string fn = to_lower(f.name);
    for (auto& ch : fn)
      if (ch == ' ') ch = '-';
    std::string q = n;
    for (auto& ch : q)
      if (ch == '_' || ch == ' ') ch = '-';
    if (fn == q) return f.code;
  }
  return 0;
}

std::string feature_slug(const Screen& s, uint8_t code) {
  if (code == s.brightness_code()) return "brightness";
  if (code == s.contrast_code()) return "contrast";
  std::string n = to_lower(s.feature(code)->name);
  for (auto& c : n)
    if (c == ' ') c = '-';
  return n;
}

bool parse_value(const std::string& spec, bool& relative, bool& percent,
                 double& number) {
  if (spec.empty()) return false;
  std::string s = spec;
  relative = s[0] == '+' || s[0] == '-';
  percent = s.back() == '%';
  if (percent) s.pop_back();
  try {
    size_t pos = 0;
    number = std::stod(s, &pos);
    return pos == s.size() && std::isfinite(number) && std::fabs(number) < 1e6;
  } catch (...) {
    return false;
  }
}

std::string describe(const Session& session, size_t index, const Screen& s) {
  std::ostringstream o;
  o << "[" << index << "] " << s.label() << "  (" << s.kind() << ", "
    << s.location() << ")\n    id: " << s.id() << "\n";
  if (s.can_lock()) o << "    lock: " << (s.locked() ? "on" : "off") << "\n";
  for (const auto& f : s.features()) {
    Limits l = s.limits(f.code);
    o << "    " << feature_slug(s, f.code) << " (0x" << hex2(f.code)
      << "): " << f.value << "/" << f.max;
    if (s.has_custom_limits(f.code)) o << "  limits " << l.min << "-" << l.max;
    o << "\n";
  }
  (void)session;
  return o.str();
}

}  // namespace

bool execute(Session& session, const Command& cmd, std::string& out) {
  std::ostringstream o;
  const std::string& v = cmd.verb;

  if (v == "rescan") {
    session.discover_async(nullptr);
    out = "rescanning\n";
    return true;
  }

  auto targets = session.select(cmd.selector);
  if (targets.empty()) {
    out = session.discovering()      ? "still detecting displays, try again\n"
          : session.screens.empty() ? "no displays found\n"
                                    : "no display matches '" + cmd.selector + "'\n";
    return false;
  }

  if (v == "list") {
    for (size_t i = 0; i < session.screens.size(); ++i)
      for (auto& t : targets)
        if (t == session.screens[i]) o << describe(session, i, *t);
    out = o.str();
    return true;
  }

  if (v == "get") {
    for (auto& s : targets) {
      std::vector<uint8_t> codes;
      if (cmd.args.empty()) {
        for (auto& f : s->features()) codes.push_back(f.code);
      } else {
        for (auto& a : cmd.args)
          if (uint8_t c = resolve_feature(*s, a)) codes.push_back(c);
        if (codes.empty() && targets.size() == 1) {
          out = s->label() + " has no feature matching the request\n";
          return false;
        }
      }
      for (uint8_t c : codes) {
        const FeatureState* f = s->feature(c);
        o << s->id() << "\t" << feature_slug(*s, c) << "\t" << f->value << "\t"
          << f->max << "\t" << percent_of(f->value, f->max) << "%\n";
      }
    }
    out = o.str();
    if (out.empty()) {
      out = "no selected display has the requested feature\n";
      return false;
    }
    return true;
  }

  if (v == "set") {
    if (cmd.args.size() != 2) {
      out = "usage: set FEATURE VALUE\n";
      return false;
    }
    bool rel = false, pct = false;
    double num = 0;
    if (!parse_value(cmd.args[1], rel, pct, num)) {
      out = "bad value '" + cmd.args[1] + "'\n";
      return false;
    }
    bool any = false;
    for (auto& s : targets) {
      uint8_t c = resolve_feature(*s, cmd.args[0]);
      if (!c) continue;
      any = true;
      const FeatureState* f = s->feature(c);
      if (rel && pct) {
        s->step_percent(c, num);
      } else if (rel) {
        s->set_value(c, f->value + static_cast<int>(std::lround(num)));
      } else if (pct) {
        s->set_value(c, static_cast<int>(std::lround(num * f->max / 100.0)));
      } else {
        s->set_value(c, static_cast<int>(std::lround(num)));
      }
      f = s->feature(c);
      o << s->id() << "\t" << feature_slug(*s, c) << "\t" << f->value << "\n";
    }
    if (!any) {
      out = "no selected display has feature '" + cmd.args[0] + "'\n";
      return false;
    }
    out = o.str();
    return true;
  }

  if (v == "lock") {
    std::string mode = cmd.args.empty() ? "toggle" : cmd.args[0];
    bool target_state = false;
    if (mode == "on") target_state = true;
    else if (mode == "off") target_state = false;
    else if (mode == "toggle") {
      for (auto& s : targets)
        if (s->can_lock()) {
          target_state = !s->locked();
          break;
        }
    } else {
      out = "usage: lock on|off|toggle\n";
      return false;
    }
    for (auto& s : targets) {
      if (!s->can_lock()) continue;
      s->set_locked(target_state);
      o << s->id() << "\tlock\t" << (target_state ? "on" : "off") << "\n";
    }
    out = o.str();
    if (out.empty()) {
      out = "no selected display has both brightness and contrast\n";
      return false;
    }
    return true;
  }

  if (v == "refresh") {
    for (auto& s : targets) s->refresh();
    out = "ok\n";
    return true;
  }

  out = "unknown command '" + v + "'\n";
  return false;
}

// Wire format: one line, fields separated by 0x1F. Reply: "ok"/"err" line,
// then free text until EOF.
bool send_to_daemon(const Command& cmd, bool& ok, std::string& out) {
  if (socket_path().empty()) return false;
  try {
    auto client = Gio::SocketClient::create();
    client->set_timeout(3);
    auto conn = client->connect(Gio::UnixSocketAddress::create(socket_path()));
    std::string line = cmd.selector + kSep + cmd.verb;
    for (auto& a : cmd.args) line += kSep + a;
    line += "\n";
    gsize written = 0;
    conn->get_output_stream()->write_all(line, written);

    auto in = Gio::DataInputStream::create(conn->get_input_stream());
    std::string status;
    if (!in->read_line(status)) return false;
    ok = status == "ok";
    out.clear();
    std::string l;
    while (in->read_line(l)) out += l + "\n";
    return true;
  } catch (const Glib::Error&) {
    return false;
  }
}

ControlServer::~ControlServer() {
  if (service_) {
    service_->stop();
    service_->close();
    ::unlink(socket_path().c_str());
  }
}

bool ControlServer::try_serve() {
  if (service_) return true;
  const std::string path = socket_path();
  if (path.empty()) return false;
  // Whoever holds server.lock owns the socket. The lock dies with its
  // process, so a crashed owner leaves a stale socket we may replace.
  auto owner = std::make_unique<FileLock>(runtime_dir() + "/server.lock", false);
  if (!owner->locked()) return false;
  ::unlink(path.c_str());

  auto svc = Gio::ThreadedSocketService::create(4);
  try {
    Glib::RefPtr<Gio::SocketAddress> effective;
    svc->add_address(Gio::UnixSocketAddress::create(path), Gio::SOCKET_TYPE_STREAM,
                     Gio::SOCKET_PROTOCOL_DEFAULT, effective);
  } catch (const Glib::Error&) {
    return false;
  }
  Session* session = &session_;
  svc->signal_run().connect(
      [session](const Glib::RefPtr<Gio::SocketConnection>& conn,
                const Glib::RefPtr<Glib::Object>&) {
        try {
          auto in = Gio::DataInputStream::create(conn->get_input_stream());
          std::string line;
          if (!in->read_line(line)) return true;
          Command cmd;
          std::vector<std::string> parts;
          std::string cur;
          for (char ch : line) {
            if (ch == kSep) {
              parts.push_back(cur);
              cur.clear();
            } else {
              cur += ch;
            }
          }
          parts.push_back(cur);
          if (parts.size() < 2) return true;
          cmd.selector = parts[0];
          cmd.verb = parts[1];
          cmd.args.assign(parts.begin() + 2, parts.end());

          // Session lives on the main thread.
          auto p = std::make_shared<std::promise<std::pair<bool, std::string>>>();
          auto fut = p->get_future();
          Glib::MainContext::get_default()->invoke([session, cmd, p]() {
            std::string out;
            bool ok = execute(*session, cmd, out);
            p->set_value({ok, out});
            return false;
          });
          if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            return true;  // main loop gone (shutting down)
          auto [ok, out] = fut.get();
          std::string reply = std::string(ok ? "ok" : "err") + "\n" + out;
          gsize written = 0;
          conn->get_output_stream()->write_all(reply, written);
        } catch (const Glib::Error&) {
        }
        return true;
      },
      false);
  svc->start();
  service_ = svc;
  owner_ = std::move(owner);
  return true;
}

}  // namespace dc
