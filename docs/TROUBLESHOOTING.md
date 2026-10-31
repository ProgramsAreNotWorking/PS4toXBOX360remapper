# Solución de problemas

Esta guía asume que el proyecto está en `~/PS4toXBOX360remapper`; ajusta las
rutas si lo tienes en otro sitio.

## 1. Comprobaciones básicas

```bash
uname -r
id
ls -l /dev/uinput
lsmod | grep uinput
systemctl status ds4xbox.service --no-pager -l
journalctl -u ds4xbox.service -n 100 --no-pager
```

El daemon debe ejecutarse como **root**. Si lo lanzas como usuario normal,
verás errores de permisos al abrir `/dev/input/event*` y `/dev/uinput`.

Si el servicio no existe:

```bash
sudo ./scripts/install.sh
```

Si el servicio existe pero falla:

```bash
sudo systemctl restart ds4xbox.service
journalctl -u ds4xbox.service -f
```

Para probar en primer plano con más detalle:

```bash
sudo systemctl stop ds4xbox.service
sudo ./ds4xbox --verbose
```

No ejecutes dos instancias a la vez: el lock de `/run/ds4xbox/lock` hará que
la segunda salga con un error.

## 2. `/dev/uinput` no existe o no se puede abrir

Comprueba el módulo:

```bash
sudo modprobe uinput
ls -l /dev/uinput
```

Si no aparece:

```bash
dmesg | tail -30
```

En NixOS, añade:

```nix
boot.kernelModules = [ "uinput" ];
```

El módulo `uinput` puede estar compilado dentro del kernel o como módulo. Si
`/dev/uinput` tiene permisos `0600 root:root`, el daemon como root puede
abrirlo; un usuario normal no.

Si el daemon dice `UI_DEV_SETUP failed`, comprueba la versión del kernel. La
API `UI_DEV_SETUP`/`UI_ABS_SETUP` necesita Linux 4.5 o superior.

## 3. El daemon no captura el mando (`EBUSY`)

El error típico es:

```
cannot grab /dev/input/eventX: Device or resource busy
```

Significa que otro programa tiene el mando abierto o capturado. Cierra:

- Steam (y espera a que termine `steamwebhelper`).
- `ds4drv`.
- `antimicro`, `antimicrox`.
- `sc-controller`.
- Cualquier otro daemon de mandos.

Después:

```bash
sudo systemctl restart ds4xbox.service
```

Para identificar qué proceso tiene el dispositivo:

```bash
sudo fuser -v /dev/input/eventX
sudo lsof /dev/input/eventX
```

`--no-grab` permite que el daemon lea sin capturar, pero entonces no oculta el
mando original y no sirve para el objetivo de este proyecto. Úsalo solo para
depurar.

## 4. El mando original sigue visible en Steam/SDL

Comprueba que la regla está instalada y en el orden correcto:

```bash
ls -l /etc/udev/rules.d/72-ds4xbox.rules
```

Debe existir y llamarse `72-ds4xbox.rules`. La regla tiene que ejecutarse
después de `70-uaccess.rules` y antes de `73-seat-late.rules`. Si tu
distribución usa otra numeración, renómbrala para que quede entre esas dos.

Recarga udev y reconecta el mando:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input --action=change
sudo udevadm trigger --subsystem-match=hidraw --action=change
sudo udevadm trigger --subsystem-match=usb --action=change
```

Desconecta y vuelve a conectar el mando (o reinicia el Bluetooth). Cierra y
vuelve a abrir Steam: Steam cachea la lista de mandos al arrancar.

Comprueba las propiedades del nodo:

```bash
udevadm info --query=all --name=/dev/input/eventX
udevadm info --attribute-walk --name=/dev/input/eventX | \
  grep -E 'ID_INPUT|LIBINPUT|name|phys|ID_SEAT'
```

Lo esperado para el mando Sony:

- `ID_INPUT` vacío o ausente.
- `ID_INPUT_JOYSTICK` vacío o ausente.
- `LIBINPUT_IGNORE_DEVICE=1`.
- Sin `uaccess` (la etiqueta `seat` puede seguir presente; no concede acceso
  por sí sola).
- `MODE=0600`, `OWNER=root`, `GROUP=root`.

Si `ID_INPUT_JOYSTICK=1` sigue apareciendo, la regla no se está aplicando.
Prueba a ejecutar udev en modo test:

```bash
sudo udevadm test /sys/class/input/eventX
```

Revisa que `72-ds4xbox.rules` aparezca en la salida y que no haya errores de
sintaxis. Una regla udev con una línea mal continuada se ignora total o
parcialmente.

También puedes comprobar el nodo `hidraw`:

```bash
ls -l /dev/hidraw*
udevadm info --query=all --name=/dev/hidrawX
```

El nodo `hidraw` del mando Sony debe quedar en `0600 root:root` y sin
`uaccess`. Si Steam sigue viendo el mando, puede ser por `hidraw` o por el
nodo USB; revisa las tres reglas (input, hidraw, usb).

### El mando original sigue oculto después de parar el servicio

La ocultación es temporal y depende de `/run/ds4xbox/active`:

```bash
sudo systemctl stop ds4xbox.service
ls -l /run/ds4xbox/active 2>/dev/null || echo "flag ausente: correcto"
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input --action=change
sudo udevadm trigger --subsystem-match=hidraw --action=change
sudo udevadm trigger --subsystem-match=usb --action=change
```

Si el flag sigue existiendo, bórralo:

```bash
sudo rm -f /run/ds4xbox/active
```

Después desconecta y vuelve a conectar el mando (o reinicia el Bluetooth).
La unidad systemd también ejecuta esa limpieza en `ExecStopPost`; si el
servicio no la ejecutó, revisa:

```bash
systemctl cat ds4xbox.service | grep -A2 ExecStopPost
journalctl -u ds4xbox.service -n 50 --no-pager
```

## 5. El mando virtual no aparece

Comprueba el log del daemon:

```bash
journalctl -u ds4xbox.service -n 100 --no-pager
```

Busca líneas como:

```
controller 1: ... -> virtual Microsoft X-Box 360 pad
controller 1: grabbed /dev/input/eventX (gamepad)
```

Si no aparece `UI_DEV_CREATE` o hay un error de `uinput`, revisa la sección 2.

Comprueba los nodos:

```bash
ls -l /dev/input/
ls -l /dev/input/by-id/ | grep -i x-box
cat /proc/bus/input/devices | grep -A 8 -i 'X-Box'
```

El mando virtual debe tener:

- Nombre `Microsoft X-Box 360 pad` o `Microsoft X-Box One pad`.
- `phys=ds4xbox/virtual`.
- Vendor `045e`, product `028e` o `02d1`.

Si el nodo existe pero el usuario no puede abrirlo, comprueba la regla de los
mandos virtuales:

```bash
udevadm info --query=all --name=/dev/input/eventY | \
  grep -E 'MODE|GROUP|TAG'
```

Debe tener `MODE=0660`, `GROUP=input` y `TAG=uaccess` (o una ACL para tu
usuario). Si no, recarga udev.

## 6. El mando virtual aparece pero no llegan eventos

1. Comprueba que el daemon capturó el mando real:

   ```bash
   sudo ./ds4xbox --verbose
   ```

   Debe decir `grabbed /dev/input/eventX (gamepad)`.

2. Comprueba el mando real con `evtest` **antes** de arrancar el daemon:

   ```bash
   sudo systemctl stop ds4xbox.service
   sudo evtest /dev/input/eventX
   ```

   Pulsa botones y mueve sticks. Si no hay eventos, el problema es del kernel,
   del cable, del Bluetooth o del mando, no del daemon.

3. Arranca el daemon y comprueba el mando virtual:

   ```bash
   sudo ./ds4xbox --verbose
   # en otra terminal:
   sudo evtest /dev/input/eventY
   ```

   Si el mando real funciona pero el virtual no, revisa el log del daemon y
   prueba `--no-ff` para descartar un problema de force feedback.

4. Si el mando real tiene un layout raro, comprueba los ejes:

   ```bash
   sudo evtest /dev/input/eventX
   ```

   Mira los códigos `ABS_X`, `ABS_Y`, `ABS_RX`, `ABS_RY`, `ABS_Z`, `ABS_RZ`.
   Si el stick derecho está en `ABS_Z/RZ` y los gatillos en `ABS_RX/RY`, usa
   `--swap-sticks-triggers`.

### Tirones, congelaciones o huecos al mover el stick

La versión actual usa una cola de salida no bloqueante y `EPOLLOUT`; si el
programa que lee el mando virtual (juego, Steam, navegador) se atasca, el
daemon ya no descarta el informe completo. Comprueba el log:

```bash
journalctl -u ds4xbox.service -f
```

- `uinput busy; queueing input` es normal y significa que se está usando la
  cola.
- `uinput queue overflow; dropped N report(s)` indica que el consumidor va
  demasiado lento incluso para 128 informes; suele ser un problema del juego,
  de Steam Input o de un programa que abre el mando virtual y no lee.
- `uinput write failed` o `EPOLLOUT` repetido apunta a un fallo del kernel o a
  que el dispositivo virtual se cerró.

Para descartar el consumidor, prueba con `evtest` o `tests/read_pad` y mueve
el stick en círculos:

```bash
sudo evtest /dev/input/eventY
```

Si con `evtest` el recorrido es continuo, el problema está en el juego o en
Steam, no en la conversión. Si aparece un hueco también con `evtest`, envía el
log completo con `--verbose` y el modelo/kernel.

## 7. Los botones están cambiados

El mapeo es posicional, no por etiqueta:

- Cruz (abajo) → A.
- Círculo (derecha) → B.
- Cuadrado (izquierda) → X.
- Triángulo (arriba) → Y.

Si un juego muestra otra cosa, puede estar aplicando su propia capa de
remapping (Steam Input, por ejemplo). Comprueba con `evtest` que el mando
virtual emite `BTN_SOUTH` al pulsar Cruz y `BTN_NORTH` al pulsar Cuadrado.

En algunos clones los botones vienen con otra disposición; el daemon asume el
layout estándar de DS4. Si tu clon es diferente, revisa `map_button_code()` o
abre una incidencia con la salida de `evtest`.

## 8. La vibración no funciona

- Comprueba que el juego soporta force feedback. Muchos juegos de PC solo
  vibran con mandos Xbox y usan la API FF estándar.
- No uses `--no-ff`.
- Busca en el log del daemon:

  ```text
  controller 1: force feedback enabled (16 effect slots)
  ```

  Si aparece `force feedback not supported by this device`, el driver/kernel
  no expone FF_RUMBLE; revisa el kernel o prueba por USB.

- Para una prueba rápida sin juego, arranca el daemon con `--test-rumble`:

  ```bash
  sudo systemctl stop ds4xbox.service
  sudo ./ds4xbox --verbose --test-rumble
  ```

  Debe vibrar una vez al detectar el mando y mostrar `rumble test playing`.

- No uses un probador de mando del navegador como referencia fiable. Muchos
  navegadores muestrean el mando a baja velocidad o con su propia capa de
  compatibilidad, y pueden mostrar huecos o saltos que no existen en el
  dispositivo. Para confirmar la entrada, usa `evtest` o `jstest` sobre el
  mando virtual Xbox.

- Comprueba que el mando real anuncia FF:

  ```bash
  sudo evtest /dev/input/eventX
  ```

  `evtest` muestra los efectos FF disponibles. Si el mando no anuncia
  `FF_RUMBLE`, el daemon no puede reenviar rumble.

- El DualSense tiene haptics avanzados que no son FF clásico; el daemon solo
  reenvía los efectos FF estándar.
- Si el mando está por Bluetooth, algunos modelos/kernels tienen problemas
  con FF. Prueba por USB para aislar el problema.

## 9. Bluetooth

Comprueba el estado:

```bash
bluetoothctl show
bluetoothctl devices
bluetoothctl info AA:BB:CC:DD:EE:FF
rfkill list bluetooth
dmesg | tail -50
```

El mando debe estar `Paired: yes` y `Trusted: yes`. Si no:

```bash
bluetoothctl trust AA:BB:CC:DD:EE:FF
bluetoothctl connect AA:BB:CC:DD:EE:FF
```

Para volver a emparejar:

```bash
bluetoothctl remove AA:BB:CC:DD:EE:FF
```

y repite el modo emparejamiento (Share + PS en DS4, Create + PS en DualSense).

Si el mando se conecta pero no aparecen nodos evdev:

```bash
dmesg | grep -i -E 'playstation|sony|dualshock|dualsense|hid'
```

Comprueba que el kernel tiene `hid-playstation` (recomendado, 5.12+) o
`hid-sony`. En kernels antiguos, el soporte de DualSense puede ser limitado.

Si el mando se desconecta solo, revisa la batería y la configuración de
ahorro de energía del adaptador Bluetooth. Prueba por USB para descartar el
daemon.

## 10. Steam y Proton

- Cierra Steam antes de instalar o recargar las reglas udev.
- Vuelve a abrir Steam después.
- Si Steam sigue viendo el mando Sony, revisa las secciones 4 y 5.
- Si Steam ve el mando virtual pero aplica su propia configuración, desactiva
  **Steam Input** para ese mando o usa el modo Xbox.
- En Proton, el mando virtual es un dispositivo evdev estándar; la mayoría de
  juegos lo ven como Xbox 360.
- Si un juego usa `SDL_GAMECONTROLLERCONFIG` o una configuración antigua,
  prueba a borrar la configuración de mandos de Steam para ese juego.
- Algunos juegos con anti-cheat pueden rechazar dispositivos virtuales. No hay
  una solución general desde espacio de usuario.

## 11. NixOS

El módulo hace lo necesario:

```nix
services.ds4xbox.enable = true;
```

Comprueba tras `nixos-rebuild switch`:

```bash
systemctl status ds4xbox.service
ls -l /run/current-system/sw/bin/ds4xbox
ls -l /etc/udev/rules.d/ | grep ds4xbox
```

NixOS enlaza las reglas del paquete en `/etc/udev/rules.d/` (a través de
`services.udev.packages`), no las copia. Si no aparece, comprueba que el
paquete está en `services.udev.packages` (el módulo lo hace) y que has
reconstruido el sistema.

Para probar el paquete sin el módulo:

```bash
nix build path:.
sudo ./result/bin/ds4xbox --verbose
```

El binario del store no tiene las reglas udev instaladas; para ocultar el
mando original, instala la regla o usa el módulo.

## 12. Clones y mandos raros

- Si el mando no es Sony y quieres gestionarlo con `--device`:

  ```bash
  sudo ds4xbox --device /dev/input/eventX --force --verbose
  ```

  `--force` permite el mapeo DS4 aunque el vendor no sea Sony. Asegúrate de
  que el dispositivo realmente tiene el layout de DS4.

- Si los sticks/gatillos están cruzados:

  ```bash
  sudo ds4xbox --swap-sticks-triggers --verbose
  ```

- Si el clon no tiene ejes de gatillo y solo botones `BTN_TL2`/`BTN_TR2`, el
  daemon sintetiza los ejes. Si tiene ejes pero no los anuncia
  correctamente, revisa `evtest`.

- Si el clon usa un nombre genérico (`Wireless Controller`), las reglas udev
  no lo ocultarán por nombre. Puedes añadir una regla por VID/PID:

  ```udev
  SUBSYSTEM=="input", KERNELS=="*0003:XXXX:YYYY*", \
      ENV{LIBINPUT_IGNORE_DEVICE}="1", \
      ENV{ID_INPUT}="", ENV{ID_INPUT_JOYSTICK}="", \
      TAG-="uaccess", \
      OWNER="root", GROUP="root", MODE="0600"
  ```

  Sustituye `XXXX:YYYY` por el VID/PID que muestra `udevadm info`. Ten en
  cuenta que `EVIOCGRAB` ya impide que otros programas lean los eventos,
  aunque el nodo siga siendo visible.

## 13. Varios mandos

El daemon soporta hasta 8 mandos. Cada uno se agrupa por `phys`. Si dos
mandos del mismo modelo comparten `phys` (algunos clones), pueden agruparse
mal. Comprueba:

```bash
ds4xbox --list
```

Cada mando debe tener un `group=` distinto. Si no, usa `--device` para
gestionar uno concreto o revisa el `phys` con `udevadm info`.

## 14. El servicio no arranca tras actualizar

```bash
sudo systemctl daemon-reload
sudo systemctl restart ds4xbox.service
journalctl -u ds4xbox.service -n 100 --no-pager
```

Si cambiaste la ruta del binario, reinstala con `scripts/install.sh` o ajusta
`ExecStart` en `/etc/systemd/system/ds4xbox.service`.

## 15. Desinstalar y volver al estado original

```bash
sudo ./scripts/uninstall.sh
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Desconecta y vuelve a conectar el mando. Los nodos deberían recuperar sus
permisos y etiquetas normales. Si no, reinicia udev o el sistema.

## 16. Recoger información para una incidencia

Incluye:

```bash
uname -a
cat /etc/os-release
lsusb
cat /proc/bus/input/devices
udevadm info --attribute-walk --name=/dev/input/eventX
sudo evtest /dev/input/eventX
journalctl -u ds4xbox.service -n 200 --no-pager
sudo ./ds4xbox --verbose
```

y describe:

- Modelo del mando (DS4 v1/v2, DualSense, clon).
- Conexión (USB/Bluetooth).
- Kernel y distribución.
- Si Steam estaba abierto.
- Qué botón/eje falla y qué se esperaba.
