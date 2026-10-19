// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#include "session.hpp"

#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <thread>

#include <giomm/file.h>
#include <glibmm/fileutils.h>
#include <glibmm/keyfile.h>

#include "util.hpp"

namespace dc {

namespace {
// After a local change, ignore hardware reads / other processes' reports for
// that control briefly so in-flight writes don't make sliders jump back.
constexpr gint64 kHoldOffUs = 1500 * 1000;

std::string vkey(uint8_t c) { return "v-" + hex2(c); }
std::string mkey(uint8_t c) { return "m-" + hex2(c); }
}  // namespace

int map_linked(int value, Limits from, Limits to) {
  if (from.max <= from.min) return to.max;
  double f = double(value - from.min) / double(from.max - from.min);
  f = std::max(0.0, std::min(1.0, f));
  return static_cast<int>(std::floor(to.min + f * (to.max - to.min) + 0.5));
}

// ---------------------------------------------------------------- Screen

Screen::Screen(Session& session, DevicePtr dev)
    : session_(session), dev_(std::move(dev)) {
  for (const auto& f : dev_->features)
    features_.push_back({f.code, f.name, f.value, f.max, f.default_min});
}

Screen::~Screen() { worker_.reset(); }

void Screen::start() {
  std::weak_ptr<Screen> weak = shared_from_this();
  worker_ = std::make_unique<Worker>(dev_, [weak](Worker::Result r) {
    Glib::MainContext::get_default()->invoke([weak, r]() {
      if (auto self = weak.lock()) self->on_result(r);
      return false;
    });
  });
}

const FeatureState* Screen::feature(uint8_t code) const {
  for (const auto& f : features_)
    if (f.code == code) return &f;
  return nullptr;
}

FeatureState* Screen::mut(uint8_t code) {
  for (auto& f : features_)
    if (f.code == code) return &f;
  return nullptr;
}

Limits Screen::limits(uint8_t code) const {
  const FeatureState* f = feature(code);
  if (!f) return {};
  return session_.config.limits(id(), code, f->max, f->default_min);
}

bool Screen::has_custom_limits(uint8_t code) const {
  return session_.config.has_limits(id(), code);
}

bool Screen::locked() const { return can_lock() && session_.config.lock(id()); }

bool Screen::held(uint8_t code) const {
  auto it = last_local_set_.find(code);
  return it != last_local_set_.end() &&
         g_get_monotonic_time() - it->second < kHoldOffUs;
}

Changes Screen::set_value(uint8_t code, int value) {
  Changes want;
  if (!feature(code)) return want;
  Limits lim = limits(code);
  want[code] = clampi(value, lim.min, lim.max);

  if (locked()) {
    uint8_t b = brightness_code(), c = contrast_code();
    if (code == b) want[c] = map_linked(want[code], lim, limits(c));
    else if (code == c) want[b] = map_linked(want[code], lim, limits(b));
  }

  Changes changed;
  std::set<uint8_t> codes;
  gint64 now = g_get_monotonic_time();
  for (auto& [c, v] : want) {
    FeatureState* f = mut(c);
    last_local_set_[c] = now;
    if (f->value == v) continue;
    f->value = v;
    changed[c] = v;
    codes.insert(c);
    worker_->write(c, v);
  }
  if (!changed.empty()) {
    session_.queue_publish(*this, codes);
    changed_.emit(changed);
  }
  return changed;
}

Changes Screen::step_percent(uint8_t code, double percent) {
  const FeatureState* f = feature(code);
  if (!f) return {};
  int delta = static_cast<int>(std::lround(percent * f->max / 100.0));
  if (delta == 0 && percent != 0) delta = percent > 0 ? 1 : -1;
  return set_value(code, f->value + delta);
}

Changes Screen::set_locked(bool on) {
  session_.config.set_lock(id(), on);
  session_.save_config();
  // Bring contrast in line with brightness right away.
  if (on && can_lock()) return set_value(brightness_code(), feature(brightness_code())->value);
  return {};
}

Changes Screen::enforce_limits() {
  Changes all;
  for (const auto& f : std::vector<FeatureState>(features_)) {
    Limits l = limits(f.code);
    int v = clampi(f.value, l.min, l.max);
    bool sync = locked() && f.code == brightness_code();
    if (v != f.value || sync) {
      auto ch = set_value(f.code, v);
      all.insert(ch.begin(), ch.end());
    }
  }
  return all;
}

void Screen::refresh() {
  std::vector<uint8_t> codes;
  for (const auto& f : features_) codes.push_back(f.code);
  refresh(codes);
}

void Screen::refresh(const std::vector<uint8_t>& codes) { worker_->read(codes); }

void Screen::flush() { worker_->flush(); }

void Screen::apply_external(uint8_t code, int value, int max) {
  FeatureState* f = mut(code);
  if (!f || held(code)) return;
  if (max > 0) f->max = max;
  if (f->value == value) return;
  f->value = value;
  changed_.emit(Changes{{code, value}});
}

void Screen::on_result(Worker::Result r) {
  Changes changed;
  std::set<uint8_t> codes;
  for (auto& [code, vm] : r.reads) {
    FeatureState* f = mut(code);
    if (!f || held(code)) continue;
    if (vm.second > 0) f->max = vm.second;
    if (f->value != vm.first) {
      f->value = vm.first;
      changed[code] = vm.first;
    }
    codes.insert(code);
  }
  if (!codes.empty()) session_.queue_publish(*this, codes);
  if (!changed.empty()) changed_.emit(changed);
  for (const auto& e : r.errors) error_.emit(e);
}

// ---------------------------------------------------------------- Session

Session::Session() { config.load(); }

Session::~Session() {
  *alive_ = false;
  publish_now();
  hotplug_io_.disconnect();
  hotplug_timer_.disconnect();
  if (hotplug_fd_ >= 0) ::close(hotplug_fd_);
  publish_timer_.disconnect();
  state_timer_.disconnect();
  config_timer_.disconnect();
  poll_timer_.disconnect();
  screens.clear();
}

void Session::adopt(std::vector<DevicePtr> devs) {
  screens.clear();
  for (auto& d : devs) {
    auto s = std::make_shared<Screen>(*this, d);
    s->start();
    screens.push_back(s);
  }
  // Tell everyone else what we just read from hardware.
  for (auto& s : screens) {
    std::set<uint8_t> codes;
    for (auto& f : s->features()) codes.insert(f.code);
    queue_publish(*s, codes);
  }
  screens_changed_.emit();
}

void Session::discover_sync(std::string* log) { adopt(discover_devices(log)); }

void Session::discover_async(std::function<void(const std::string&)> done) {
  if (discovering_) return;
  discovering_ = true;
  // Existing screens stay usable while probing; discovery takes the bus
  // locks, so their workers simply wait for it.
  std::weak_ptr<bool> alive = alive_;
  std::thread([this, alive, done]() {
    auto log = std::make_shared<std::string>();
    auto devs = std::make_shared<std::vector<DevicePtr>>(discover_devices(log.get()));
    Glib::MainContext::get_default()->invoke([this, alive, done, log, devs]() {
      if (alive.expired()) return false;
      discovering_ = false;
      adopt(std::move(*devs));
      if (done) done(*log);
      return false;
    });
  }).detach();
}

void Session::clear() {
  if (screens.empty()) return;
  screens.clear();
  screens_changed_.emit();
}

std::vector<ScreenPtr> Session::select(const std::string& sel) const {
  if (sel.empty() || sel == "all") return screens;
  std::vector<ScreenPtr> out;
  bool numeric = !sel.empty() && sel.find_first_not_of("0123456789") == std::string::npos;
  if (numeric) {
    if (sel.size() < 6) {
      size_t i = std::stoul(sel);
      if (i < screens.size()) out.push_back(screens[i]);
    }
    return out;
  }
  for (auto& s : screens)
    if (s->id() == sel) return {s};
  std::string needle = to_lower(sel);
  for (auto& s : screens) {
    if (to_lower(s->id()).find(needle) != std::string::npos ||
        to_lower(s->label()).find(needle) != std::string::npos ||
        to_lower(s->location()).find(needle) != std::string::npos)
      out.push_back(s);
  }
  return out;
}

void Session::save_config() { config.save(); }

void Session::queue_publish(const Screen& s, const std::set<uint8_t>& codes) {
  pending_publish_[s.id()].insert(codes.begin(), codes.end());
  if (!publish_timer_.connected())
    publish_timer_ = Glib::signal_timeout().connect(
        [this]() {
          publish_now();
          return false;
        },
        80);
}

void Session::publish_now() {
  publish_timer_.disconnect();
  if (pending_publish_.empty()) return;
  FileLock lock(runtime_dir() + "/state.lock");
  Glib::KeyFile kf;
  try {
    kf.load_from_file(state_path());
  } catch (const Glib::Error&) {
  }
  for (auto& [id, codes] : pending_publish_) {
    for (auto& s : screens) {
      if (s->id() != id) continue;
      kf.set_string(id, "label", s->label());
      kf.set_string(id, "kind", s->kind());
      kf.set_integer(id, "pid", getpid());
      for (uint8_t c : codes) {
        const FeatureState* f = s->feature(c);
        if (!f) continue;
        kf.set_integer(id, vkey(c), f->value);
        kf.set_integer(id, mkey(c), f->max);
      }
    }
  }
  pending_publish_.clear();
  try {
    Glib::file_set_contents(state_path(), kf.to_data());
  } catch (const Glib::Error&) {
  }
}

void Session::load_state() {
  Glib::KeyFile kf;
  try {
    FileLock lock(runtime_dir() + "/state.lock");
    kf.load_from_file(state_path());
  } catch (const Glib::Error&) {
    return;
  }
  for (auto& s : screens) {
    const std::string& id = s->id();
    if (!kf.has_group(id)) continue;
    try {
      if (kf.has_key(id, "pid") && kf.get_integer(id, "pid") == getpid()) continue;
      for (auto& f : std::vector<FeatureState>(s->features())) {
        if (!kf.has_key(id, vkey(f.code))) continue;
        int v = kf.get_integer(id, vkey(f.code));
        int m = kf.has_key(id, mkey(f.code)) ? kf.get_integer(id, mkey(f.code)) : 0;
        s->apply_external(f.code, v, m);
      }
    } catch (const Glib::Error&) {
    }
  }
}

void Session::watch() {
  watch_hotplug();
  auto debounce = [](sigc::connection& timer, std::function<void()> fn) {
    timer.disconnect();
    timer = Glib::signal_timeout().connect(
        [fn]() {
          fn();
          return false;
        },
        40);
  };
  auto matches = [](const Glib::RefPtr<Gio::File>& a,
                    const Glib::RefPtr<Gio::File>& b, const char* name) {
    return (a && a->get_basename() == name) || (b && b->get_basename() == name);
  };

  state_mon_ = Gio::File::create_for_path(runtime_dir())->monitor_directory();
  state_mon_->signal_changed().connect(
      [this, debounce, matches](const Glib::RefPtr<Gio::File>& f,
                                const Glib::RefPtr<Gio::File>& o,
                                Gio::FileMonitorEvent) {
        if (matches(f, o, "state.ini")) debounce(state_timer_, [this] { load_state(); });
      });

  config_mon_ = Gio::File::create_for_path(config_dir())->monitor_directory();
  config_mon_->signal_changed().connect(
      [this, debounce, matches](const Glib::RefPtr<Gio::File>& f,
                                const Glib::RefPtr<Gio::File>& o,
                                Gio::FileMonitorEvent) {
        if (matches(f, o, "config.ini"))
          debounce(config_timer_, [this] {
            config.load();
            config_changed_.emit();
          });
      });
}

void Session::watch_hotplug() {
  // Kernel uevents: connector plugged/unplugged -> "SUBSYSTEM=drm ... HOTPLUG=1".
  hotplug_fd_ = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                         NETLINK_KOBJECT_UEVENT);
  if (hotplug_fd_ < 0) return;
  sockaddr_nl sa{};
  sa.nl_family = AF_NETLINK;
  sa.nl_groups = 1;
  if (::bind(hotplug_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    ::close(hotplug_fd_);
    hotplug_fd_ = -1;
    return;
  }
  hotplug_io_ = Glib::signal_io().connect(
      [this](Glib::IOCondition) {
        char buf[8192];
        bool hit = false;
        ssize_t n;
        while ((n = ::recv(hotplug_fd_, buf, sizeof buf, 0)) > 0) {
          std::string msg(buf, static_cast<size_t>(n));
          if (msg.find(std::string("SUBSYSTEM=drm\0", 14)) != std::string::npos &&
              msg.find("HOTPLUG=1") != std::string::npos)
            hit = true;
        }
        if (hit) {
          // Panels need a moment after link-up before they answer DDC/CI,
          // and one plug often produces several events.
          hotplug_timer_.disconnect();
          hotplug_timer_ = Glib::signal_timeout().connect_seconds(
              [this]() {
                if (discovering_) return true;  // retry after current scan
                discover_async(nullptr);
                return false;
              },
              3);
        }
        return true;
      },
      hotplug_fd_, Glib::IO_IN);
}

void Session::start_polling(int seconds, std::function<bool()> should_poll) {
  poll_timer_.disconnect();
  if (seconds <= 0) return;
  poll_timer_ = Glib::signal_timeout().connect_seconds(
      [this, should_poll]() {
        if (should_poll && !should_poll()) return true;
        if (screens.empty() && !discovering_) {
          discover_async(nullptr);  // monitor may have been plugged in
          return true;
        }
        for (auto& s : screens) {
          std::vector<uint8_t> codes;
          if (s->brightness_code()) codes.push_back(s->brightness_code());
          if (s->contrast_code()) codes.push_back(s->contrast_code());
          s->refresh(codes);
        }
        return true;
      },
      seconds);
}

}  // namespace dc
