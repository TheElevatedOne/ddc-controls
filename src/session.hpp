// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <giomm/filemonitor.h>
#include <glibmm/main.h>
#include <sigc++/sigc++.h>

#include "config.hpp"
#include "device.hpp"
#include "worker.hpp"

namespace dc {

// Proportional mapping used by the brightness/contrast lock:
//   from=[0,100], to=[0,75]:  100 -> 75, 50 -> 38, 0 -> 0
int map_linked(int value, Limits from, Limits to);

struct FeatureState {
  uint8_t code = 0;
  std::string name;
  int value = 0;
  int max = 0;
  int default_min = 0;
};

using Changes = std::map<uint8_t, int>;

class Session;

// Front-end view of one Device: cached values, limits, lock. Lives on the
// main thread; hardware I/O happens on its Worker.
class Screen : public std::enable_shared_from_this<Screen> {
 public:
  Screen(Session& session, DevicePtr dev);
  ~Screen();
  void start();

  const std::string& id() const { return dev_->id(); }
  const std::string& label() const { return dev_->label(); }
  const std::string& kind() const { return dev_->kind(); }
  const std::string& location() const { return dev_->location(); }

  const std::vector<FeatureState>& features() const { return features_; }
  const FeatureState* feature(uint8_t code) const;
  uint8_t brightness_code() const { return dev_->brightness_code; }
  uint8_t contrast_code() const { return dev_->contrast_code; }
  bool can_lock() const { return brightness_code() && contrast_code(); }

  Limits limits(uint8_t code) const;
  bool has_custom_limits(uint8_t code) const;
  bool locked() const;

  // User actions. Apply limits and the lock, queue hardware writes,
  // publish to other processes, emit signal_changed, return what changed.
  Changes set_value(uint8_t code, int value);
  Changes step_percent(uint8_t code, double percent);
  Changes set_locked(bool on);
  Changes enforce_limits();

  void refresh();
  void refresh(const std::vector<uint8_t>& codes);
  void flush();

  // A value reported by another process (state file).
  void apply_external(uint8_t code, int value, int max);

  sigc::signal<void, const Changes&>& signal_changed() { return changed_; }
  sigc::signal<void, const std::string&>& signal_error() { return error_; }

 private:
  FeatureState* mut(uint8_t code);
  bool held(uint8_t code) const;
  void on_result(Worker::Result r);

  Session& session_;
  DevicePtr dev_;
  std::vector<FeatureState> features_;
  std::unique_ptr<Worker> worker_;
  std::map<uint8_t, gint64> last_local_set_;
  sigc::signal<void, const Changes&> changed_;
  sigc::signal<void, const std::string&> error_;
};

using ScreenPtr = std::shared_ptr<Screen>;

class Session {
 public:
  Session();
  ~Session();

  Config config;
  std::vector<ScreenPtr> screens;

  void discover_sync(std::string* log = nullptr);
  void discover_async(std::function<void(const std::string& log)> done);
  bool discovering() const { return discovering_; }
  void clear();

  // "" / "all" -> every screen; N -> index; exact id; else substring of
  // id, label or connector (case-insensitive).
  std::vector<ScreenPtr> select(const std::string& selector) const;

  void save_config();

  // Cross-process sharing through $XDG_RUNTIME_DIR/ddc-controls/state.ini.
  void queue_publish(const Screen& s, const std::set<uint8_t>& codes);
  void publish_now();
  // Watch state/config files and DRM hot-plug events (rescans on plug).
  void watch();

  // Periodically re-read brightness/contrast (catches OSD changes).
  void start_polling(int seconds, std::function<bool()> should_poll = {});

  sigc::signal<void>& signal_config_changed() { return config_changed_; }
  sigc::signal<void>& signal_screens_changed() { return screens_changed_; }

 private:
  void adopt(std::vector<DevicePtr> devs);
  void watch_hotplug();
  void load_state();

  bool discovering_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::map<std::string, std::set<uint8_t>> pending_publish_;
  sigc::connection publish_timer_, state_timer_, config_timer_, poll_timer_;
  sigc::connection hotplug_io_, hotplug_timer_;
  int hotplug_fd_ = -1;
  Glib::RefPtr<Gio::FileMonitor> state_mon_, config_mon_;
  sigc::signal<void> config_changed_, screens_changed_;
};

}  // namespace dc
