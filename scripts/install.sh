#!/usr/bin/env bash
#
# ds4xbox installer.  Run with sudo:
#
#   sudo ./scripts/install.sh
#
# Optional environment variables:
#   PREFIX=/usr/local
#   BINDIR=/usr/local/bin
#   UDEV_DIR=/etc/udev/rules.d
#   SYSTEMD_DIR=/etc/systemd/system
#   DOCDIR=/usr/local/share/doc/ds4xbox
#   KERNEL_HEADERS=/path/to/linux-headers/include
#   NO_SYSTEMD=1
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: ejecuta este script como root (sudo ./scripts/install.sh)." >&2
    exit 1
fi

if [ ! -e /dev/uinput ]; then
    echo "AVISO: /dev/uinput no existe; intentando cargar el modulo uinput..."
    if command -v modprobe >/dev/null 2>&1; then
        modprobe uinput 2>/dev/null || true
    fi
fi
if [ ! -e /dev/uinput ]; then
    echo "ERROR: /dev/uinput sigue sin existir." >&2
    echo "  Carga el modulo uinput y vuelve a intentarlo:" >&2
    echo "    sudo modprobe uinput" >&2
    echo "  En NixOS anade: boot.kernelModules = [ \"uinput\" ];" >&2
    exit 1
fi

PREFIX="${PREFIX:-/usr/local}"
BINDIR="${BINDIR:-$PREFIX/bin}"
UDEV_DIR="${UDEV_DIR:-/etc/udev/rules.d}"
SYSTEMD_DIR="${SYSTEMD_DIR:-/etc/systemd/system}"
DOCDIR="${DOCDIR:-$PREFIX/share/doc/ds4xbox}"
TRANSIENT=0

# NixOS mounts /etc from the store read-only.  For a quick test we can use
# /run/udev/rules.d and /run/systemd/system, which udev/systemd read but
# which disappear on reboot.  Permanent installs should use the NixOS
# module instead.
if grep -q '^ID=nixos' /etc/os-release 2>/dev/null &&
   { [ ! -w /etc/udev/rules.d ] || [ ! -w /etc/systemd/system ]; }; then
    echo "NixOS detectado: /etc es de solo lectura."
    echo "Se usara una instalacion temporal en /run para probar ahora."
    echo "Para una instalacion permanente, usa el modulo NixOS (ver README)."
    UDEV_DIR="/run/udev/rules.d"
    SYSTEMD_DIR="/run/systemd/system"
    TRANSIENT=1
fi

if [ ! -x "$REPO_DIR/ds4xbox" ]; then
    echo "Compilando ds4xbox..."
    make_args=()
    if [ -n "${KERNEL_HEADERS:-}" ]; then
        make_args+=("KERNEL_HEADERS=$KERNEL_HEADERS")
    fi
    make -C "$REPO_DIR" "${make_args[@]}"
fi

echo "Instalando binario en $BINDIR/ds4xbox"
install -Dm755 "$REPO_DIR/ds4xbox" "$BINDIR/ds4xbox"

echo "Instalando regla udev en $UDEV_DIR/72-ds4xbox.rules"
install -Dm644 "$REPO_DIR/udev/72-ds4xbox.rules" "$UDEV_DIR/72-ds4xbox.rules"

echo "Instalando unidad systemd en $SYSTEMD_DIR/ds4xbox.service"
install -Dm644 "$REPO_DIR/systemd/ds4xbox.service.in" "$SYSTEMD_DIR/ds4xbox.service"
sed -i "s|@BINDIR@|$BINDIR|g" "$SYSTEMD_DIR/ds4xbox.service"

echo "Instalando documentacion en $DOCDIR"
install -Dm644 "$REPO_DIR/README.md" "$DOCDIR/README.md"
install -Dm644 "$REPO_DIR/LICENSE" "$DOCDIR/LICENSE"
for doc in ARCHITECTURE TROUBLESHOOTING COMPATIBILITY; do
    install -Dm644 "$REPO_DIR/docs/$doc.md" "$DOCDIR/$doc.md"
done

if command -v udevadm >/dev/null 2>&1; then
    echo "Recargando reglas udev..."
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=input --action=change || true
    udevadm trigger --subsystem-match=hidraw --action=change || true
    udevadm trigger --subsystem-match=usb --action=change || true
else
    echo "AVISO: no se encontro udevadm; recarga las reglas udev manualmente."
fi

if command -v systemctl >/dev/null 2>&1 && [ "${NO_SYSTEMD:-0}" != "1" ]; then
    echo "Activando el servicio ds4xbox..."
    systemctl daemon-reload
    if [ "$TRANSIENT" = "1" ]; then
        systemctl restart ds4xbox.service
        echo
        echo "Servicio TEMPORAL activo (desaparece al reiniciar)."
        echo "Para que sea permanente en NixOS, usa el modulo del README."
    else
        systemctl enable ds4xbox.service
        systemctl restart ds4xbox.service
        echo
        echo "Servicio activo."
    fi
    echo "Comprueba su estado con:"
    echo "  systemctl status ds4xbox.service"
    echo "  journalctl -u ds4xbox.service -f"
else
    echo
    echo "Systemd no esta disponible o NO_SYSTEMD=1."
    echo "Ejecuta el daemon manualmente como root:"
    echo "  sudo $BINDIR/ds4xbox --verbose"
fi

echo
echo "Siguientes pasos:"
echo "  1. Conecta el mando por USB o emparejalo por Bluetooth:"
echo "       ./scripts/pair-bluetooth.sh"
echo "  2. Comprueba que aparece un mando Xbox virtual:"
echo "       $BINDIR/ds4xbox --list"
echo "  3. Si algo falla:"
echo "       ./scripts/diagnose.sh"
echo "       journalctl -u ds4xbox.service -n 100 --no-pager"
echo
echo "El mando original aparecera como 'permission denied' en --list si"
echo "ejecutas como usuario: eso es intencionado.  El mando virtual Xbox"
echo "debe ser visible y usable."
