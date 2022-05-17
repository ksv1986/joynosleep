# Joynosleep

When I play some Windows games in wine with gamepad only, screensaver (or monitor sleep) steps in and dims my screen.

A simple idea came to my mind: what if I monitor pressed buttons, and postpone screensaver while gamepad is used?

And here is a quick-n-dirty python implementation.

## Features

- Hot plug autodetection
- Should work with any DE supporting org.freedesktop.ScreenSaver DBus interface

## Usage

Run `joynosleep`

## Install

```shell
install -D -m 0755 joynosleep /usr/bin/joynosleep
install -D -m 0644 systemd/joynosleep.service ~/.config/systemd/user/joynosleep.service
systemctl --user daemon-reload
systemctl --user start joynosleep
```

## TODO

- Wake up from sleep with button press
  This requires using DPMS, and therefore easier to do in C
