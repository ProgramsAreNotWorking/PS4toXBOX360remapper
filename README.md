# ds4xbox

Daemon ligero para Linux que convierte un mando **DualShock 4** (PS4) o
**DualSense** (PS5) en un mando **Xbox 360** o **Xbox One** virtual, y oculta
el mando original a los demás programas.

- Sin dependencias: solo C, `make` y las cabeceras del kernel.
- USB y Bluetooth, con hotplug y reconexión automática.
- Varios mandos a la vez.
- Vibración/force feedback reenviada al mando real.
- Perfil Xbox 360 por defecto; perfil Xbox One opcional.
- Reglas udev para que Steam, SDL y libinput no vean el mando Sony original.
- Servicio systemd y módulo NixOS incluidos.

> **Estado:** versión 1.0.0. El daemon compila y pasa las pruebas de mapeo.
> Ya se ha probado por USB con un DualShock 4; la prueba con Bluetooth, con
> DualSense y con otros modelos sigue pendiente. Para probarlo hace falta
> instalar las reglas udev y ejecutar el daemon como root; el instalador se
> encarga de ambas cosas.

---

## Cómo funciona

```
Mando PS4/PS5 (USB o Bluetooth)
        |
        |  evdev: /dev/input/eventN
        v
+-------------------+       EVIOCGRAB        +----------------------+
| ds4xbox (root)    | --- captura exclusiva ->| nodos Sony ocultos   |
|                   |                         | 0600 root, sin       |
|  lee y traduce    |                         | ID_INPUT_JOYSTICK    |
+---------+---------+                         +----------------------+
          |
          |  /dev/uinput
          v
+-------------------+       udev + uaccess
| Xbox 360/One      | ---> visible para el usuario y los juegos
| virtual           |
+-------------------+
```

El daemon abre el nodo evdev del gamepad y, si existe, el del clic del
touchpad; captura esos nodos con `EVIOCGRAB` y crea un mando Xbox virtual con
`/dev/uinput`. No lee los nodos de sensores ni de audio porque no se traducen.
Las reglas udev dejan todos los nodos originales en `0600 root:root`, les
quitan la etiqueta `uaccess` y borran
`ID_INPUT*`, de modo que Steam, SDL, libinput y los juegos no los detectan
como mando. La etiqueta `seat` se conserva para poder restaurar el acceso
normal cuando el daemon se detiene.

El mando virtual usa los identificadores de Microsoft:

| Perfil   | VID:PID     | Nombre                     |
|----------|-------------|----------------------------|
| Xbox 360 | `045e:028e` | `Microsoft X-Box 360 pad`  |
| Xbox One | `045e:02d1` | `Microsoft X-Box One pad`  |

La mayoría de juegos, Steam y Proton reconocen el perfil Xbox 360 sin
configuración adicional. Si un juego concreto prefiere Xbox One, usa
`--profile xboxone`.

---

## Instalación rápida (systemd/udev)

Requisitos:

- Linux con `evdev`, `uinput` y udev (systemd).
- Kernel 4.5 o superior (recomendado 6.x).
- `make`, `gcc` y cabeceras del kernel.
- BlueZ para Bluetooth.

```bash
git clone <este-repositorio> ds4xbox
cd ds4xbox
make
sudo ./scripts/install.sh
```

El instalador:

1. Instala el binario en `/usr/local/bin/ds4xbox`.
2. Instala `72-ds4xbox.rules` en `/etc/udev/rules.d/`.
3. Instala y activa `ds4xbox.service`.
4. Recarga udev y systemd.

Después conecta el mando por USB o empareja por Bluetooth:

```bash
./scripts/pair-bluetooth.sh
```

Comprueba el estado:

```bash
systemctl status ds4xbox.service
journalctl -u ds4xbox.service -f
/usr/local/bin/ds4xbox --list
```

En NixOS, `/usr/local/bin` no suele estar en el `PATH`; usa la ruta absoluta
`/usr/local/bin/ds4xbox` (el módulo NixOS sí lo deja en el `PATH`).

En la salida de `--list`, el mando Sony original aparecerá como
`(permission denied)` si lo ejecutas como usuario normal. **Eso es
intencionado**: las reglas udev lo ocultan. El mando virtual
`Microsoft X-Box 360 pad` debe aparecer y ser usable.

### NixOS (recomendado)

En NixOS, `/etc` es de solo lectura. El instalador lo detecta y, para una
prueba rápida, coloca la regla udev y la unidad systemd en `/run` (desaparecen
al reiniciar). Para que la instalación sea permanente, usa el módulo NixOS que
se describe a continuación.

El repositorio incluye las dos formas: **sin flakes** (`default.nix`) y con
flakes (`flake.nix`). Si los flakes no funcionan en tu equipo, usa la primera.

#### Sin flakes

Compilar el paquete:

```bash
nix-build
./result/bin/ds4xbox --self-test
```

Importar el módulo NixOS directamente desde `configuration.nix`:

```nix
{ ... }:

{
  imports = [
    /ruta/a/PS4toXBOX360remapper/nix/module.nix
  ];

  services.ds4xbox = {
    enable = true;
    profile = "xbox360";          # o "xboxone"
    swapSticksTriggers = false;   # true para algunos clones
    noForceFeedback = false;      # true para desactivar la vibración
    extraArgs = [ ];              # argumentos adicionales
  };
}
```

Después:

```bash
sudo nixos-rebuild switch
```

El módulo carga `uinput`, instala las reglas udev, activa el servicio y añade
`ds4xbox` al PATH del sistema.

Para desarrollo sin flakes:

```bash
nix-shell
make
make test
```

#### Con flakes (opcional)

```nix
{
  inputs.ds4xbox.url = "path:/ruta/a/PS4toXBOX360remapper";

  outputs = { self, nixpkgs, ds4xbox }: {
    nixosConfigurations.miPC = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ds4xbox.nixosModules.default
        {
          services.ds4xbox = {
            enable = true;
            profile = "xbox360";
          };
        }
      ];
    };
  };
}
```

Luego:

```bash
sudo nixos-rebuild switch --flake .#miPC
```

Compilar sin instalar con flakes:

```bash
nix build path:.          # funciona aunque el árbol no esté commiteado
sudo ./result/bin/ds4xbox --verbose
```

Dev shell con flakes:

```bash
nix develop
make
make test
```

### Otros sistemas sin systemd

El daemon no depende de systemd. Instala el binario y la regla udev a mano,
y ejecútalo como root:

```bash
sudo install -Dm755 ds4xbox /usr/local/bin/ds4xbox
sudo install -Dm644 udev/72-ds4xbox.rules /etc/udev/rules.d/72-ds4xbox.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input --action=change
sudo /usr/local/bin/ds4xbox --verbose
```

La regla **debe** llamarse `72-ds4xbox.rules`: tiene que ejecutarse después de
`70-uaccess.rules` (que añade `uaccess`) y antes de `73-seat-late.rules` (que
aplica la ACL). Si tu distribución numera las reglas de otra forma, colócala
entre esas dos.

---

## Uso

```bash
# Daemon en primer plano, con registros detallados
sudo ds4xbox --verbose

# Perfil Xbox One
sudo ds4xbox --profile xboxone

# Un solo mando concreto
sudo ds4xbox --device /dev/input/event12 --verbose

# Guardar eventos físicos y virtuales con marcas de tiempo para compararlos
sudo ds4xbox --verbose --trace-prefix /tmp/ds4xbox

# Clones con el stick derecho en Z/RZ y los gatillos en RX/RY
sudo ds4xbox --swap-sticks-triggers

# Listar dispositivos de entrada
ds4xbox --list

# Pruebas internas de mapeo
ds4xbox --self-test
```

Opciones:

| Opción | Descripción |
|--------|-------------|
| `-p, --profile xbox360\|xboxone` | Perfil del mando virtual. Por defecto `xbox360`. |
| `-d, --device RUTA` | Gestionar solo `/dev/input/eventN`. |
| `--force` | Permite `--device` con un mando que no sea Sony. |
| `--no-grab` | No captura los nodos originales. Solo depuración: no oculta nada. |
| `--no-ff` | Desactiva la vibración/force feedback. |
| `--test-rumble` | Al detectar el mando, reproduce una vibración corta de prueba. |
| `--swap-sticks-triggers` | Intercambia stick derecho y gatillos en clones. |
| `--trace-prefix RUTA` | Guarda eventos físicos y virtuales en dos CSV al salir. |
| `-l, --list` | Lista dispositivos de entrada y sale. |
| `--self-test` | Ejecuta las pruebas de mapeo y sale. |
| `-v, --verbose` | Registros de depuración. |
| `-q, --quiet` | Solo errores. |
| `-V, --version` | Muestra la versión. |
| `-h, --help` | Muestra la ayuda. |

### Mapeo

| DualShock 4 / DualSense | Xbox 360 / Xbox One |
|--------------------------|---------------------|
| Cruz (Cross)             | A                   |
| Círculo (Circle)         | B                   |
| Cuadrado (Square)        | X                   |
| Triángulo (Triangle)     | Y                   |
| L1 / R1                  | LB / RB             |
| L2 / R2                  | LT / RT (analógicos)|
| Share / Create           | Back                |
| Options                  | Start               |
| PS                       | Guide               |
| L3 / R3                  | Clic de stick       |
| D-pad                    | D-pad               |
| Clic del touchpad        | Back                |
| Movimiento del touchpad  | No se reenvía       |
| Giroscopio/acelerómetro  | No se reenvía       |
| Altavoz / jack / luz     | No se reenvía       |

Los sticks se normalizan desde el rango del DS4 (`0..255`, centro `128`) al
rango firmado de Xbox (`-32768..32767`). Los gatillos se mantienen en
`0..255`. Si un mando clónico no expone ejes de gatillo, el daemon los
sintetiza a partir de `BTN_TL2`/`BTN_TR2`.

---

## Bluetooth

El emparejamiento se hace una sola vez con BlueZ. Después, el daemon detecta
la reconexión automáticamente.

1. Pon el mando en modo emparejamiento:
   - **DualShock 4:** mantén pulsados **Share + PS** hasta que la barra de luz
     parpadee rápidamente.
   - **DualSense:** mantén pulsados **Create + PS** hasta que la barra parpadee.
2. Ejecuta:

   ```bash
   ./scripts/pair-bluetooth.sh
   ```

3. Introduce la MAC del mando cuando te la pida. El script hace `pair`,
   `trust` y `connect`.

Si `bluetoothctl` no tiene permisos, ejecútalo con `sudo` o revisa la
configuración de polkit/BlueZ. El mando debe estar marcado como `trusted` para
que BlueZ permita la reconexión.

---

## Pruebas

### Pruebas automáticas de mapeo

```bash
make test
```

### Prueba de integración con un DS4 falso

Este test crea un DualShock 4 virtual con `uinput`, arranca el daemon, lee el
mando Xbox virtual y comprueba que Cruz→A, Cuadrado→X, L2→LT y el D-pad
funcionan. **Necesita root** porque crea dispositivos y lee nodos evdev:

```bash
sudo tests/integration.sh
```

Si tienes instalado el servicio, el script lo para temporalmente y lo vuelve
a arrancar al terminar.

### Prueba con un mando real

1. Instala las reglas y el servicio:

   ```bash
   sudo ./scripts/install.sh
   ```

2. Conecta el mando por USB o empareja por Bluetooth.

3. Comprueba los registros:

   ```bash
   journalctl -u ds4xbox.service -f
   ```

   Deberías ver algo como:

   ```
   controller 1: Sony Interactive Entertainment Wireless Controller (054c:05c4) -> virtual Microsoft X-Box 360 pad
   controller 1: grabbed /dev/input/eventX (gamepad)
   ```

4. Comprueba el mando virtual:

   ```bash
   /usr/local/bin/ds4xbox --list
   ls -l /dev/input/by-id/ | grep -i x-box
   ```

5. Prueba con `evtest`, `jstest` o directamente con un juego:

   ```bash
   sudo evtest
   ```

   El mando virtual debe aparecer como `Microsoft X-Box 360 pad`. Pulsa los
   botones y comprueba que los eventos llegan.

6. Prueba la vibración en un juego compatible o con la casilla de vibración
   del probador de mandos. El mando virtual anuncia `FF_RUMBLE`; el daemon
   reenvía las subidas, reproducciones y borrados de efectos al mando real.
   Con `--verbose` debe aparecer:

   ```text
   controller 1: force feedback enabled (16 effect slots)
   ```

Si algo falla, ejecuta:

```bash
./scripts/diagnose.sh
```

---

## Ocultación: qué se consigue y qué no

Con las reglas udev instaladas y el daemon ejecutándose como root:

- Los nodos evdev, `js` y `hidraw` del mando Sony quedan en `0600 root:root`.
- Se eliminan `ID_INPUT`, `ID_INPUT_JOYSTICK`, `ID_INPUT_TOUCHPAD`, etc.
- Se elimina la etiqueta `uaccess`, así que el usuario de la sesión no recibe
  ACL sobre esos nodos. La etiqueta `seat` se conserva para que udev pueda
  restaurar el acceso normal cuando el daemon se detiene.
- Se marca `LIBINPUT_IGNORE_DEVICE=1` para que libinput lo ignore.
- El daemon captura el gamepad y el touchpad con `EVIOCGRAB`, de modo que otro
  programa que consiga abrirlos no recibe eventos. Los nodos de sensores y
  audio siguen ocultos para usuarios normales por los permisos y propiedades
  udev, aunque el daemon no los abra.

La ocultación es **temporal**: solo está activa mientras el daemon está en
ejecución. Al pararlo (o al parar el servicio) se elimina el flag
`/run/ds4xbox/active`, se vuelve a ejecutar udev y el mando original queda
libre y visible para el resto del sistema. Si el proceso muere de forma
anormal, la unidad systemd lo limpia en `ExecStopPost`.

Limitaciones honestas:

- **`lsusb` y `/proc/bus/input/devices` siguen mostrando el dispositivo Sony.**
  Los descriptores USB no se pueden cambiar desde espacio de usuario. Lo que se
  oculta es el acceso a los eventos y la detección como mando.
- Un proceso que se ejecute como **root** puede abrir los nodos originales si
  el daemon no los ha capturado; con `EVIOCGRAB` activo, no recibe eventos.
- El mando virtual es un dispositivo `uinput`. Algunos sistemas anti-cheat
  pueden tratarlo de forma especial; no hay garantía en juegos competitivos.
- No se reenvían touchpad como ratón, giroscopio, acelerómetro, luz, altavoz,
  jack de audio ni gatillos adaptativos del DualSense.
- No hay perfiles por juego. La idea es que el mando virtual Xbox funcione
  globalmente; para ajustes por juego, usa la configuración del propio juego o
  Steam Input.

---

## Solución de problemas rápida

| Síntoma | Qué comprobar |
|---------|---------------|
| `cannot open /dev/uinput` | `sudo modprobe uinput`; revisa permisos de `/dev/uinput`. |
| `EACCES` al abrir evdev | Ejecuta como root e instala las reglas udev. |
| `EBUSY` al capturar | Steam, `ds4drv`, `antimicro` u otro programa tiene el mando. Ciérralo. |
| No aparece el mando virtual | `journalctl -u ds4xbox -n 100`; comprueba `UI_DEV_CREATE` y `/dev/uinput`. |
| El mando original sigue visible | Las reglas udev no se aplicaron. Revisa el nombre `72-ds4xbox.rules`, recarga udev y reconecta. |
| El mando original sigue oculto tras parar | Comprueba que no exista `/run/ds4xbox/active`; recarga udev y reconecta. El servicio lo limpia en `ExecStopPost`. |
| Steam ve dos mandos | Cierra Steam, instala las reglas, recarga udev y vuelve a abrir Steam. Desactiva Steam Input para el mando virtual si es necesario. |
| No funciona por Bluetooth | `bluetoothctl info MAC`; haz `trust` y `connect`; revisa `dmesg` y `journalctl`. |
| Los sticks/gatillos están cruzados | Prueba `--swap-sticks-triggers`. |
| Un clon no se detecta | Usa `--device /dev/input/eventN --force` y revisa `evtest`. |
| La vibración no funciona | Comprueba que el juego soporte FF; no uses `--no-ff`; busca `force feedback enabled` en el log; prueba la casilla de vibración del probador. |
| Hay tirones o se pierden movimientos | Actualiza a esta versión (cola de salida no bloqueante); si aparece `uinput queue overflow`, el programa que lee el mando virtual va demasiado lento. |

Para más detalle, consulta:

- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md)

---

## Desinstalación

```bash
sudo ./scripts/uninstall.sh
```

El script para el servicio, elimina el binario, la regla udev, la unidad
systemd, la documentación instalada y el flag `/run/ds4xbox/active`. Después
recarga udev y systemd para que el mando original vuelva a estar disponible.

---

## Estructura del proyecto

```
src/            Daemon en C (evdev + uinput)
udev/           Regla 72-ds4xbox.rules
systemd/        Unidad ds4xbox.service.in
scripts/        Instalación, desinstalación, emparejamiento y diagnóstico
nix/            Paquete y módulo NixOS
tests/          DS4 falso, lector evdev y test de integración
docs/           Arquitectura, problemas y compatibilidad
Makefile        Compilación, instalación y pruebas
```

---

## Uso de inteligencia artificial

Este proyecto se desarrolló con apoyo de inteligencia artificial: DeepSeek
V4.1 Flash se utilizó durante el desarrollo y GPT-6 Luna Max para los retoques
finales.

## Licencia

Este proyecto se publica en el dominio público bajo la
[Unlicense](LICENSE). Úsalo, modifícalo y redistribúyelo sin restricciones.
