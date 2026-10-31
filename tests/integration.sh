#!/usr/bin/env bash
#
# Test de integración de ds4xbox con un DualShock 4 falso.
#
# Necesita root porque crea dispositivos uinput y lee nodos evdev:
#
#   sudo tests/integration.sh
#
# Si el servicio ds4xbox está activo, se para durante el test y se vuelve a
# arrancar al terminar.
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: este test necesita root (sudo tests/integration.sh)." >&2
    exit 1
fi

make_args=()
if [ -n "${KERNEL_HEADERS:-}" ]; then
    make_args+=("KERNEL_HEADERS=$KERNEL_HEADERS")
fi

echo "Compilando ds4xbox y las herramientas de test..."
make -C "$REPO_DIR" "${make_args[@]}"
make -C "$REPO_DIR" "${make_args[@]}" tests

tmpdir="$(mktemp -d)"
fake_pid=""
daemon_pid=""
reader_pid=""
service_was_active=0

cleanup() {
    set +e
    if [ -n "$reader_pid" ]; then
        kill "$reader_pid" 2>/dev/null
        wait "$reader_pid" 2>/dev/null
    fi
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" 2>/dev/null
        wait "$daemon_pid" 2>/dev/null
    fi
    if [ -n "$fake_pid" ]; then
        kill "$fake_pid" 2>/dev/null
        wait "$fake_pid" 2>/dev/null
    fi
    if [ "$service_was_active" = "1" ]; then
        systemctl start ds4xbox.service 2>/dev/null
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

if command -v systemctl >/dev/null 2>&1 &&
   systemctl is-active --quiet ds4xbox.service 2>/dev/null; then
    echo "Parando ds4xbox.service durante el test..."
    service_was_active=1
    systemctl stop ds4xbox.service
fi

echo "Creando DualShock 4 falso..."
"$REPO_DIR/tests/fake_ds4" --demo > "$tmpdir/fake.out" 2>&1 &
fake_pid=$!

event_path=""
for _ in $(seq 1 50); do
    if [ -s "$tmpdir/fake.out" ]; then
        event_path="$(head -n 1 "$tmpdir/fake.out")"
        break
    fi
    sleep 0.1
done

if [ -z "$event_path" ] || [ ! -e "$event_path" ]; then
    echo "No se pudo crear el mando falso." >&2
    cat "$tmpdir/fake.out" >&2
    exit 1
fi
echo "Mando falso: $event_path"

echo "Arrancando ds4xbox..."
"$REPO_DIR/ds4xbox" --device "$event_path" --force --verbose \
    > "$tmpdir/daemon.out" 2>&1 &
daemon_pid=$!

virtual_event=""
for _ in $(seq 1 50); do
    for f in /sys/class/input/event*/device/phys; do
        [ -e "$f" ] || continue
        if [ "$(cat "$f" 2>/dev/null)" = "ds4xbox/virtual" ]; then
            virtual_event="/dev/input/$(basename "$(dirname "$(dirname "$f")")")"
            break 2
        fi
    done
    sleep 0.1
done

if [ -z "$virtual_event" ] || [ ! -e "$virtual_event" ]; then
    echo "No apareció el mando Xbox virtual." >&2
    cat "$tmpdir/daemon.out" >&2
    exit 1
fi
echo "Mando virtual: $virtual_event"

echo "Leyendo eventos del mando virtual durante 5 segundos..."
"$REPO_DIR/tests/read_pad" "$virtual_event" 5 > "$tmpdir/read.out" 2>&1 &
reader_pid=$!
wait "$reader_pid"
reader_pid=""

echo
echo "--- eventos del mando virtual ---"
cat "$tmpdir/read.out"

echo
echo "--- registro del daemon ---"
cat "$tmpdir/daemon.out"

fail=0
for expected in "BTN_SOUTH (A)" "BTN_NORTH (X)" "ABS_X" "ABS_Z" "ABS_HAT0Y"; do
    if ! grep -qF "$expected" "$tmpdir/read.out"; then
        echo "FALLO: no se vio '$expected' en el mando virtual." >&2
        fail=1
    fi
done

if ! grep -q "BTN_SOUTH (A).*value=1" "$tmpdir/read.out"; then
    echo "FALLO: no se vio pulsar A." >&2
    fail=1
fi
if ! grep -q "BTN_NORTH (X).*value=1" "$tmpdir/read.out"; then
    echo "FALLO: no se vio pulsar X (Square)." >&2
    fail=1
fi
if ! grep -q "ABS_Z.*value=255" "$tmpdir/read.out"; then
    echo "FALLO: no se vio el gatillo L2 a 255." >&2
    fail=1
fi
if ! grep -q "ABS_HAT0Y.*value=-1" "$tmpdir/read.out"; then
    echo "FALLO: no se vio el dpad arriba." >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo "Test FALLIDO." >&2
    exit 1
fi

echo
echo "Test OK: el DualShock 4 falso se ha convertido en un mando Xbox 360."
