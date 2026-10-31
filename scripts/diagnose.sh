#!/usr/bin/env bash
#
# Diagnostico rapido de ds4xbox.  Se puede ejecutar como usuario normal;
# algunas comprobaciones necesitan root y se indican.
#
set -u

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

section() {
    echo
    echo "=== $* ==="
}

section "Sistema"
uname -a
echo "Usuario: $(id)"
echo "Kernel: $(uname -r)"

section "uinput"
if [ -e /dev/uinput ]; then
    ls -l /dev/uinput
else
    echo "FALTA /dev/uinput.  Prueba: sudo modprobe uinput"
fi
if command -v lsmod >/dev/null 2>&1; then
    lsmod | grep -E '^uinput' || echo "El modulo uinput no aparece en lsmod."
fi

section "Regla udev"
rule_found=0
for rule in /etc/udev/rules.d/72-ds4xbox.rules /run/udev/rules.d/72-ds4xbox.rules; do
    if [ -f "$rule" ]; then
        echo "OK: $rule"
        grep -nE 'KERNELS|uaccess|MODE' "$rule" | head -20
        rule_found=1
    fi
done
if [ "$rule_found" -ne 1 ]; then
    echo "FALTA la regla 72-ds4xbox.rules (ni en /etc ni en /run)."
    echo "Instala con: sudo ./scripts/install.sh"
fi

section "Servicio systemd"
if command -v systemctl >/dev/null 2>&1; then
    systemctl status ds4xbox.service --no-pager -l || true
    echo
    echo "--- ultimos registros ---"
    journalctl -u ds4xbox.service -n 40 --no-pager || true
else
    echo "systemd no disponible."
fi

section "Dispositivos de entrada"
ls -l /dev/input 2>/dev/null || true
echo
echo "Nombres de los event devices:"
for f in /sys/class/input/event*/device/name; do
    [ -e "$f" ] || continue
    printf '%s: %s\n' "$(basename "$(dirname "$(dirname "$f")")")" "$(cat "$f")"
done

section "ds4xbox --list"
if [ -x "$REPO_DIR/ds4xbox" ]; then
    "$REPO_DIR/ds4xbox" --list || true
elif command -v ds4xbox >/dev/null 2>&1; then
    ds4xbox --list || true
else
    echo "No se encontro el binario ds4xbox."
fi

section "Herramientas utiles"
for tool in evtest jstest sdl2-jstest udevadm; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "OK: $tool -> $(command -v "$tool")"
    else
        echo "No instalado: $tool"
    fi
done

cat <<'EOF'

Notas:
  - Si --list muestra "permission denied" para el mando Sony, es correcto:
    las reglas udev lo ocultan y solo root puede abrirlo.
  - El mando virtual "Microsoft X-Box 360 pad" debe ser visible y usable.
  - Si el daemon no arranca, revisa journalctl y prueba manualmente:
      sudo ./ds4xbox --verbose
  - Si otro programa tiene el mando capturado (EBUSY), cierra Steam,
    ds4drv, antimicro, etc.
EOF
