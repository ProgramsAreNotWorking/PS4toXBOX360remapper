/*
 * read_pad - print evdev events from a device for a few seconds.
 *
 * Usage:
 *   tests/read_pad /dev/input/eventN [seconds]
 *
 * This file is released into the public domain (Unlicense).
 */
#include "../src/common.h"

#include <poll.h>
#include <time.h>

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 +
           (uint64_t)ts.tv_nsec / 1000000;
}

static const char *key_name(unsigned int code)
{
    switch (code) {
    case BTN_SOUTH: return "BTN_SOUTH (A)";
    case BTN_EAST: return "BTN_EAST (B)";
    case BTN_NORTH: return "BTN_NORTH (X)";
    case BTN_WEST: return "BTN_WEST (Y)";
    case BTN_TL: return "BTN_TL (LB)";
    case BTN_TR: return "BTN_TR (RB)";
    case BTN_SELECT: return "BTN_SELECT (Back)";
    case BTN_START: return "BTN_START (Start)";
    case BTN_MODE: return "BTN_MODE (Guide)";
    case BTN_THUMBL: return "BTN_THUMBL (L3)";
    case BTN_THUMBR: return "BTN_THUMBR (R3)";
    default: return NULL;
    }
}

static const char *abs_name(unsigned int code)
{
    switch (code) {
    case ABS_X: return "ABS_X";
    case ABS_Y: return "ABS_Y";
    case ABS_RX: return "ABS_RX";
    case ABS_RY: return "ABS_RY";
    case ABS_Z: return "ABS_Z";
    case ABS_RZ: return "ABS_RZ";
    case ABS_HAT0X: return "ABS_HAT0X";
    case ABS_HAT0Y: return "ABS_HAT0Y";
    default: return NULL;
    }
}

static void print_event(const struct input_event *event)
{
    const char *name = NULL;

    if (event->type == EV_KEY)
        name = key_name(event->code);
    else if (event->type == EV_ABS)
        name = abs_name(event->code);

    if (name != NULL) {
        printf("%-20s code=%-3u value=%d\n", name,
               (unsigned)event->code, event->value);
    } else if (event->type == EV_FF) {
        printf("EV_FF                code=%-3u value=%d\n",
               (unsigned)event->code, event->value);
    } else {
        printf("type=%-3u code=%-3u value=%d\n",
               (unsigned)event->type, (unsigned)event->code,
               event->value);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct pollfd pfd;
    struct input_event event;
    char name[DS4_NAME_MAX];
    uint64_t end;
    int seconds = 5;
    int fd;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s /dev/input/eventN [seconds]\n",
                argv[0]);
        return 2;
    }

    if (argc == 3)
        seconds = atoi(argv[2]);
    if (seconds <= 0)
        seconds = 5;

    fd = open(argv[1], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }

    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        snprintf(name, sizeof(name), "(unknown)");

    printf("Device: %s\n", name);
    printf("Reading %d second(s)...\n", seconds);
    fflush(stdout);

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    end = now_ms() + (uint64_t)seconds * 1000;

    while (now_ms() < end) {
        int ready = poll(&pfd, 1, 100);

        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (ready == 0)
            continue;

        while (read(fd, &event, sizeof(event)) ==
               (ssize_t)sizeof(event)) {
            print_event(&event);
        }
    }

    close(fd);
    return 0;
}
