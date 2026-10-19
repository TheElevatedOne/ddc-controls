// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <glibmm/keyfile.h>

namespace dc {

struct Limits {
  int min = 0;
  int max = 100;
  bool operator==(const Limits& o) const { return min == o.min && max == o.max; }
};

// ~/.config/ddc-controls/config.ini
//
//   [general]
//   step=5             ; waybar scroll / up / down step in percent
//   poll=30            ; waybar hardware re-read interval in seconds, 0 = off
//   format={brightness}%
//
//   [ddc-AOC-24G2W1G4-12345]
//   lock=true          ; contrast follows brightness
//   limit-12=0;75      ; VCP 0x12 (contrast) restricted to 0..75
class Config {
 public:
  void load();
  bool save();

  int step() const;
  int poll_interval() const;
  std::string waybar_format() const;

  bool lock(const std::string& id) const;
  void set_lock(const std::string& id, bool on);

  // Effective limits clamped to [0, hw_max]. Falls back to
  // [default_min, hw_max] when the user has not configured any.
  Limits limits(const std::string& id, uint8_t code, int hw_max,
                int default_min = 0) const;
  bool has_limits(const std::string& id, uint8_t code) const;
  void set_limits(const std::string& id, uint8_t code, Limits l);
  void clear_limits(const std::string& id, uint8_t code);

 private:
  int get_int(const std::string& group, const std::string& key, int def) const;
  std::unique_ptr<Glib::KeyFile> kf_ = std::make_unique<Glib::KeyFile>();
};

}  // namespace dc
