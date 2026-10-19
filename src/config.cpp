// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#include "config.hpp"

#include <vector>

#include "util.hpp"

namespace dc {

namespace {
std::string limit_key(uint8_t code) { return "limit-" + hex2(code); }
}  // namespace

void Config::load() {
  auto fresh = std::make_unique<Glib::KeyFile>();
  try {
    fresh->load_from_file(config_path(), Glib::KEY_FILE_KEEP_COMMENTS);
  } catch (const Glib::Error&) {
    // Missing or malformed: start from defaults.
  }
  kf_ = std::move(fresh);
}

bool Config::save() {
  try {
    return kf_->save_to_file(config_path());
  } catch (const Glib::Error&) {
    return false;
  }
}

int Config::get_int(const std::string& group, const std::string& key,
                    int def) const {
  try {
    if (kf_->has_group(group) && kf_->has_key(group, key))
      return kf_->get_integer(group, key);
  } catch (const Glib::Error&) {
  }
  return def;
}

int Config::step() const { return clampi(get_int("general", "step", 5), 1, 100); }

int Config::poll_interval() const {
  return clampi(get_int("general", "poll", 30), 0, 86400);
}

std::string Config::waybar_format() const {
  try {
    if (kf_->has_group("general") && kf_->has_key("general", "format"))
      return kf_->get_string("general", "format");
  } catch (const Glib::Error&) {
  }
  return "{brightness}%";
}

bool Config::lock(const std::string& id) const {
  try {
    if (kf_->has_group(id) && kf_->has_key(id, "lock"))
      return kf_->get_boolean(id, "lock");
  } catch (const Glib::Error&) {
  }
  return false;
}

void Config::set_lock(const std::string& id, bool on) {
  kf_->set_boolean(id, "lock", on);
}

bool Config::has_limits(const std::string& id, uint8_t code) const {
  try {
    return kf_->has_group(id) && kf_->has_key(id, limit_key(code));
  } catch (const Glib::Error&) {
    return false;
  }
}

Limits Config::limits(const std::string& id, uint8_t code, int hw_max,
                      int default_min) const {
  Limits l{clampi(default_min, 0, hw_max), hw_max};
  if (has_limits(id, code)) {
    try {
      std::vector<int> v = kf_->get_integer_list(id, limit_key(code));
      if (v.size() == 2) {
        l.min = clampi(v[0], 0, hw_max);
        l.max = clampi(v[1], 0, hw_max);
        if (l.min > l.max) std::swap(l.min, l.max);
      }
    } catch (const Glib::Error&) {
    }
  }
  return l;
}

void Config::set_limits(const std::string& id, uint8_t code, Limits l) {
  if (l.min > l.max) std::swap(l.min, l.max);
  kf_->set_integer_list(id, limit_key(code), std::vector<int>{l.min, l.max});
}

void Config::clear_limits(const std::string& id, uint8_t code) {
  try {
    if (has_limits(id, code)) kf_->remove_key(id, limit_key(code));
  } catch (const Glib::Error&) {
  }
}

}  // namespace dc
