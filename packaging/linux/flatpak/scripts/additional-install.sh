#!/bin/sh

set -e

# User Service
mkdir -p ~/.config/systemd/user
cp "/app/share/vibeshine/systemd/user/app-io.github.Nonary.vibeshine.service" "$HOME/.config/systemd/user/app-io.github.Nonary.vibeshine.service"
echo "Vibeshine User Service has been installed."
echo "Use [systemctl --user enable app-io.github.Nonary.vibeshine] once to autostart Vibeshine on login."

# Load uhid (DS5 emulation)
UHID=$(cat /app/share/vibeshine/modules-load.d/60-sunshine.conf)
echo "Enabling DS5 emulation."
# Pass configuration as a literal argument to the host shell.
# shellcheck disable=SC2016
flatpak-spawn --host pkexec sh -c 'printf "%s\n" "$1" > /etc/modules-load.d/60-sunshine.conf' sh "$UHID"
flatpak-spawn --host pkexec modprobe uhid

# Udev rule
UDEV=$(cat /app/share/vibeshine/udev/rules.d/60-sunshine.rules)
echo "Configuring mouse permission."
# shellcheck disable=SC2016
flatpak-spawn --host pkexec sh -c 'printf "%s\n" "$1" > /etc/udev/rules.d/60-sunshine.rules' sh "$UDEV"
echo "Restart computer for mouse permission to take effect."
