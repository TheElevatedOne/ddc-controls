// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dc {

constexpr uint8_t VCP_BRIGHTNESS = 0x10;
constexpr uint8_t VCP_CONTRAST = 0x12;

struct FeatureInfo {
  uint8_t code = 0;
  std::string name;
  int value = 0;
  int max = 0;
  int default_min = 0;  // lower limit used when the user has not set one
};

// A controllable display. Not thread-safe: after discovery a Device is used
// only from its Worker thread.
class Device {
 public:
  virtual ~Device() = default;

  const std::string& id() const { return id_; }
  const std::string& label() const { return label_; }
  const std::string& kind() const { return kind_; }
  const std::string& location() const { return location_; }

  std::vector<FeatureInfo> features;
  uint8_t brightness_code = 0;  // 0 = none
  uint8_t contrast_code = 0;

  virtual bool read(uint8_t code, int& value, int& max, std::string& err) = 0;
  virtual bool write(uint8_t code, int value, std::string& err) = 0;
  // Make id() unique when two devices report identical identities.
  virtual void disambiguate() {}

 protected:
  std::string id_;        // stable across reboots / bus renumbering
  std::string label_;     // human readable name
  std::string kind_;      // "DDC/CI" or "Backlight"
  std::string location_;  // connector / bus / sysfs name
};

using DevicePtr = std::shared_ptr<Device>;

// Probe DDC/CI displays via libddcci, then add brightnessctl backlight
// devices for panels that are not reachable over DDC/CI. Slow (~1 s).
std::vector<DevicePtr> discover_devices(std::string* log = nullptr);

// MCCS name for a continuous VCP code we know how to present, or nullptr.
const char* vcp_name(uint8_t code);

}  // namespace dc
