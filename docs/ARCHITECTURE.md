# Arquitectura de ds4xbox

Este documento explica cómo está construido el daemon, por qué se tomaron
ciertas decisiones y cuáles son sus límites.

## Objetivos

1. Leer un DualShock 4 o DualSense por USB o Bluetooth.
2. Exponer un mando Xbox 360/One virtual que funcione sin configuración por
   juego.
3. Impedir que otros programas lean el mando original y, en la medida de lo
   posible, que lo detecten como mando PlayStation.
4. Soportar hotplug, reconexión, varios mandos y force feedback.
5. No tener dependencias externas: solo C y la API evdev/uinput del kernel.

## No objetivos

- Emular un mando de PS4 para la consola.
- Reenviar touchpad como ratón, giroscopio, acelerómetro, luz, altavoz o jack.
- Cambiar los descriptores USB del dispositivo Sony.
- Funcionar en Windows/macOS.
- Garantizar compatibilidad con anti-cheat que rechacen dispositivos virtuales.

## Componentes

| Archivo | Responsabilidad |
|---------|-----------------|
| `src/main.c` | CLI, perfiles, ayuda, `--list`, `--self-test`. |
| `src/daemon.c` | Enumeración de dispositivos, agrupación, captura, uinput, bucle epoll, hotplug, FF, limpieza. |
| `src/mapping.c` | Funciones puras de mapeo y pruebas internas. |
| `src/log.c` | Registro con timestamp y nivel. |
| `src/common.h` | Tipos, constantes, perfiles y declaraciones compartidas. |
| `udev/72-ds4xbox.rules` | Oculta los nodos Sony y da acceso al mando virtual. |
| `systemd/ds4xbox.service.in` | Servicio de producción como root. |
| `scripts/` | Instalación, desinstalación, emparejamiento y diagnóstico. |
| `nix/` | Paquete y módulo NixOS. |
| `tests/` | DS4 falso, lector evdev y test de integración. |

## Flujo de eventos

```
                     +----------------------+
USB / Bluetooth ---> | kernel hid-playstation|
                     | o hid-sony           |
                     +----------+-----------+
                                |
                                | crea nodos input
                                v
+----------------------+   +----------------------+
| eventN (gamepad)     |   | eventM (touchpad)    |
| eventP (motion)      |   | eventQ (jack)        |
+----------+-----------+   +----------+-----------+
           |                          |
           |        EVIOCGRAB         |
           +------------+-------------+
                        |
                        v
              +---------+----------+
              | ds4xbox (root)     |
              | epoll single-thread|
              +---------+----------+
                        |
                        | EV_KEY / EV_ABS / EV_SYN
                        v
              +---------+----------+
              | /dev/uinput        |
              | Xbox 360/One       |
              +---------+----------+
                        |
                        v
              +---------+----------+
              | eventR (virtual)   |
              | udev: uaccess      |
              +--------------------+
```

El daemon es **single-threaded**. Usa `epoll` para esperar eventos de:

- Los descriptores de los nodos evdev capturados.
- El descriptor de `inotify` que avisa de cambios en `/dev/input`.
- El descriptor de `uinput`, que recibe las peticiones de force feedback
  (`EV_UINPUT`: `UI_FF_UPLOAD` / `UI_FF_ERASE`) y los eventos `EV_FF` de
  reproducción.

Un temporizador de rescaneo de 1 segundo complementa a `inotify` para
detectar dispositivos que aparecen o desaparecen sin evento.

La escritura al `uinput` es **no bloqueante**. Cada `SYN_REPORT` se encola
como una unidad (`struct output_frame`) en una cola de 128 informes; si el
kernel no acepta la escritura completa, se conserva el resto y se activa
`EPOLLOUT` para reintentarla. Dentro de cada informe se agrupan las lecturas
repetidas de un mismo eje (`EV_ABS`), quedándose con el último valor. Esto
evita el error de la versión anterior, que descartaba el informe completo
cuando `write()` devolvía `EAGAIN`, y explica los huecos al dibujar círculos
con el stick. Si la cola se llena porque el consumidor del mando virtual va
demasiado lento, se descarta el informe más antiguo, se avisa con
`uinput queue overflow` y se reenvía un estado completo (`resync`) al vaciarse.

## Descubrimiento y agrupación

`enumerate_devices()` recorre `/dev/input/event*`, abre cada nodo y consulta:

- `EVIOCGNAME`: nombre del dispositivo.
- `EVIOCGID`: bus, vendor, product, versión.
- `EVIOCGPHYS`: ruta física.
- `EVIOCGUNIQ`: identificador único (si existe).
- `EVIOCGBIT`: capacidades de teclas, ejes y FF.

El mando PlayStation se identifica por vendor `054c` o por nombre que contenga
`DualShock`, `DualSense`, `PlayStation`, `PS4`, `Sony Interactive
Entertainment` o `Sony Computer Entertainment`.

Los nodos de un mismo mando se agrupan por `EVIOCGPHYS`, normalizando el
sufijo final `/inputN`. El kernel suele exponer el gamepad, el touchpad, los
sensores de movimiento y el jack como nodos distintos del mismo dispositivo
HID, con `phys` del estilo `usb-0000:04:00.0-3/input0`, `.../input1`, etc.
Al quitar ese sufijo, todos los nodos del mismo mando comparten grupo. Si el
dispositivo no expone `phys`, se usa `EVIOCGUNIQ`; si tampoco, la ruta del nodo.

El **primario** es el nodo con capacidades de gamepad (botones y ejes de
sticks/gatillos). Los demás nodos del grupo se añaden como secundarios:

- El touchpad se detecta por nombre (`Touchpad`, `Touch Pad`, etc.) o por
  `BTN_LEFT` + `ABS_MT_POSITION_X`. Solo se reenvía su clic como Back.
- Los sensores de movimiento y el jack se capturan para que no queden
  accesibles, pero sus eventos no se traducen.

Cada miembro se captura con `EVIOCGRAB` antes de crear el mando virtual. Si la
captura falla con `EBUSY`, otro programa tiene el mando; el daemon registra el
error y no lo gestiona.

## uinput: el mando virtual

`uinput_create_device()` abre `/dev/uinput` y configura:

- `EV_KEY`, `EV_ABS`, `EV_SYN` y, si hay FF, `EV_FF`.
- Botones Xbox: `BTN_SOUTH`, `BTN_EAST`, `BTN_NORTH`, `BTN_WEST`, `BTN_TL`,
  `BTN_TR`, `BTN_SELECT`, `BTN_START`, `BTN_MODE`, `BTN_THUMBL`, `BTN_THUMBR`.
- Ejes: sticks `ABS_X/Y/RX/RY` en `-32768..32767`; gatillos `ABS_Z/RZ` en
  `0..255`; hat `ABS_HAT0X/Y` en `-1..1`.
- Identidad del perfil: `045e:028e` (Xbox 360) o `045e:02d1` (Xbox One).
- `UI_SET_PHYS` con `ds4xbox/virtual` para que el test de integración y los
  scripts puedan localizar el mando virtual sin confundirlo con un mando Xbox
  real.
- Bits FF copiados del mando original, con `ff_effects_max` igual al número de
  efectos que soporta el dispositivo real.

La regla udev reconoce el nombre exacto del perfil y aplica `MODE=0660`,
`GROUP=input` y `TAG+=uaccess`, de modo que el usuario de la sesión puede
abrir el mando virtual aunque el daemon corra como root.

## Mapeo

`mapping.c` contiene funciones puras, sin estado ni acceso al kernel:

- `map_stick_value()`: normaliza un eje de stick DS4 (`0..255`, centro `128`,
  o rangos firmados) al rango `-32768..32767`. Aplica zona muerta y saturación
  de forma conservadora.
- `map_trigger_value()`: normaliza un gatillo a `0..255`.
- `map_hat_value()`: normaliza el hat a `-1`, `0` o `1`.
- `map_button_code()`: traduce los botones DS4 a botones Xbox. El mapeo es
  posicional:
  - `BTN_SOUTH` (Cruz) → `BTN_SOUTH` (A)
  - `BTN_EAST` (Círculo) → `BTN_EAST` (B)
  - `BTN_WEST` (Cuadrado) → `BTN_NORTH` (X)
  - `BTN_NORTH` (Triángulo) → `BTN_WEST` (Y)
  - `BTN_TL/TR` → `BTN_TL/TR` (LB/RB)
  - `BTN_SELECT` (Share) → `BTN_SELECT` (Back)
  - `BTN_START` (Options) → `BTN_START` (Start)
  - `BTN_MODE` (PS) → `BTN_MODE` (Guide)
  - `BTN_THUMBL/R` → `BTN_THUMBL/R` (L3/R3)
- `map_axis_code()` / `map_axis_kind()`: traducen ejes y permiten intercambiar
  stick derecho y gatillos con `--swap-sticks-triggers`.

Los gatillos digitales `BTN_TL2`/`BTN_TR2` se ignoran si el mando expone
`ABS_Z`/`ABS_RZ`; si no los expone, el daemon sintetiza los ejes a partir de
esos botones. Esto cubre clones que solo tienen botones de gatillo.

El D-pad puede venir como hat (`ABS_HAT0X/Y`) o como botones
(`BTN_DPAD_UP/DOWN/LEFT/RIGHT`). El daemon soporta ambos y convierte los
botones a hat para el mando virtual.

## Force feedback

El flujo de rumble es:

```
Juego / SDL
    |
    | EVIOCSFF / write(EV_FF) al mando virtual
    v
uinput (kernel)
    |
    | EV_UINPUT: UI_FF_UPLOAD / UI_FF_ERASE
    v
ds4xbox
    |
    | EVIOCSFF / EVIOCRMFF en el nodo primario real
    v
Mando DS4/DualSense
```

- `controller_handle_uinput_readable()` lee los eventos `EV_UINPUT`.
- En `UI_FF_UPLOAD`, el daemon pide el efecto con `UI_BEGIN_FF_UPLOAD`, lo
  sube al mando real con `EVIOCSFF`, guarda la correspondencia entre el
  `effect_id` virtual y el real, y responde con `UI_END_FF_UPLOAD`.
- En `UI_FF_ERASE`, borra el efecto real con `EVIOCRMFF` y responde con
  `UI_END_FF_ERASE`.
- Los eventos `EV_FF` de reproducción, ganancia y autocenter se reenvían
  escribiendo en el descriptor del mando real.
- Si el mando real desaparece, el daemon marca el control como muerto y
  conserva las correspondencias que no pudo borrar para reintentar en la
  limpieza.
- Si `EVIOCGEFFECTS` no está disponible pero el mando anuncia `FF_RUMBLE`, se
  asumen 16 ranuras de efectos en lugar de desactivar la vibración.
- `--test-rumble` sube y reproduce un efecto corto en el mando real al
  detectarlo; sirve para comprobar el hardware sin depender de un juego. El
  efecto se borra aproximadamente un segundo después.

`--no-ff` desactiva todo el subsistema. Algunos juegos usan haptics avanzados
del DualSense que no pasan por la API FF clásica; esos efectos no se reenvían.

## Ocultación

La ocultación tiene dos capas:

### 1. Reglas udev

`udev/72-ds4xbox.rules` se instala como `72-ds4xbox.rules` para ejecutarse:

1. Después de `70-uaccess.rules`, que añade `TAG+="uaccess"` a los dispositivos
   de entrada.
2. Antes de `73-seat-late.rules`, que aplica la ACL de `uaccess`.

La regla:

- Coincide con los HID de Sony (`054c:05c4`, `09cc`, `0ba0`, `0ce6`, `0df2`) y
  con nombres de dispositivos PlayStation.
- Borra `ID_INPUT`, `ID_INPUT_JOYSTICK`, `ID_INPUT_KEY`, `ID_INPUT_TOUCHPAD`,
  etc.
- Borra la etiqueta `uaccess`; conserva `ID_SEAT` y `seat` para que el acceso
  normal se pueda restaurar al parar el daemon.
- Pone `OWNER="root"`, `GROUP="root"`, `MODE="0600"`.
- Pone `LIBINPUT_IGNORE_DEVICE=1`.
- Hace lo mismo con los nodos `hidraw` y con el nodo USB del dispositivo.

Al quitar `uaccess` antes de que `73-seat-late.rules` aplique la ACL, el
usuario de la sesión no recibe acceso a los nodos originales.

La regla solo se aplica si existe `/run/ds4xbox/active`. El daemon crea ese
flag al arrancar, lo borra al terminar y ejecuta `udevadm trigger` para que
udev vuelva a evaluar los dispositivos. Cuando el flag no existe, una rama
anterior de la regla restaura explícitamente `MODE=0660`, `GROUP=input`,
`TAG+="uaccess"` y limpia `LIBINPUT_IGNORE_DEVICE`; así el mando original
queda libre aunque el daemon se haya cerrado. `ExecStopPost` de systemd repite
la limpieza como red de seguridad para un `SIGKILL` o un cuelgue.

### 2. EVIOCGRAB

Aunque un programa consiga abrir el nodo (por ejemplo, ejecutándose como
root), `EVIOCGRAB` hace que solo el daemon reciba los eventos. El daemon
captura el gamepad, el touchpad, los sensores y el jack.

### Límites

- `lsusb` y `/proc/bus/input/devices` siguen mostrando el dispositivo Sony.
  Los descriptores USB no se pueden cambiar desde espacio de usuario.
- Un proceso root puede abrir el nodo original; con `EVIOCGRAB` activo no
  recibe eventos, pero puede ver que existe.
- El mando virtual es un dispositivo `uinput`; algunos anti-cheat lo detectan.
- La regla udev oculta también otros dispositivos Sony con esos PIDs. Si usas
  un accesorio Sony con el mismo PID, añade una excepción.

## Hotplug, reconexión y varios mandos

- `inotify` observa `/dev/input` y marca `g_rescan` cuando hay cambios.
- Cada segundo se ejecuta un rescaneo de seguridad.
- `scan_devices()` enumera los nodos, agrupa y crea controladores nuevos.
- Cada controlador tiene una `generation` que se incrementa al recrearse; los
  `epoll_ref` guardan la generación para ignorar eventos de descriptores
  viejos.
- Si un nodo desaparece, el controlador se marca como muerto y se destruye;
  el mando virtual se cierra y se vuelve a crear cuando el mando reaparezca.
- Se pueden gestionar hasta `DS4_MAX_CONTROLLERS` (8) mandos.
- Si la creación de un controlador falla (por ejemplo, `/dev/uinput` no está
  disponible), el grupo se cachea durante 5 segundos para no repetir el error
  en cada rescaneo.

## Instancia única y servicio

El daemon usa `flock` no bloqueante sobre:

1. `/run/ds4xbox/lock`
2. `/run/ds4xbox.lock`
3. `/tmp/ds4xbox-UID.lock` (fallback)

El servicio systemd crea `/run/ds4xbox` con `RuntimeDirectory`. Si no se puede
crear ningún lock, el daemon continúa con un aviso: es preferible funcionar a
quedarse sin daemon, aunque podrían ejecutarse dos instancias.

El servicio corre como root porque:

- Los nodos originales quedan en `0600 root:root`.
- `/dev/uinput` suele ser `0600 root:root`.
- `EVIOCGRAB` y la creación de uinput se hacen con privilegios de root.

No necesita red, ni `CAP_SYS_ADMIN`, ni módulos del kernel más allá de
`uinput`.

## Pruebas

- `make test`: ejecuta `mapping_self_test()`, que prueba sticks, gatillos,
  hat, botones y el modo `--swap-sticks-triggers`.
- `tests/fake_ds4.c`: crea un DS4 falso con uinput, con ejes, botones, hat y
  FF. Imprime el nodo evdev y, con `--demo`, inyecta una secuencia repetida.
- `tests/read_pad.c`: lee un nodo evdev y muestra los eventos con nombres.
- `tests/integration.sh`: requiere root, crea el DS4 falso, arranca el daemon,
  localiza el mando virtual por `phys=ds4xbox/virtual` y comprueba el mapeo.

## Decisiones y alternativas

- **evdev + uinput en vez de un módulo del kernel:** no requiere compilar
  módulos ni firmar código; funciona en cualquier kernel moderno. A cambio,
  el dispositivo Sony sigue visible en USB.
- **udev + EVIOCGRAB en vez de solo udev:** udev oculta los nodos a usuarios
  normales; `EVIOCGRAB` protege frente a programas que abren el nodo antes o
  que corren con más privilegios.
- **Un solo hilo y epoll:** el volumen de eventos de un mando es bajo; un
  hilo evita bloqueos y carreras. La única operación potencialmente lenta es
  la subida de efectos FF, que es una ioctl corta.
- **Perfil Xbox 360 por defecto:** es el más compatible con SDL/Steam/Proton.
  El perfil Xbox One está disponible para juegos que lo prefieran.
- **Sin archivos de configuración:** las opciones son flags; el servicio
  systemd y el módulo NixOS las fijan. Esto simplifica el arranque y evita
  estado oculto.
