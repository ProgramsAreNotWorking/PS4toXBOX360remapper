# Compatibilidad

## Estado de las pruebas

La versión 1.0.0 compila sin avisos y pasa:

- `ds4xbox --self-test` (mapeo de sticks, gatillos, hat, botones y
  `--swap-sticks-triggers`).
- `tests/integration.sh` con un DualShock 4 falso creado con `uinput`
  (pendiente de ejecutar en esta instalación; requiere root).

Se ha probado por USB con un DualShock 4; la prueba con Bluetooth, con
DualSense y con otros modelos sigue pendiente. La tabla siguiente indica el
soporte previsto por el código y el layout que espera, no una garantía de
haberlo probado con cada modelo.

La prueba de vibración se puede hacer con la casilla del probador de mandos o
con `sudo ./ds4xbox --verbose --test-rumble`.

## Mandos

| Modelo | VID:PID | USB | Bluetooth | Notas |
|--------|---------|-----|-----------|-------|
| DualShock 4 v1 (CUH-ZCT1) | `054c:05c4` | Sí | Sí | Layout estándar DS4. |
| DualShock 4 v2 (CUH-ZCT2) | `054c:09cc` | Sí | Sí | Layout estándar DS4. |
| DualShock 4 USB Wireless Adapter | `054c:0ba0` | Sí | — | El adaptador expone un DS4; el daemon lo trata como Sony. |
| DualSense | `054c:0ce6` | Sí | Sí | Se mapea como DS4; no se reenvían gatillos adaptativos, haptics avanzados, luz ni audio. |
| DualSense Edge | `054c:0df2` | Sí | Sí | Igual que DualSense; los botones extra no se reenvían. |
| Clones con layout DS4 | variable | Sí | variable | Pueden necesitar `--device --force`, `--swap-sticks-triggers` o reglas udev por VID/PID. |
| Mandos Xbox/PS3/Switch | — | No | No | No son el objetivo. |

## Perfiles virtuales

| Perfil | VID:PID | Nombre | Cuándo usarlo |
|--------|---------|--------|---------------|
| `xbox360` | `045e:028e` | `Microsoft X-Box 360 pad` | Por defecto; máxima compatibilidad con SDL/Steam/Proton. |
| `xboxone` | `045e:02d1` | `Microsoft X-Box One pad` | Juegos que prefieren el perfil Xbox One. |

Ambos perfiles usan el layout estándar de Xbox: sticks firmados, gatillos
`0..255`, hat `-1..1` y botones `BTN_SOUTH/EAST/NORTH/WEST`.

## Sistemas y kernels

| Requisito | Detalle |
|-----------|---------|
| Linux | Cualquier distribución con evdev, uinput y udev. |
| Kernel | 4.5+ para `UI_DEV_SETUP`/`UI_ABS_SETUP`; recomendado 6.x. |
| Controlador del kernel | `hid-playstation` (5.12+) o `hid-sony` (anterior). |
| systemd | Opcional; el daemon funciona sin systemd. |
| udev | Necesario para ocultar los nodos originales y dar acceso al mando virtual. |
| BlueZ | Necesario para Bluetooth. |
| NixOS | Módulo y paquete incluidos; recomendado. |

Distribuciones probadas o previstas:

- NixOS 26.11 (entorno de desarrollo de esta versión).
- Arch Linux, Fedora, Ubuntu, Debian: el daemon y las reglas udev son
  estándar; no se han probado en esta instalación.
- Otras distribuciones con systemd/udev: deberían funcionar.

En distribuciones sin systemd, instala el binario y la regla udev a mano y
ejecuta el daemon como root. Consulta el README.

## Software

| Software | Estado | Notas |
|----------|--------|-------|
| SDL2 / SDL3 | Sí | El mando virtual es un evdev estándar con VID/PID de Xbox. |
| Steam | Sí, con matices | Cierra Steam al instalar las reglas. Puede aplicar Steam Input; desactívalo si es necesario. |
| Proton | Sí | La mayoría de juegos ven el mando virtual como Xbox 360. |
| Godot 4 | Sí | Detecta el mando virtual a través de SDL/evdev. |
| Unity | Sí | A través de su capa de input del sistema. |
| Juegos con anti-cheat | Variable | Algunos rechazan dispositivos `uinput`; no hay garantía. |
| `evtest` | Sí | Útil para comprobar el mando real y el virtual. |
| `jstest` | Sí | Comprueba el nodo `js` del mando virtual. |
| `sdl2-jstest` | Sí | Comprueba la detección vía SDL. |

## Funciones soportadas

| Función | Soporte |
|---------|---------|
| Sticks analógicos | Sí |
| Gatillos analógicos | Sí |
| Botones DS4 | Sí |
| D-pad (hat o botones) | Sí |
| Clic del touchpad → Back | Sí |
| Movimiento del touchpad | No |
| Giroscopio / acelerómetro | No |
| Vibración FF clásica | Sí |
| Liberar el mando original al parar | Sí; flag `/run/ds4xbox/active` + `udevadm trigger` |
| Gatillos adaptativos DualSense | No |
| Haptics avanzados DualSense | No |
| Luz / altavoz / jack | No |
| Perfiles por juego | No |
| Varios mandos | Sí (hasta 8) |
| Hotplug USB/Bluetooth | Sí |
| Reconexión automática | Sí, tras emparejar/confiar una vez |
| Cambio de perfil en caliente | No; reinicia el servicio |

## Layout esperado del DS4

El daemon asume el layout evdev estándar de `hid-playstation`/`hid-sony`:

| Elemento | Código evdev | Rango |
|----------|--------------|-------|
| Stick izquierdo X/Y | `ABS_X`, `ABS_Y` | `0..255`, centro `128` |
| Stick derecho X/Y | `ABS_RX`, `ABS_RY` | `0..255`, centro `128` |
| Gatillo L2 | `ABS_Z` | `0..255` |
| Gatillo R2 | `ABS_RZ` | `0..255` |
| D-pad | `ABS_HAT0X`, `ABS_HAT0Y` | `-1..1` |
| Cruz/Círculo/Cuadrado/Triángulo | `BTN_SOUTH`, `BTN_EAST`, `BTN_WEST`, `BTN_NORTH` | `0/1` |
| L1/R1 | `BTN_TL`, `BTN_TR` | `0/1` |
| L2/R2 digitales | `BTN_TL2`, `BTN_TR2` | `0/1` |
| Share/Options/PS | `BTN_SELECT`, `BTN_START`, `BTN_MODE` | `0/1` |
| L3/R3 | `BTN_THUMBL`, `BTN_THUMBR` | `0/1` |
| Clic del touchpad | `BTN_LEFT` | `0/1` |

Algunos clones invierten los ejes o usan rangos firmados. `map_stick_value()`
y `map_trigger_value()` normalizan rangos conocidos; si tu clon usa otro
layout, revisa `evtest` y ajusta el código o usa las opciones disponibles.

## Limitaciones conocidas

1. **El dispositivo Sony sigue en `lsusb`.** No se pueden cambiar los
   descriptores USB desde espacio de usuario. Se ocultan los nodos y se
   capturan los eventos, pero el hardware es visible.
2. **Root puede ver el dispositivo.** `EVIOCGRAB` impide recibir eventos a
   otros descriptores, pero no impide que un proceso root vea que existe.
3. **Anti-cheat.** Los dispositivos `uinput` pueden ser detectados como
   virtuales. No hay una solución general.
4. **DualSense.** No se reenvían gatillos adaptativos, haptics avanzados,
   micrófono, altavoz, luz ni jack. El mapeo básico de botones/sticks/gatillos
   funciona como un DS4.
5. **Touchpad.** Solo se reenvía el clic como Back; el movimiento no se
   convierte en ratón.
6. **Sensores.** El daemon no abre el giroscopio ni el acelerómetro porque no
   se reenvían; udev oculta esos nodos para usuarios normales mientras está
   activo.
7. **Steam Input.** Steam puede aplicar su propia capa de remapping. Si un
   juego ve botones cambiados, revisa la configuración de Steam Input.
8. **Bluetooth.** La latencia, la vibración y la reconexión dependen del
   adaptador, del kernel y del firmware del mando. Prueba por USB para
   aislar problemas.
9. **Clones.** Los mandos con nombres genéricos no se ocultan por nombre; usa
   una regla udev por VID/PID. Los layouts raros pueden requerir
   `--swap-sticks-triggers` o cambios en el mapeo.
10. **Varios mandos del mismo modelo.** Si comparten `phys`, pueden agruparse
    mal. Usa `--device` para gestionar uno concreto.

## Cómo comprobar tu modelo

```bash
lsusb
cat /proc/bus/input/devices
sudo evtest
```

En `evtest`, selecciona el mando Sony y comprueba:

- Nombre y VID/PID.
- Ejes `ABS_X/Y/RX/RY/Z/RZ` y hat.
- Botones `BTN_SOUTH/EAST/NORTH/WEST`, `BTN_TL/TR`, `BTN_TL2/TR2`,
  `BTN_SELECT/START/MODE`, `BTN_THUMBL/R`.
- Efectos FF disponibles.

Si el layout coincide, el daemon debería funcionar. Si no, envía la salida de
`evtest` y `udevadm info --attribute-walk` para poder añadir soporte.
