# Joynosleep

When I play some Windows games in wine with gamepad only, screensaver (or monitor sleep) steps in and dims my screen.

A simple idea came to my mind: what if I monitor pressed buttons, and postpone screensaver while gamepad is used?

And here is a simple implementation written in C.

For a python version that doesn't require compilation, see `prototype` branch.

## Features

- Minimal dependencies (libsystemd only)
- Low resource usage
- Hot plug autodetection
- Should work with any DE supporting org.freedesktop.ScreenSaver DBus interface

## Build

Requires [meson](https://mesonbuild.com/), [ninja](https://ninja-build.org/) and [libsystemd](https://systemd.io/)

```shell
meson setup --prefix=/usr build
meson compile -C build
```

## Usage

Run `./build/joynosleep`

## Install

```shell
sudo meson install -C build
install -D -m 0644 systemd/joynosleep.service ~/.config/systemd/user/joynosleep.service
systemctl --user daemon-reload
systemctl --user start joynosleep
```

## TODO

- Wake up from sleep with button press
  This requires using DPMS
