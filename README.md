# ddc-controls

![Static Badge](https://img.shields.io/badge/CLAUDE-VIBECODED?style=for-the-badge&logo=claude&label=VIBECODED&labelColor=%23333333&color=%23de7356)

A GTK3 (gtkmm) monitor control panel in the spirit of
[vdu_controls](https://github.com/digitaltrails/vdu_controls). It talks DDC/CI
directly through **libddcci** and falls back to **brightnessctl** for panels
that do not speak DDC/CI (laptop backlights and `ddcci-driver-linux` devices).
It also ships a **waybar module**, **per-control range limits**, and a
**brightness/contrast lock**.

## Install

### Release binary

Download `ddc-controls-<version>-x86_64-linux.tar.gz` from the
[releases page](https://github.com/TheElevatedOne/ddc-controls/releases).
libddcci is linked in statically, so the only runtime dependency is gtkmm 3
(`gtkmm3` on Arch, `libgtkmm-3.0-1t64` on Debian/Ubuntu), plus brightnessctl
if you have backlight panels.

```sh
tar xf ddc-controls-*-x86_64-linux.tar.gz
sudo cp -r ddc-controls-*-x86_64-linux/usr/* /usr/
```

### From source

```sh
# deps (Arch): gtkmm3 libddcci-git brightnessctl meson
meson setup build
ninja -C build
sudo meson install -C build    # /usr/local by default
```

Needs [libddcci](https://github.com/TheElevatedOne/libddcci) >= 0.4.0. Pass
`-Dstatic_libddcci=true` to link `libddcci.a` instead of the shared library.

DDC/CI needs the `i2c-dev` module and read/write access to `/dev/i2c-*`
(usually by being in the `i2c` group):

```sh
echo i2c-dev | sudo tee /etc/modules-load.d/i2c-dev.conf
sudo usermod -aG i2c $USER
```

## GUI

`ddc-controls` (or `ddc-controls gui`) opens one card per display with
sliders for brightness and contrast; other continuous VCP controls the
monitor reports (RGB gains, volume, sharpness, …) are under *More controls*.
The app is single-instance: running it again brings the existing window up.

* **Link** (padlock): lock contrast to brightness.
* **Settings** button (tools icon): set the min/max limits for each control.
* Header buttons: rescan for displays, re-read values from the hardware.

Slider drags are coalesced on a worker thread per display, so the UI never
blocks on the (slow) I²C bus.

## Limits

Each control can be limited to a sub-range, e.g. contrast 0–75. Sliders,
`set`, `up`/`down`, and waybar scrolling all clamp to it. Limits are in the
monitor's raw units (0–100 on most monitors; for brightnessctl devices,
0–`max_brightness`, with a default minimum of 1 so the panel doesn't blank).

## Lock

With the lock on, moving brightness moves contrast proportionally *through
its limited range* (and the reverse when you move contrast):

    contrast = c_min + (b - b_min) / (b_max - b_min) * (c_max - c_min)

| Brightness (0–100) | Contrast (limit 0–75) |
|---:|---:|
| 100 | 75 |
| 50 | 38 |
| 35 | 26 |
| 0 | 0 |

Turning the lock on immediately syncs contrast to the current brightness.

## CLI

```
ddc-controls [-d DISPLAY] list
ddc-controls [-d DISPLAY] get [brightness|contrast|0x16|red-gain ...]
ddc-controls [-d DISPLAY] set FEATURE VALUE   # 60, 60%, +5, -5, +5%, -5%
ddc-controls [-d DISPLAY] up|down [STEP]      # brightness, STEP percent
ddc-controls [-d DISPLAY] lock on|off|toggle
ddc-controls refresh | rescan
```

`DISPLAY` is `all` (default), an index from `list`, the display id, or a
substring of its name or connector (`DP-3`, `AOC`, `intel_backlight`).

## Waybar

The stock `backlight` module only sees `/sys/class/backlight`. Run
`ddc-controls waybar` as a custom module instead (`data/waybar-module.jsonc`):

```jsonc
"custom/ddc": {
    "exec": "ddc-controls waybar",          // or: ddc-controls waybar -d DP-3
    "return-type": "json",
    "format": "{icon} {}",
    "format-icons": ["󰃞", "󰃟", "󰃝", "󰃠"],
    "on-scroll-up": "ddc-controls up",
    "on-scroll-down": "ddc-controls down",
    "on-click": "ddc-controls gui",
    "on-click-middle": "ddc-controls lock toggle",
    "on-click-right": "ddc-controls set brightness 50%"
}
```

The module emits `text`, `tooltip` (all displays with values and limits),
`percentage` (brightness, for `format-icons`), `alt` and `class`
(`locked`/`unlocked`, see `data/waybar-style.css`).
Options: `--format '{brightness}% {lock}'` (placeholders `{brightness}`,
`{contrast}`, `{name}`, `{lock}`) and `--poll SEC` (re-read hardware to catch
changes made from the monitor's OSD; `0` disables).

The daemon keeps the I²C handles open and listens on
`$XDG_RUNTIME_DIR/ddc-controls/control.sock`, so scroll commands take
milliseconds and rapid scrolling is coalesced instead of queuing up dozens of
bus transactions. Without a daemon, commands open the hardware directly
(~1 s, mostly display detection). With several bars (one per output), one
daemon owns the socket; the others follow the shared state file and take over
if it exits.

The GUI, the daemon and one-off commands share their latest values through
`$XDG_RUNTIME_DIR/ddc-controls/state.ini` and watch `config.ini`, so a change
made in one place shows up everywhere. Bus access is serialized between
processes with a lock file per I²C bus.

## Configuration

`~/.config/ddc-controls/config.ini` (written by the GUI; safe to edit by hand,
changes are picked up live):

```ini
[general]
step=5                 ; up/down and scroll step, percent
poll=30                ; waybar hardware poll interval, seconds (0 = off)
format={brightness}%   ; waybar text

[ddc-AOC-24G2W1G4-81532]   ; id from `ddc-controls list`
lock=true
limit-12=0;75              ; VCP 0x12 (contrast) limited to 0..75
limit-10=10;100            ; brightness 10..100
```

Display ids come from the EDID (manufacturer, model, serial), so settings
follow a monitor across ports and reboots. Identical monitors without serial
numbers get the I²C bus appended to keep them apart.

## Hot-plug

The GUI and the waybar daemon listen for DRM hot-plug uevents and rescan a few
seconds after a monitor is connected or removed. The current displays stay
usable while the rescan runs.

## Releasing

Bump `version` in `meson.build`, then run the **Build and release** workflow
(Actions tab → *Run workflow*). It builds on Ubuntu 24.04 with libddcci linked
statically and publishes release `v<version>` with the binary tarball, the
source tarball and `SHA256SUMS`. Running it again for the same version
replaces the assets.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). libddcci is MIT-licensed.
