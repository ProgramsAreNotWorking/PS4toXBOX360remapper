/*
 * ds4xbox - command line interface
 *
 * This file is released into the public domain (Unlicense).
 */
#include "common.h"

#include <getopt.h>
#include <strings.h>

static const struct profile profiles[] = {
    {
        .name = "xbox360",
        .vendor = MICROSOFT_VENDOR,
        .product = 0x028e,
        .version = 0x0114,
        .device_name = "Microsoft X-Box 360 pad",
    },
    {
        .name = "xboxone",
        .vendor = MICROSOFT_VENDOR,
        .product = 0x02d1,
        .version = 0x0100,
        .device_name = "Microsoft X-Box One pad",
    },
};

const struct profile *profile_by_name(const char *name)
{
    if (name == NULL)
        return NULL;

    if (strcasecmp(name, "xbox360") == 0 ||
        strcasecmp(name, "x360") == 0 ||
        strcasecmp(name, "360") == 0)
        return &profiles[0];

    if (strcasecmp(name, "xboxone") == 0 ||
        strcasecmp(name, "xone") == 0 ||
        strcasecmp(name, "one") == 0)
        return &profiles[1];

    return NULL;
}

void print_usage(FILE *out)
{
    fprintf(out,
        "Uso: ds4xbox [opciones]\n"
        "\n"
        "Convierte un mando DualShock 4 / DualSense en un mando Xbox 360\n"
        "o Xbox One virtual, ocultando el mando original a los demas\n"
        "programas (con las reglas udev instaladas y ejecutando como root).\n"
        "\n"
        "Opciones:\n"
        "  -p, --profile NOMBRE        perfil virtual: xbox360 (por defecto)\n"
        "                              o xboxone\n"
        "  -d, --device RUTA           gestionar solo /dev/input/eventN\n"
        "      --force                 permitir --device con un mando que no\n"
        "                              sea Sony/PlayStation (mapeo DS4 forzado)\n"
        "      --no-grab               no capturar los dispositivos originales\n"
        "                              (solo para depuracion; no oculta nada)\n"
        "      --no-ff                 desactivar la vibracion/force feedback\n"
        "      --test-rumble           probar la vibracion al detectar el mando\n"
        "      --swap-sticks-triggers  clones con el stick derecho en Z/RZ y\n"
        "                              los gatillos en RX/RY\n"
        "  -l, --list                  listar dispositivos de entrada y salir\n"
        "      --self-test             ejecutar las pruebas de mapeo y salir\n"
        "  -v, --verbose               mostrar registros de depuracion\n"
        "  -q, --quiet                 mostrar solo errores\n"
        "  -V, --version               mostrar la version y salir\n"
        "  -h, --help                  mostrar esta ayuda y salir\n"
        "\n"
        "Ejemplos:\n"
        "  sudo ds4xbox\n"
        "  sudo ds4xbox --profile xboxone\n"
        "  sudo ds4xbox --device /dev/input/event12 --verbose\n"
        "  ds4xbox --list\n"
        "\n"
        "La primera vez hay que emparejar el mando por Bluetooth con BlueZ.\n"
        "El daemon se encarga de la reconexion y del hotplug USB/Bluetooth.\n");
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        { "profile", required_argument, NULL, 'p' },
        { "device", required_argument, NULL, 'd' },
        { "force", no_argument, NULL, 1000 },
        { "no-grab", no_argument, NULL, 1001 },
        { "no-ff", no_argument, NULL, 1002 },
        { "test-rumble", no_argument, NULL, 1005 },
        { "swap-sticks-triggers", no_argument, NULL, 1003 },
        { "list", no_argument, NULL, 'l' },
        { "self-test", no_argument, NULL, 1004 },
        { "verbose", no_argument, NULL, 'v' },
        { "quiet", no_argument, NULL, 'q' },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct daemon daemon;
    const struct profile *profile = &profiles[0];
    bool list = false;
    bool self_test = false;
    int option;
    int result;

    memset(&daemon, 0, sizeof(daemon));
    daemon.epoll_fd = -1;
    daemon.inotify_fd = -1;
    daemon.inotify_wd = -1;
    daemon.lock_fd = -1;

    opterr = 0;

    while ((option = getopt_long(argc, argv, "p:d:lvqVh",
                                 long_options, NULL)) != -1) {
        switch (option) {
        case 'p':
            profile = profile_by_name(optarg);
            if (profile == NULL) {
                fprintf(stderr,
                        "ds4xbox: perfil desconocido '%s' "
                        "(use xbox360 o xboxone)\n", optarg);
                return 2;
            }
            break;

        case 'd':
            if (optarg[0] != '/') {
                fprintf(stderr,
                        "ds4xbox: --device necesita una ruta absoluta "
                        "(por ejemplo /dev/input/event12)\n");
                return 2;
            }
            if (strlen(optarg) >= sizeof(daemon.only_device)) {
                fprintf(stderr, "ds4xbox: la ruta del dispositivo es "
                                "demasiado larga\n");
                return 2;
            }
            snprintf(daemon.only_device, sizeof(daemon.only_device),
                     "%s", optarg);
            break;

        case 1000:
            daemon.force = true;
            break;

        case 1001:
            daemon.no_grab = true;
            break;

        case 1002:
            daemon.no_ff = true;
            break;

        case 1003:
            daemon.swap_sticks_triggers = true;
            break;

        case 1005:
            daemon.test_rumble = true;
            break;

        case 'l':
            list = true;
            break;

        case 1004:
            self_test = true;
            break;

        case 'v':
            g_log_level = LOG_LVL_DEBUG;
            break;

        case 'q':
            g_log_level = LOG_LVL_ERROR;
            break;

        case 'V':
            printf("ds4xbox %s\n", DS4XBOX_VERSION);
            return 0;

        case 'h':
            print_usage(stdout);
            return 0;

        case '?':
        default:
            if (optopt != 0)
                fprintf(stderr, "ds4xbox: opcion no valida '-%c'\n",
                        optopt);
            else
                fprintf(stderr, "ds4xbox: opcion no valida\n");
            print_usage(stderr);
            return 2;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "ds4xbox: argumento inesperado '%s'\n",
                argv[optind]);
        print_usage(stderr);
        return 2;
    }

    if (self_test)
        return mapping_self_test() == 0 ? 0 : 1;

    if (list)
        return list_devices() == 0 ? 0 : 1;

    if (daemon.force && daemon.only_device[0] == '\0') {
        fprintf(stderr, "ds4xbox: --force solo tiene sentido junto con "
                        "--device\n");
        return 2;
    }

    daemon.profile = *profile;
    daemon.verbose = g_log_level >= LOG_LVL_DEBUG;
    daemon.quiet = g_log_level == LOG_LVL_ERROR;

    result = daemon_run(&daemon);
    return result == 0 ? 0 : 1;
}
