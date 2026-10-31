#!/usr/bin/env bash
#
# Ayuda para emparejar un DualShock 4 / DualSense por Bluetooth.
# No requiere root si bluetoothctl puede hablar con BlueZ (polkit/D-Bus).
#
set -euo pipefail

if ! command -v bluetoothctl >/dev/null 2>&1; then
    echo "ERROR: bluetoothctl no esta instalado (paquete bluez)." >&2
    exit 1
fi

cat <<'EOF'
Pon el mando en modo emparejamiento:
  - DualShock 4: mantén pulsados Share + PS hasta que la barra de luz
    parpadee rápidamente.
  - DualSense: mantén pulsados Create + PS hasta que la barra parpadee.
  - Si está conectado por USB, desenchúfalo antes de emparejar.

Buscando mandos durante 30 segundos...
EOF

if bluetoothctl --help 2>&1 | grep -q -- '--timeout'; then
    bluetoothctl --timeout 30 scan on || true
else
    timeout 30 bluetoothctl scan on || true
fi

echo
echo "Dispositivos conocidos:"
bluetoothctl devices || true
echo
read -r -p "Introduce la MAC del mando (por ejemplo AA:BB:CC:DD:EE:FF): " MAC
if [ -z "${MAC:-}" ]; then
    echo "No se introdujo ninguna MAC." >&2
    exit 1
fi

echo "Emparejando $MAC..."
bluetoothctl pair "$MAC" || true
bluetoothctl trust "$MAC" || true
bluetoothctl connect "$MAC" || true

echo
echo "Hecho.  Comprueba el daemon con:"
echo "  systemctl status ds4xbox.service"
echo "  journalctl -u ds4xbox.service -f"
echo "  ds4xbox --list"
