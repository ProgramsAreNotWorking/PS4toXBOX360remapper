/*
 * fake_ds4 - create a uinput device that looks like a DualShock 4.
 *
 * This is a test helper.  It needs access to /dev/uinput (usually root).
 *
 * Usage:
 *   sudo tests/fake_ds4 [--demo]
 *
 * It prints the /dev/input/eventN path on stdout and keeps running until
 * SIGINT/SIGTERM.  With --demo it injects a repeating sequence of events.
 *
 * This file is released into the public domain (Unlicense).
 */
#include "../src/common.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int emit_event(int fd, unsigned int type, unsigned int code, int value)
{
    struct input_event event;

    memset(&event, 0, sizeof(event));
    event.type = (uint16_t)type;
    event.code = (uint16_t)code;
    event.value = value;

    if (write(fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
        perror("write(uinput)");
        return -1;
    }
    return 0;
}

static int emit_syn(int fd)
{
    return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static void drain_ff(int fd)
{
    struct input_event event;

    while (read(fd, &event, sizeof(event)) == (ssize_t)sizeof(event)) {
        if (event.type == EV_FF) {
            printf("FF code=%u value=%d\n",
                   (unsigned)event.code, event.value);
            fflush(stdout);
        }
    }
}

static int tap(int fd, unsigned int code)
{
    if (emit_event(fd, EV_KEY, code, 1) < 0 || emit_syn(fd) < 0)
        return -1;
    usleep(150000);
    if (emit_event(fd, EV_KEY, code, 0) < 0 || emit_syn(fd) < 0)
        return -1;
    usleep(150000);
    return 0;
}

static int find_event_node(const char *sysname, char *out, size_t out_size)
{
    char dirpath[PATH_MAX];
    DIR *dir;
    struct dirent *de;
    int found = -1;

    snprintf(dirpath, sizeof(dirpath), "/sys/class/input/%s", sysname);
    dir = opendir(dirpath);
    if (dir == NULL)
        return -1;

    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        snprintf(out, out_size, "/dev/input/%s", de->d_name);
        found = 0;
        break;
    }

    closedir(dir);
    return found;
}

static int create_fake_ds4(void)
{
    static const unsigned int keys[] = {
        BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
        BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
        BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR,
    };
    static const struct {
        unsigned int code;
        int minimum;
        int maximum;
        int fuzz;
        int flat;
    } axes[] = {
        { ABS_X, 0, 255, 0, 0 },
        { ABS_Y, 0, 255, 0, 0 },
        { ABS_RX, 0, 255, 0, 0 },
        { ABS_RY, 0, 255, 0, 0 },
        { ABS_Z, 0, 255, 0, 0 },
        { ABS_RZ, 0, 255, 0, 0 },
        { ABS_HAT0X, -1, 1, 0, 0 },
        { ABS_HAT0Y, -1, 1, 0, 0 },
    };
    struct uinput_setup setup;
    char phys[128];
    char sysname[64];
    char event_path[PATH_MAX];
    int fd;
    size_t i;

    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("open(/dev/uinput)");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_FF) < 0) {
        perror("UI_SET_EVBIT");
        close(fd);
        return -1;
    }

    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, keys[i]) < 0) {
            perror("UI_SET_KEYBIT");
            close(fd);
            return -1;
        }
    }

    for (i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
        struct uinput_abs_setup abs_setup;

        if (ioctl(fd, UI_SET_ABSBIT, axes[i].code) < 0) {
            perror("UI_SET_ABSBIT");
            close(fd);
            return -1;
        }

        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.code = (uint16_t)axes[i].code;
        abs_setup.absinfo.minimum = axes[i].minimum;
        abs_setup.absinfo.maximum = axes[i].maximum;
        abs_setup.absinfo.fuzz = axes[i].fuzz;
        abs_setup.absinfo.flat = axes[i].flat;
        abs_setup.absinfo.value = 0;

        if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
            perror("UI_ABS_SETUP");
            close(fd);
            return -1;
        }
    }

    if (ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) < 0 ||
        ioctl(fd, UI_SET_FFBIT, FF_GAIN) < 0 ||
        ioctl(fd, UI_SET_FFBIT, FF_AUTOCENTER) < 0) {
        perror("UI_SET_FFBIT");
        close(fd);
        return -1;
    }

    snprintf(phys, sizeof(phys), "ds4xbox-test-%ld/input0",
             (long)getpid());
    if (ioctl(fd, UI_SET_PHYS, phys) < 0) {
        perror("UI_SET_PHYS");
        close(fd);
        return -1;
    }

    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = SONY_VENDOR;
    setup.id.product = 0x05c4;
    setup.id.version = 0x0100;
    snprintf(setup.name, sizeof(setup.name),
             "Sony Interactive Entertainment Wireless Controller");
    setup.ff_effects_max = 16;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        perror("UI_DEV_SETUP");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    memset(sysname, 0, sizeof(sysname));
    if (ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
        perror("UI_GET_SYSNAME");
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        return -1;
    }

    if (find_event_node(sysname, event_path, sizeof(event_path)) < 0) {
        fprintf(stderr, "cannot find event node for %s\n", sysname);
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        return -1;
    }

    printf("%s\n", event_path);
    fflush(stdout);

    return fd;
}

int main(int argc, char **argv)
{
    bool demo = false;
    int fd;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else {
            fprintf(stderr, "usage: %s [--demo]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = create_fake_ds4();
    if (fd < 0)
        return 1;

    if (!demo) {
        printf("Fake DS4 ready.  Press Ctrl+C to remove it.\n");
        fflush(stdout);
        while (!g_stop) {
            drain_ff(fd);
            usleep(100000);
        }
    } else {
        printf("Fake DS4 demo running.  Press Ctrl+C to stop.\n");
        fflush(stdout);
        while (!g_stop) {
            drain_ff(fd);
            if (tap(fd, BTN_SOUTH) < 0)
                break;
            if (tap(fd, BTN_WEST) < 0)
                break;

            if (emit_event(fd, EV_ABS, ABS_X, 255) < 0 ||
                emit_syn(fd) < 0)
                break;
            usleep(300000);
            if (emit_event(fd, EV_ABS, ABS_X, 128) < 0 ||
                emit_syn(fd) < 0)
                break;
            usleep(100000);

            if (emit_event(fd, EV_ABS, ABS_Z, 255) < 0 ||
                emit_syn(fd) < 0)
                break;
            usleep(300000);
            if (emit_event(fd, EV_ABS, ABS_Z, 0) < 0 ||
                emit_syn(fd) < 0)
                break;
            usleep(100000);

            if (emit_event(fd, EV_ABS, ABS_HAT0Y, -1) < 0 ||
                emit_syn(fd) < 0)
                break;
            usleep(300000);
            if (emit_event(fd, EV_ABS, ABS_HAT0Y, 0) < 0 ||
                emit_syn(fd) < 0)
                break;
            usleep(100000);
        }
    }

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
