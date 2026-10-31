#!/usr/bin/env bash
#
# ds4xbox uninstaller.  Run with sudo:
#
#   sudo ./scripts/uninstall.sh
#
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: ejecuta este script como root (sudo ./scripts/uninstall.sh)." >&2
    exit 1
fi

PREFIX="${PREFIX:-/usr/local}"
BINDIR="${BINDIR:-$PREFIX/bin}"
UDEV_DIR="${UDEV_DIR:-/etc/udev/rules.d}"
SYSTEMD_DIR="${SYSTEMD_DIR:-/etc/systemd/system}"
DOCDIR="${DOCDIR:-$PREFIX/share/doc/ds4xbox}"

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop ds4xbox.service 2>/dev/null || true
    systemctl disable ds4xbox.service 2>/dev/null || true
fi

rm -f "$SYSTEMD_DIR/ds4xbox.service" /run/systemd/system/ds4xbox.service
rm -f "$UDEV_DIR/72-ds4xbox.rules" /run/udev/rules.d/72-ds4xbox.rules
rm -f /run/ds4xbox/active
rm -f "$BINDIR/ds4xbox"
rm -rf "$DOCDIR"

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=input --action=change || true
    udevadm trigger --subsystem-match=hidraw --action=change || true
    udevadm trigger --subsystem-match=usb --action=change || true
fi

echo "ds4xbox desinstalado."
echo "Si el mando original sigue sin ser accesible, desconectalo y vuelve a"
echo "conectarlo para que udev aplique de nuevo las reglas por defecto."
