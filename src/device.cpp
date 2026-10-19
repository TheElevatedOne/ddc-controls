// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
#include "device.hpp"

#include <ddcci.h>
#include <dirent.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include <glib.h>

#include "util.hpp"

namespace dc {

namespace {

struct KnownVcp {
  uint8_t code;
  const char* name;
};

// Continuous MCCS controls worth showing as sliders.
const KnownVcp kKnown[] = {
    {0x10, "Brightness"},       {0x12, "Contrast"},
    {0x13, "Backlight"},        {0x6B, "Backlight (white)"},
    {0x16, "Red gain"},         {0x18, "Green gain"},
    {0x1A, "Blue gain"},        {0x6C, "Red black level"},
    {0x6E, "Green black level"},{0x70, "Blue black level"},
    {0x62, "Volume"},           {0x87, "Sharpness"},
    {0x8A, "Saturation"},       {0x90, "Hue"},
    {0x0E, "Clock"},            {0x20, "Horizontal position"},
    {0x30, "Vertical position"},
};

std::string sanitize(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
      out += static_cast<char>(c);
    else if (c == ' ' && !out.empty() && out.back() != '_')
      out += '_';
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

bool transient(ddcci_status_t st) {
  return st == DDCCI_ERR_IO || st == DDCCI_ERR_CHECKSUM ||
         st == DDCCI_ERR_TIMEOUT || st == DDCCI_ERR_BUSY;
}

class DdcDevice : public Device {
 public:
  DdcDevice(ddcci_display* h, const ddcci_info& info) : h_(h), bus_(info.bus) {
    kind_ = "DDC/CI";
    const ddcci_edid& e = info.edid;
    std::string model = e.model[0] ? e.model : "Monitor";
    std::string mfr = e.manufacturer;
    label_ = mfr.empty() ? model : mfr + " " + model;
    location_ = info.connector[0] ? std::string(info.connector)
                                  : std::string(info.path);

    std::string serial = e.serial[0] ? std::string(e.serial)
                         : e.serial_number ? std::to_string(e.serial_number)
                                           : std::string();
    if (info.edid_ok) {
      id_ = "ddc-" + sanitize(mfr) + "-" + sanitize(model);
      if (!serial.empty()) id_ += "-" + sanitize(serial);
      else id_ += "-" + std::to_string(e.product_code);
    } else {
      id_ = "ddc-bus" + std::to_string(bus_);
    }
    lock_path_ = runtime_dir() + "/bus-" + std::to_string(bus_) + ".lock";
  }

  ~DdcDevice() override {
    if (h_) ddcci_close(h_);
  }

  bool read(uint8_t code, int& value, int& max, std::string& err) override {
    FileLock lock(lock_path_);
    ddcci_feature f{};
    ddcci_status_t st = DDCCI_OK;
    for (int attempt = 0; attempt < 3; ++attempt) {
      st = ddcci_get_vcp(h_, code, &f);
      if (st == DDCCI_OK || !transient(st)) break;
      usleep(40000);
    }
    if (st != DDCCI_OK) {
      err = "read VCP 0x" + hex2(code) + ": " + ddcci_strerror(st);
      return false;
    }
    value = f.current;
    max = f.maximum;
    return true;
  }

  bool write(uint8_t code, int value, std::string& err) override {
    FileLock lock(lock_path_);
    ddcci_status_t st = DDCCI_OK;
    for (int attempt = 0; attempt < 3; ++attempt) {
      st = ddcci_set_vcp(h_, code, static_cast<uint16_t>(value));
      if (st == DDCCI_OK || !transient(st)) break;
      usleep(40000);
    }
    // libddcci applies the post-write settle delay before the *next* command
    // on this handle only. Other processes use their own handle, so honour
    // the spec's 50 ms here while we still hold the bus lock.
    usleep(50000);
    if (st != DDCCI_OK) {
      err = "write VCP 0x" + hex2(code) + ": " + ddcci_strerror(st);
      return false;
    }
    return true;
  }

  void probe_features(const ddcci_info& info) {
    std::set<uint8_t> seen;
    auto add = [&](uint8_t code, int cur, int max) {
      if (seen.count(code) || max <= 0) return;
      seen.insert(code);
      const char* n = vcp_name(code);
      FeatureInfo fi;
      fi.code = code;
      fi.name = n ? n : "VCP 0x" + hex2(code);
      fi.value = cur;
      fi.max = max;
      features.push_back(fi);
    };

    if (info.brightness.present && info.brightness.maximum > 0) {
      brightness_code = info.brightness.opcode;
      add(brightness_code, info.brightness.current, info.brightness.maximum);
      features.back().name = "Brightness";
    }
    if (info.contrast.present && info.contrast.maximum > 0) {
      contrast_code = info.contrast.opcode;
      add(contrast_code, info.contrast.current, info.contrast.maximum);
    }
    for (const auto& k : kKnown) {
      if (seen.count(k.code) || !ddcci_info_has_vcp(&info, k.code)) continue;
      // Backlight aliases are redundant once brightness resolved.
      if ((k.code == 0x13 || k.code == 0x6B) && brightness_code) continue;
      int v = 0, m = 0;
      std::string err;
      if (read(k.code, v, m, err)) add(k.code, v, m);
    }
  }

  void disambiguate() override { id_ += "-bus" + std::to_string(bus_); }

 private:
  ddcci_display* h_;
  int bus_;
  std::string lock_path_;
};

// Backlight devices (laptop panels, ddcci-driver backlights, ...) driven
// through the brightnessctl CLI.
class BacklightDevice : public Device {
 public:
  BacklightDevice(const std::string& name, int cur, int max) : name_(name) {
    kind_ = "Backlight";
    id_ = "backlight-" + sanitize(name);
    label_ = name;
    location_ = "/sys/class/backlight/" + name;
    brightness_code = VCP_BRIGHTNESS;
    FeatureInfo fi;
    fi.code = VCP_BRIGHTNESS;
    fi.name = "Brightness";
    fi.value = cur;
    fi.max = max;
    fi.default_min = max > 1 ? 1 : 0;  // 0 blanks most laptop panels
    features.push_back(fi);
  }

  bool read(uint8_t code, int& value, int& max, std::string& err) override {
    if (code != VCP_BRIGHTNESS) {
      err = "unsupported";
      return false;
    }
    std::string out;
    if (!run({"brightnessctl", "-m", "-d", name_, "info"}, out, err))
      return false;
    // device,class,current,percent,max
    std::vector<std::string> f;
    std::stringstream ss(out);
    std::string tok;
    while (std::getline(ss, tok, ',')) f.push_back(tok);
    if (f.size() < 5) {
      err = "unexpected brightnessctl output";
      return false;
    }
    value = std::atoi(f[2].c_str());
    max = std::atoi(f[4].c_str());
    return true;
  }

  bool write(uint8_t code, int value, std::string& err) override {
    if (code != VCP_BRIGHTNESS) {
      err = "unsupported";
      return false;
    }
    std::string out;
    return run({"brightnessctl", "-q", "-d", name_, "set",
                std::to_string(value)},
               out, err);
  }

  static bool run(const std::vector<std::string>& args, std::string& out,
                  std::string& err) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    gchar* sout = nullptr;
    gchar* serr = nullptr;
    gint status = 0;
    GError* gerr = nullptr;
    bool ok = g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH,
                           nullptr, nullptr, &sout, &serr, &status, &gerr);
    if (!ok) {
      err = gerr ? gerr->message : "spawn failed";
      if (gerr) g_error_free(gerr);
      return false;
    }
    out = sout ? sout : "";
    std::string e = serr ? serr : "";
    g_free(sout);
    g_free(serr);
    if (!g_spawn_check_wait_status(status, nullptr)) {
      err = "brightnessctl failed" + (e.empty() ? "" : ": " + e);
      return false;
    }
    return true;
  }

 private:
  std::string name_;
};

// Takes every /dev/i2c-N lock, in bus order, for the duration of a full
// probe. Other processes (and our own workers) hold at most one at a time,
// so ascending order cannot deadlock.
class AllBusLocks {
 public:
  AllBusLocks() {
    std::set<int> buses;
    if (DIR* d = opendir("/dev")) {
      while (dirent* e = readdir(d)) {
        if (std::strncmp(e->d_name, "i2c-", 4) == 0)
          buses.insert(std::atoi(e->d_name + 4));
      }
      closedir(d);
    }
    for (int b : buses)
      locks_.push_back(std::make_unique<FileLock>(runtime_dir() + "/bus-" + std::to_string(b) + ".lock"));
  }

 private:
  std::vector<std::unique_ptr<FileLock>> locks_;
};

}  // namespace

const char* vcp_name(uint8_t code) {
  for (const auto& k : kKnown)
    if (k.code == code) return k.name;
  return nullptr;
}

std::vector<DevicePtr> discover_devices(std::string* log) {
  std::vector<DevicePtr> out;
  std::set<int> ddc_buses;
  auto note = [&](const std::string& s) {
    if (log) *log += s + "\n";
  };

  ddcci_info* list = nullptr;
  size_t count = 0;
  ddcci_status_t st;
  {
    AllBusLocks hold;  // released before probe_features() takes per-bus locks
    st = ddcci_find_displays(&list, &count);
  }
  if (st != DDCCI_OK) {
    note(std::string("libddcci: ") + ddcci_strerror(st));
  } else {
    for (size_t i = 0; i < count; ++i) {
      const ddcci_info& info = list[i];
      if (!info.accessible) {
        note(std::string(info.path) + ": not accessible (i2c group / i2c-dev?)");
        continue;
      }
      if (!info.ddc_supported) continue;
      ddcci_display* h = nullptr;
      ddcci_status_t ost = info.bus >= 0 ? ddcci_open(info.bus, &h)
                                         : ddcci_open_path(info.path, &h);
      if (ost != DDCCI_OK) {
        note(std::string(info.path) + ": " + ddcci_strerror(ost));
        continue;
      }
      auto dev = std::make_shared<DdcDevice>(h, info);
      dev->probe_features(info);
      if (dev->features.empty()) {
        note(dev->label() + ": no continuous controls");
        continue;
      }
      ddc_buses.insert(info.bus);
      out.push_back(dev);
    }
    ddcci_free_info_list(list);
  }

  // Fallback: anything brightnessctl can drive that DDC/CI did not cover.
  std::string lines, err;
  if (BacklightDevice::run({"brightnessctl", "-l", "-m"}, lines, err)) {
    std::stringstream ss(lines);
    std::string line;
    while (std::getline(ss, line)) {
      std::vector<std::string> f;
      std::stringstream ls(line);
      std::string tok;
      while (std::getline(ls, tok, ',')) f.push_back(tok);
      if (f.size() < 5 || f[1] != "backlight") continue;
      const std::string& name = f[0];
      // ddcci-driver-linux exposes "ddcciN" for bus N; skip it when we
      // already control that bus directly.
      if (name.rfind("ddcci", 0) == 0) {
        int bus = std::atoi(name.c_str() + 5);
        if (ddc_buses.count(bus)) continue;
      }
      out.push_back(std::make_shared<BacklightDevice>(
          name, std::atoi(f[2].c_str()), std::atoi(f[4].c_str())));
    }
  } else {
    note("brightnessctl: " + err);
  }

  // Identical monitors without EDID serials would share an id (and config).
  std::map<std::string, int> seen;
  for (auto& d : out) seen[d->id()]++;
  for (auto& d : out) {
    if (seen[d->id()] > 1) d->disambiguate();
  }
  return out;
}

}  // namespace dc
