/*
 * ds4xbox - DualShock 4 / DualSense to Xbox 360/One remapper for Linux
 *
 * This file is released into the public domain (Unlicense).
 */
#include "common.h"

#include <time.h>

#define DPAD_UP 0x01u
#define DPAD_DOWN 0x02u
#define DPAD_LEFT 0x04u
#define DPAD_RIGHT 0x08u

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_rescan = 0;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static bool trace_path(char *path, size_t path_size, const char *prefix,
                       const char *suffix)
{
    size_t prefix_size = strlen(prefix);
    size_t suffix_size = strlen(suffix);

    if (path_size == 0 || prefix_size >= path_size ||
        suffix_size >= path_size - prefix_size)
        return false;

    memcpy(path, prefix, prefix_size);
    memcpy(path + prefix_size, suffix, suffix_size + 1);
    return true;
}

static void daemon_trace_add(struct daemon *d, uint8_t stream,
                             const struct controller *c, uint64_t report_seq,
                             uint64_t timestamp_ns, uint64_t observed_ns,
                             const struct input_event *event)
{
    struct trace_record *record;
    size_t index;

    if (d->trace_records == NULL || event == NULL)
        return;

    if (d->trace_count < d->trace_capacity) {
        index = (d->trace_start + d->trace_count) % d->trace_capacity;
        d->trace_count++;
    } else {
        index = d->trace_start;
        d->trace_start = (d->trace_start + 1) % d->trace_capacity;
        d->trace_overwritten++;
    }

    record = &d->trace_records[index];
    record->timestamp_ns = timestamp_ns;
    record->observed_ns = observed_ns;
    record->report_seq = report_seq;
    record->generation = c->generation;
    record->value = event->value;
    record->type = event->type;
    record->code = event->code;
    record->controller = (uint8_t)(c->index + 1);
    record->stream = stream;
}

static void copy_string(char *destination, size_t size, const char *source)
{
    if (size == 0)
        return;
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    snprintf(destination, size, "%s", source);
}

static bool name_is_playstation(const char *name)
{
    if (name == NULL)
        return false;

    return strstr(name, "DualShock") != NULL ||
           strstr(name, "DualSense") != NULL ||
           strstr(name, "PlayStation") != NULL ||
           strstr(name, "PS4") != NULL ||
           strstr(name, "Sony Interactive Entertainment") != NULL ||
           strstr(name, "Sony Computer Entertainment") != NULL;
}

/*
 * A device is treated as a PlayStation gamepad when it has the right
 * capabilities and either comes from Sony or has a recognisable name.
 * The generic "Wireless Controller" name is deliberately not enough:
 * several non-PlayStation pads use it.
 */
static bool device_is_playstation(const struct device_info *di)
{
    if (!di->has_gamepad_caps)
        return false;
    if (di->id.vendor == SONY_VENDOR)
        return true;
    return name_is_playstation(di->name);
}

static bool check_gamepad_caps(const unsigned long *keys,
                               const unsigned long *abs)
{
    static const unsigned int required_keys[] = {
        BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
        BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
        BTN_THUMBL, BTN_THUMBR,
    };
    static const unsigned int required_abs[] = {
        ABS_X, ABS_Y, ABS_RX, ABS_RY,
    };
    bool triggers;
    bool dpad;
    size_t i;

    for (i = 0; i < sizeof(required_keys) / sizeof(required_keys[0]); i++) {
        if (!bit_test(keys, required_keys[i]))
            return false;
    }

    for (i = 0; i < sizeof(required_abs) / sizeof(required_abs[0]); i++) {
        if (!bit_test(abs, required_abs[i]))
            return false;
    }

    triggers = (bit_test(abs, ABS_Z) && bit_test(abs, ABS_RZ)) ||
               (bit_test(keys, BTN_TL2) && bit_test(keys, BTN_TR2));
    dpad = (bit_test(abs, ABS_HAT0X) && bit_test(abs, ABS_HAT0Y)) ||
           (bit_test(keys, BTN_DPAD_UP) && bit_test(keys, BTN_DPAD_DOWN) &&
            bit_test(keys, BTN_DPAD_LEFT) && bit_test(keys, BTN_DPAD_RIGHT));

    return triggers && dpad;
}

static void make_group_key(struct device_info *di)
{
    if (di->phys[0] != '\0') {
        char base[DS4_NAME_MAX];
        char *slash;
        char *end;
        long number;
        size_t len;

        snprintf(base, sizeof(base), "%.*s",
                 (int)(sizeof(base) - 1), di->phys);

        /*
         * A HID device exposes several input nodes (gamepad, touchpad,
         * motion sensors, jack) whose phys strings differ only in the
         * trailing "/inputN".  Strip that suffix so all nodes of the same
         * physical controller end up in the same group.
         */
        len = strlen(base);
        slash = strrchr(base, '/');
        if (slash != NULL && len > (size_t)(slash - base) + 6 &&
            strncmp(slash + 1, "input", 5) == 0) {
            number = strtol(slash + 6, &end, 10);
            if (end != slash + 6 && *end == '\0' && number >= 0)
                *slash = '\0';
        }

        snprintf(di->group, sizeof(di->group), "%s", base);
    } else if (di->uniq[0] != '\0') {
        snprintf(di->group, sizeof(di->group), "uniq:%.*s",
                 (int)(sizeof(di->group) - 6), di->uniq);
    } else {
        snprintf(di->group, sizeof(di->group), "path:%.*s",
                 (int)(sizeof(di->group) - 6), di->path);
    }
}

static void query_device(int fd, struct device_info *di)
{
    di->fd = fd;
    di->opened = true;

    if (ioctl(fd, EVIOCGID, &di->id) < 0)
        memset(&di->id, 0, sizeof(di->id));

    if (ioctl(fd, EVIOCGNAME(sizeof(di->name)), di->name) < 0)
        di->name[0] = '\0';

    if (ioctl(fd, EVIOCGPHYS(sizeof(di->phys)), di->phys) < 0)
        di->phys[0] = '\0';

    if (ioctl(fd, EVIOCGUNIQ(sizeof(di->uniq)), di->uniq) < 0)
        di->uniq[0] = '\0';

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(di->keybits)), di->keybits) < 0)
        memset(di->keybits, 0, sizeof(di->keybits));

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(di->absbits)), di->absbits) < 0)
        memset(di->absbits, 0, sizeof(di->absbits));

    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(di->ffbits)), di->ffbits) < 0)
        memset(di->ffbits, 0, sizeof(di->ffbits));

    di->has_gamepad_caps = check_gamepad_caps(di->keybits, di->absbits);
    di->is_playstation = device_is_playstation(di);
    make_group_key(di);
}

static int compare_device_info(const void *left, const void *right)
{
    const struct device_info *a = left;
    const struct device_info *b = right;

    return strcmp(a->path, b->path);
}

/*
 * Enumerate /dev/input/event*.  When include_inaccessible is true, nodes
 * that cannot be opened are still reported (used by --list).
 */
static int enumerate_devices(struct device_info *infos, int maximum,
                             bool include_inaccessible)
{
    DIR *directory;
    struct dirent *entry;
    int count = 0;

    directory = opendir("/dev/input");
    if (directory == NULL) {
        log_error("cannot open /dev/input: %s", strerror(errno));
        return -1;
    }

    while ((entry = readdir(directory)) != NULL && count < maximum) {
        char *end = NULL;
        long number;
        struct device_info *di;
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        number = strtol(entry->d_name + 5, &end, 10);
        if (end == entry->d_name + 5 || *end != '\0' || number < 0)
            continue;

        di = &infos[count];
        memset(di, 0, sizeof(*di));
        di->fd = -1;
        snprintf(di->path, sizeof(di->path), "/dev/input/%s", entry->d_name);

        fd = open(di->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (include_inaccessible) {
                di->opened = false;
                count++;
            }
            continue;
        }

        query_device(fd, di);
        count++;
    }

    closedir(directory);

    if (count >= maximum)
        log_warn("more than %d input devices found; ignoring the rest",
                 maximum);

    qsort(infos, (size_t)count, sizeof(*infos), compare_device_info);
    return count;
}

static struct controller *find_controller_by_group(struct daemon *d,
                                                   const char *group)
{
    int i;

    for (i = 0; i < DS4_MAX_CONTROLLERS; i++) {
        struct controller *c = &d->controllers[i];

        if (c->active && !c->dead && strcmp(c->group, group) == 0)
            return c;
    }
    return NULL;
}

static struct controller *allocate_controller(struct daemon *d)
{
    int i;

    for (i = 0; i < DS4_MAX_CONTROLLERS; i++) {
        struct controller *c = &d->controllers[i];
        int j;

        if (c->active)
            continue;

        memset(c, 0, sizeof(*c));
        c->active = true;
        c->owner = d;
        c->generation = ++d->next_generation;
        c->index = i;
        c->primary_fd = -1;
        c->uinput_fd = -1;
        c->test_ff_id = -1;
        for (j = 0; j < FF_MAX_EFFECTS; j++)
            c->real_ff_id[j] = -1;
        return c;
    }
    return NULL;
}

static int count_active_controllers(struct daemon *d)
{
    int i;
    int count = 0;

    for (i = 0; i < DS4_MAX_CONTROLLERS; i++) {
        if (d->controllers[i].active && !d->controllers[i].dead)
            count++;
    }
    return count;
}

static bool controller_has_member(const struct controller *c,
                                  const char *path)
{
    int i;

    for (i = 0; i < DS4_MAX_MEMBERS; i++) {
        if (c->members[i].used && strcmp(c->members[i].path, path) == 0)
            return true;
    }
    return false;
}

static bool device_info_is_touchpad(const struct device_info *di)
{
    return strstr(di->name, "Touchpad") != NULL ||
           strstr(di->name, "Touch Pad") != NULL ||
           strstr(di->name, "touchpad") != NULL ||
           strstr(di->name, "touch pad") != NULL ||
           (bit_test(di->keybits, BTN_LEFT) &&
            bit_test(di->absbits, ABS_MT_POSITION_X));
}

/* ------------------------------------------------------------------------- */
/* Virtual Xbox pad                                                          */
/* ------------------------------------------------------------------------- */

static int uinput_create_device(const struct profile *profile,
                                const struct controller *c)
{
    static const unsigned int keys[] = {
        BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
        BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
        BTN_MODE, BTN_THUMBL, BTN_THUMBR,
    };
    static const struct {
        int code;
        int minimum;
        int maximum;
        int fuzz;
        int flat;
    } axes[] = {
        { ABS_X, -32768, 32767, 16, 128 },
        { ABS_Y, -32768, 32767, 16, 128 },
        { ABS_RX, -32768, 32767, 16, 128 },
        { ABS_RY, -32768, 32767, 16, 128 },
        { ABS_Z, 0, 255, 0, 0 },
        { ABS_RZ, 0, 255, 0, 0 },
        { ABS_HAT0X, -1, 1, 0, 0 },
        { ABS_HAT0Y, -1, 1, 0, 0 },
    };
    struct uinput_setup setup;
    int fd;
    size_t i;

    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log_error("cannot open /dev/uinput: %s", strerror(errno));
        if (errno == ENOENT)
            log_error("hint: load the uinput module (sudo modprobe uinput)");
        else if (errno == EACCES)
            log_error("hint: run as root or grant access to /dev/uinput");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        (c->ff_enabled && ioctl(fd, UI_SET_EVBIT, EV_FF) < 0)) {
        log_error("cannot configure uinput event types: %s", strerror(errno));
        close(fd);
        return -1;
    }

    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, keys[i]) < 0) {
            log_error("UI_SET_KEYBIT(0x%x): %s", keys[i], strerror(errno));
            close(fd);
            return -1;
        }
    }

    for (i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
        struct uinput_abs_setup abs_setup;

        if (ioctl(fd, UI_SET_ABSBIT, axes[i].code) < 0) {
            log_error("UI_SET_ABSBIT(%d): %s", axes[i].code,
                      strerror(errno));
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
            log_error("UI_ABS_SETUP(%d): %s", axes[i].code,
                      strerror(errno));
            close(fd);
            return -1;
        }
    }

    if (c->ff_enabled) {
        unsigned int bit;

        for (bit = 0; bit < FF_CNT; bit++) {
            if (!bit_test(c->src_ffbits, bit))
                continue;
            if (ioctl(fd, UI_SET_FFBIT, bit) < 0)
                log_warn("UI_SET_FFBIT(0x%x): %s", bit, strerror(errno));
        }
    }

    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = profile->vendor;
    setup.id.product = profile->product;
    setup.id.version = profile->version;
    snprintf(setup.name, sizeof(setup.name), "%s", profile->device_name);
    setup.ff_effects_max = c->ff_enabled ? (uint32_t)c->ff_max : 0;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        log_error("UI_DEV_SETUP failed: %s", strerror(errno));
        log_error("hint: kernel 4.5 or newer is required");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_SET_PHYS, "ds4xbox/virtual") < 0)
        log_debug("UI_SET_PHYS failed: %s", strerror(errno));

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        log_error("UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------------- */
/* Event forwarding                                                          */
/* ------------------------------------------------------------------------- */

static void controller_set_epollout(struct controller *c, bool enable)
{
    struct epoll_event event;

    if (c->uinput_fd < 0 || c->epoll_fd < 0)
        return;
    if (c->epollout_enabled == enable)
        return;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | (enable ? EPOLLOUT : 0);
    event.data.ptr = &c->uinput_ref;

    if (epoll_ctl(c->epoll_fd, EPOLL_CTL_MOD, c->uinput_fd, &event) < 0) {
        log_error("controller %d: cannot %s EPOLLOUT on uinput: %s",
                  c->index + 1, enable ? "enable" : "disable",
                  strerror(errno));
        c->dead = true;
        return;
    }

    c->epollout_enabled = enable;
}

static void controller_enqueue_frame(struct controller *c)
{
    struct output_frame *frame;
    unsigned int abs_count = 0;
    int i;

    if (c->outcount <= 0)
        return;

    if (c->outq_count >= DS4_OUTPUT_FRAMES) {
        /*
         * The consumer is not reading fast enough.  Drop the oldest
         * complete report rather than losing the newest input, and
         * resynchronise the full state once the queue drains.
         */
        frame = &c->outq[c->outq_head];
        if (g_log_level >= LOG_LVL_DEBUG) {
            c->diagnostics.frames_dropped++;
            c->diagnostics.abs_dropped += frame->abs_remaining;
        }
        c->outq_head = (c->outq_head + 1) % DS4_OUTPUT_FRAMES;
        c->outq_count--;
        c->outq_dropped++;
        c->outq_overflow = true;
    }

    frame = &c->outq[c->outq_tail];
    memcpy(frame->events, c->outbuf,
           (size_t)c->outcount * sizeof(struct input_event));
    frame->count = (unsigned int)c->outcount;
    frame->abs_remaining = 0;
    frame->report_seq = c->input_report_seq;
    if (g_log_level >= LOG_LVL_DEBUG) {
        for (i = 0; i < c->outcount; i++) {
            if (frame->events[i].type == EV_ABS)
                abs_count++;
        }
        frame->abs_remaining = abs_count;
        c->diagnostics.frames_enqueued++;
        c->diagnostics.abs_enqueued += abs_count;
    }
    c->outq_tail = (c->outq_tail + 1) % DS4_OUTPUT_FRAMES;
    c->outq_count++;
    if (g_log_level >= LOG_LVL_DEBUG &&
        c->outq_count > c->diagnostics.queue_peak)
        c->diagnostics.queue_peak = c->outq_count;
    c->outcount = 0;
}

static void controller_flush_output(struct controller *c)
{
    if (c->dead || c->uinput_fd < 0) {
        c->outcount = 0;
        c->outq_head = 0;
        c->outq_tail = 0;
        c->outq_count = 0;
        c->outq_blocked = false;
        c->epollout_enabled = false;
        return;
    }

    while (c->outq_count > 0) {
        struct output_frame *frame = &c->outq[c->outq_head];
        size_t bytes = (size_t)frame->count * sizeof(struct input_event);
        ssize_t written;

        do {
            written = write(c->uinput_fd, frame->events, bytes);
        } while (written < 0 && errno == EINTR);

        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!c->outq_blocked) {
                    log_debug("controller %d: uinput busy; queueing input",
                              c->index + 1);
                    c->outq_blocked = true;
                }
                controller_set_epollout(c, true);
                return;
            }

            log_error("controller %d: uinput write failed: %s",
                      c->index + 1, strerror(errno));
            c->dead = true;
            c->outcount = 0;
            c->outq_head = 0;
            c->outq_tail = 0;
            c->outq_count = 0;
            return;
        }

        if (written == 0) {
            c->outq_blocked = true;
            controller_set_epollout(c, true);
            return;
        }

        if (c->owner->trace_records != NULL) {
            size_t done = (size_t)written / sizeof(struct input_event);
            uint64_t written_ns = monotonic_ns();
            size_t i;

            if (done > frame->count)
                done = frame->count;
            for (i = 0; i < done; i++) {
                daemon_trace_add(c->owner, TRACE_OUTPUT, c,
                                frame->report_seq, written_ns, written_ns,
                                &frame->events[i]);
            }
        }

        if ((size_t)written < bytes) {
            size_t done = (size_t)written / sizeof(struct input_event);

            if (done == 0 ||
                (size_t)written % sizeof(struct input_event) != 0 ||
                done > frame->count) {
                log_error("controller %d: short uinput write (%zd/%zu)",
                          c->index + 1, written, bytes);
                c->dead = true;
                c->outcount = 0;
                c->outq_head = 0;
                c->outq_tail = 0;
                c->outq_count = 0;
                return;
            }

            if (done < frame->count) {
                if (g_log_level >= LOG_LVL_DEBUG) {
                    unsigned int abs_done = 0;
                    size_t i;

                    for (i = 0; i < done; i++) {
                        if (frame->events[i].type == EV_ABS)
                            abs_done++;
                    }
                    c->diagnostics.abs_written += abs_done;
                    frame->abs_remaining -= abs_done;
                }
                memmove(frame->events, frame->events + done,
                        (frame->count - done) * sizeof(struct input_event));
                frame->count -= (unsigned int)done;
                c->outq_blocked = true;
                controller_set_epollout(c, true);
                return;
            }
        }

        if (g_log_level >= LOG_LVL_DEBUG) {
            c->diagnostics.frames_written++;
            c->diagnostics.abs_written += frame->abs_remaining;
            frame->abs_remaining = 0;
        }
        c->outq_head = (c->outq_head + 1) % DS4_OUTPUT_FRAMES;
        c->outq_count--;
    }

    if (c->outq_blocked) {
        c->outq_blocked = false;
        controller_set_epollout(c, false);
    } else if (c->epollout_enabled) {
        controller_set_epollout(c, false);
    }

    if (c->dead)
        return;

    if (c->outq_overflow) {
        c->outq_overflow = false;
        c->resync_pending = true;
        log_warn("controller %d: uinput queue overflow; dropped %u report(s)",
                 c->index + 1, c->outq_dropped);
        c->outq_dropped = 0;
    }
}

static void controller_emit(struct controller *c, unsigned int type,
                            unsigned int code, int value)
{
    struct input_event *event;
    int i;

    if (c->dead || c->uinput_fd < 0)
        return;

    /*
     * Several controllers send more than one value for the same axis in
     * a single report.  Only the last value matters, and coalescing it
     * keeps the uinput queue small.
     */
    if (type == EV_ABS) {
        for (i = 0; i < c->outcount; i++) {
            if (c->outbuf[i].type == EV_ABS &&
                c->outbuf[i].code == code) {
                c->outbuf[i].value = value;
                if (g_log_level >= LOG_LVL_DEBUG)
                    c->diagnostics.abs_coalesced++;
                return;
            }
        }
    }

    if (c->outcount >= DS4_OUTPUT_EVENTS) {
        controller_enqueue_frame(c);
        controller_flush_output(c);
    }

    if (c->dead || c->outcount >= DS4_OUTPUT_EVENTS)
        return;

    event = &c->outbuf[c->outcount++];
    memset(event, 0, sizeof(*event));
    event->type = (uint16_t)type;
    event->code = (uint16_t)code;
    event->value = value;
}

static void controller_emit_syn(struct controller *c)
{
    controller_emit(c, EV_SYN, SYN_REPORT, 0);
    controller_enqueue_frame(c);
    controller_flush_output(c);
}

static void controller_update_dpad(struct controller *c)
{
    int x = 0;
    int y = 0;

    if ((c->dpad_state & DPAD_RIGHT) && !(c->dpad_state & DPAD_LEFT))
        x = 1;
    else if ((c->dpad_state & DPAD_LEFT) && !(c->dpad_state & DPAD_RIGHT))
        x = -1;

    if ((c->dpad_state & DPAD_DOWN) && !(c->dpad_state & DPAD_UP))
        y = 1;
    else if ((c->dpad_state & DPAD_UP) && !(c->dpad_state & DPAD_DOWN))
        y = -1;

    if (x != c->last_hat_x) {
        controller_emit(c, EV_ABS, ABS_HAT0X, x);
        c->last_hat_x = x;
    }
    if (y != c->last_hat_y) {
        controller_emit(c, EV_ABS, ABS_HAT0Y, y);
        c->last_hat_y = y;
    }
}

static void controller_handle_dpad_button(struct controller *c,
                                          unsigned int code, int value)
{
    unsigned int bit = 0;

    switch (code) {
    case BTN_DPAD_UP:
        bit = DPAD_UP;
        break;
    case BTN_DPAD_DOWN:
        bit = DPAD_DOWN;
        break;
    case BTN_DPAD_LEFT:
        bit = DPAD_LEFT;
        break;
    case BTN_DPAD_RIGHT:
        bit = DPAD_RIGHT;
        break;
    default:
        return;
    }

    if (value)
        c->dpad_state |= bit;
    else
        c->dpad_state &= ~bit;

    controller_update_dpad(c);
}

static void controller_resync(struct controller *c)
{
    static const unsigned int source_buttons[] = {
        BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
        BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
        BTN_MODE, BTN_THUMBL, BTN_THUMBR, BTN_LEFT,
    };
    static const unsigned int source_axes[] = {
        ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ,
    };
    unsigned long keybits[DS4_BITS_TO_LONGS(KEY_CNT)];
    size_t i;

    if (c->primary_fd < 0 || c->uinput_fd < 0)
        return;

    memset(keybits, 0, sizeof(keybits));
    if (ioctl(c->primary_fd, EVIOCGKEY(sizeof(keybits)), keybits) < 0) {
        if (errno == ENODEV)
            c->dead = true;
        log_debug("controller %d: EVIOCGKEY failed: %s",
                  c->index + 1, strerror(errno));
        return;
    }

    for (i = 0; i < sizeof(source_buttons) / sizeof(source_buttons[0]); i++) {
        int destination = map_button_code(source_buttons[i]);

        if (destination >= 0)
            controller_emit(c, EV_KEY, (unsigned int)destination,
                            bit_test(keybits, source_buttons[i]) ? 1 : 0);
    }

    if (!c->has_trigger_axes) {
        controller_emit(c, EV_ABS, ABS_Z,
                        bit_test(keybits, BTN_TL2) ? 255 : 0);
        controller_emit(c, EV_ABS, ABS_RZ,
                        bit_test(keybits, BTN_TR2) ? 255 : 0);
    }

    if (c->has_hat) {
        int code;

        for (code = ABS_HAT0X; code <= ABS_HAT0Y; code++) {
            struct input_absinfo current;
            int destination;
            int value;

            if (!c->abs[code].present)
                continue;
            if (ioctl(c->primary_fd, EVIOCGABS(code), &current) < 0)
                continue;

            destination = map_axis_code((unsigned int)code,
                                        c->swap_sticks_triggers);
            if (destination < 0)
                continue;

            value = map_hat_value(current.value, &c->abs[code].info);
            controller_emit(c, EV_ABS, (unsigned int)destination, value);
            if (destination == ABS_HAT0X)
                c->last_hat_x = value;
            else if (destination == ABS_HAT0Y)
                c->last_hat_y = value;
        }
    } else {
        c->dpad_state = 0;
        if (bit_test(keybits, BTN_DPAD_UP))
            c->dpad_state |= DPAD_UP;
        if (bit_test(keybits, BTN_DPAD_DOWN))
            c->dpad_state |= DPAD_DOWN;
        if (bit_test(keybits, BTN_DPAD_LEFT))
            c->dpad_state |= DPAD_LEFT;
        if (bit_test(keybits, BTN_DPAD_RIGHT))
            c->dpad_state |= DPAD_RIGHT;
        controller_update_dpad(c);
    }

    for (i = 0; i < sizeof(source_axes) / sizeof(source_axes[0]); i++) {
        unsigned int code = source_axes[i];
        struct input_absinfo current;
        int destination;
        int kind;
        int value = 0;

        if (!c->abs[code].present)
            continue;
        if (ioctl(c->primary_fd, EVIOCGABS((int)code), &current) < 0)
            continue;

        destination = map_axis_code(code, c->swap_sticks_triggers);
        kind = map_axis_kind((int)code, c->swap_sticks_triggers);
        if (destination < 0 || kind == AXIS_NONE)
            continue;

        switch (kind) {
        case AXIS_STICK:
            value = map_stick_value(current.value, &c->abs[code].info);
            break;
        case AXIS_TRIGGER:
            value = map_trigger_value(current.value, &c->abs[code].info);
            break;
        case AXIS_HAT:
            value = map_hat_value(current.value, &c->abs[code].info);
            break;
        default:
            continue;
        }

        controller_emit(c, EV_ABS, (unsigned int)destination, value);
        if (destination == ABS_HAT0X)
            c->last_hat_x = value;
        else if (destination == ABS_HAT0Y)
            c->last_hat_y = value;
    }

    controller_emit_syn(c);
}

static void controller_handle_primary_event(struct controller *c,
                                            const struct input_event *event)
{
    switch (event->type) {
    case EV_KEY:
        if (is_dpad_button(event->code)) {
            if (!c->has_hat)
                controller_handle_dpad_button(c, event->code, event->value);
            return;
        }

        /* Some controllers expose the touchpad click on the primary node. */
        if (event->code == BTN_LEFT) {
            controller_emit(c, EV_KEY, BTN_SELECT,
                            event->value ? 1 : 0);
            return;
        }

        if (event->code == BTN_TL2 || event->code == BTN_TR2) {
            if (!c->has_trigger_axes) {
                controller_emit(c, EV_ABS,
                                event->code == BTN_TL2 ? ABS_Z : ABS_RZ,
                                event->value ? 255 : 0);
            }
            return;
        }

        {
            int destination = map_button_code(event->code);

            if (destination >= 0)
                controller_emit(c, EV_KEY, (unsigned int)destination,
                                event->value ? 1 : 0);
        }
        break;

    case EV_ABS:
        {
            int destination;
            int kind;
            int value = 0;

            if (event->code >= ABS_CNT)
                return;
            if (!c->abs[event->code].present)
                return;

            destination = map_axis_code(event->code,
                                        c->swap_sticks_triggers);
            kind = map_axis_kind((int)event->code,
                                 c->swap_sticks_triggers);
            if (destination < 0 || kind == AXIS_NONE)
                return;

            switch (kind) {
            case AXIS_STICK:
                value = map_stick_value(event->value,
                                        &c->abs[event->code].info);
                break;
            case AXIS_TRIGGER:
                value = map_trigger_value(event->value,
                                          &c->abs[event->code].info);
                break;
            case AXIS_HAT:
                value = map_hat_value(event->value,
                                      &c->abs[event->code].info);
                break;
            default:
                return;
            }

            controller_emit(c, EV_ABS, (unsigned int)destination, value);
            if (destination == ABS_HAT0X)
                c->last_hat_x = value;
            else if (destination == ABS_HAT0Y)
                c->last_hat_y = value;
        }
        break;

    case EV_SYN:
        if (event->code == SYN_REPORT)
            controller_emit_syn(c);
        break;

    default:
        break;
    }
}

static void controller_handle_secondary_event(struct controller *c,
                                              struct member *member,
                                              const struct input_event *event)
{
    if (!member->touchpad)
        return;

    if (event->type == EV_KEY && event->code == BTN_LEFT) {
        controller_emit(c, EV_KEY, BTN_SELECT, event->value ? 1 : 0);
        member->dirty = true;
    } else if (event->type == EV_SYN && event->code == SYN_REPORT) {
        if (member->dirty) {
            controller_emit_syn(c);
            member->dirty = false;
        }
    }
}

static const char *controller_member_role(const struct member *member)
{
    if (member->primary)
        return "gamepad";
    if (member->touchpad)
        return "touchpad";
    return "ignored secondary";
}

static void controller_resync_member_state(struct controller *c,
                                           struct member *member)
{
    unsigned long keybits[DS4_BITS_TO_LONGS(KEY_CNT)];

    if (member->primary) {
        controller_resync(c);
        return;
    }

    if (!member->touchpad)
        return;

    memset(keybits, 0, sizeof(keybits));
    if (ioctl(member->fd, EVIOCGKEY(sizeof(keybits)), keybits) < 0) {
        log_debug("controller %d: EVIOCGKEY failed for %s: %s",
                  c->index + 1, member->path, strerror(errno));
        return;
    }

    controller_emit(c, EV_KEY, BTN_SELECT,
                    bit_test(keybits, BTN_LEFT) ? 1 : 0);
    controller_emit_syn(c);
}

static void controller_track_input_event(struct controller *c,
                                         struct member *member,
                                         const struct input_event *event)
{
    struct controller_diagnostics *stats = &c->diagnostics;

    if (g_log_level < LOG_LVL_DEBUG)
        return;

    member->stats_events++;
    if (event->type == EV_ABS)
        member->stats_abs_events++;
    if (event->type == EV_SYN && event->code == SYN_REPORT)
        member->stats_reports++;
    if (event->type == EV_SYN && event->code == SYN_DROPPED)
        member->stats_syn_dropped++;

    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        stats->syn_dropped++;
        if (member->primary)
            stats->input_abs_current = 0;
        return;
    }

    if (event->type == EV_SYN && event->code == SYN_REPORT &&
        member->syncing) {
        member->stats_recoveries++;
        stats->input_recovery_reports++;
        return;
    }

    if (!member->primary)
        return;

    if (event->type == EV_ABS) {
        stats->input_abs_seen++;
        if (member->syncing)
            stats->input_abs_discarded++;
        else
            stats->input_abs_current++;
    }

    if (event->type != EV_SYN || event->code != SYN_REPORT)
        return;

    {
        uint64_t count = stats->input_abs_current;

        if (stats->input_reports == 0 || count < stats->input_abs_min)
            stats->input_abs_min = count;
        if (count > stats->input_abs_max)
            stats->input_abs_max = count;
        stats->input_abs_sum += count;
        stats->input_reports++;
    }
    stats->input_abs_current = 0;
}

static void controller_log_diagnostics(struct controller *c)
{
    struct controller_diagnostics *stats = &c->diagnostics;
    uint64_t now;
    uint64_t elapsed;
    double abs_per_report;

    if (g_log_level < LOG_LVL_DEBUG)
        return;

    now = monotonic_ms();
    if (stats->last_log_ms == 0) {
        stats->last_log_ms = now;
        return;
    }
    elapsed = now - stats->last_log_ms;
    if (elapsed < 1000)
        return;

    abs_per_report = stats->input_reports > 0 ?
        (double)stats->input_abs_sum / (double)stats->input_reports : 0.0;
    log_debug("controller %d: stats %llums: input reports=%llu "
              "ABS/report avg=%.2f min=%llu max=%llu seen=%llu "
              "discarded=%llu input lag max=%lluus coalesced=%llu; "
              "output frames "
              "enqueued=%llu written=%llu dropped=%llu "
              "(ABS enqueued=%llu written=%llu dropped=%llu), "
              "queue=%u/%u peak=%llu; SYN_DROPPED=%llu "
              "recovery_reports=%llu",
              c->index + 1,
              (unsigned long long)elapsed,
              (unsigned long long)stats->input_reports,
              abs_per_report,
              (unsigned long long)(stats->input_reports > 0 ?
                                   stats->input_abs_min : 0),
              (unsigned long long)stats->input_abs_max,
              (unsigned long long)stats->input_abs_seen,
              (unsigned long long)stats->input_abs_discarded,
              (unsigned long long)stats->input_lag_max_us,
              (unsigned long long)stats->abs_coalesced,
              (unsigned long long)stats->frames_enqueued,
              (unsigned long long)stats->frames_written,
              (unsigned long long)stats->frames_dropped,
              (unsigned long long)stats->abs_enqueued,
              (unsigned long long)stats->abs_written,
              (unsigned long long)stats->abs_dropped,
              c->outq_count, DS4_OUTPUT_FRAMES,
              (unsigned long long)stats->queue_peak,
              (unsigned long long)stats->syn_dropped,
              (unsigned long long)stats->input_recovery_reports);

    for (size_t i = 0; i < DS4_MAX_MEMBERS; i++) {
        struct member *member = &c->members[i];

        if (!member->used)
            continue;
        if (member->stats_events > 0 || member->stats_syn_dropped > 0) {
            log_debug("controller %d: node %s (%s; %s): events=%llu "
                      "ABS=%llu reports=%llu SYN_DROPPED=%llu recovered=%llu",
                      c->index + 1, member->path, member->name,
                      controller_member_role(member),
                      (unsigned long long)member->stats_events,
                      (unsigned long long)member->stats_abs_events,
                      (unsigned long long)member->stats_reports,
                      (unsigned long long)member->stats_syn_dropped,
                      (unsigned long long)member->stats_recoveries);
        }
        member->stats_events = 0;
        member->stats_abs_events = 0;
        member->stats_reports = 0;
        member->stats_syn_dropped = 0;
        member->stats_recoveries = 0;
    }

    memset(stats, 0, sizeof(*stats));
    stats->last_log_ms = now;
}

/* ------------------------------------------------------------------------- */
/* Force feedback                                                            */
/* ------------------------------------------------------------------------- */

static void controller_ff_upload(struct controller *c, int request_id)
{
    struct uinput_ff_upload upload;
    int virtual_id;
    int result = 0;

    memset(&upload, 0, sizeof(upload));
    upload.request_id = request_id;

    if (ioctl(c->uinput_fd, UI_BEGIN_FF_UPLOAD, &upload) < 0) {
        log_debug("controller %d: UI_BEGIN_FF_UPLOAD failed: %s",
                  c->index + 1, strerror(errno));
        return;
    }

    virtual_id = upload.effect.id;

    if (!c->ff_enabled || c->primary_fd < 0 ||
        virtual_id < 0 || virtual_id >= c->ff_max) {
        result = -EINVAL;
    } else {
        struct ff_effect real;
        int new_id = -1;

        memcpy(&real, &upload.effect, sizeof(real));
        real.id = -1;

        if (ioctl(c->primary_fd, EVIOCSFF, &real) == 0) {
            new_id = real.id;
        } else if (errno == ENOSPC && c->real_ff_id[virtual_id] >= 0) {
            /*
             * The real device is out of effect slots.  Erase the old
             * version of this effect and try once more.
             */
            ioctl(c->primary_fd, EVIOCRMFF, c->real_ff_id[virtual_id]);
            c->real_ff_id[virtual_id] = -1;
            real.id = -1;
            if (ioctl(c->primary_fd, EVIOCSFF, &real) == 0)
                new_id = real.id;
        }

        if (new_id >= 0) {
            if (c->real_ff_id[virtual_id] >= 0)
                ioctl(c->primary_fd, EVIOCRMFF,
                      c->real_ff_id[virtual_id]);
            c->real_ff_id[virtual_id] = new_id;
            result = 0;
            log_debug("controller %d: FF effect %d -> real %d",
                      c->index + 1, virtual_id, new_id);
        } else {
            result = -errno;
            log_debug("controller %d: FF upload failed: %s",
                      c->index + 1, strerror(errno));
        }
    }

    upload.retval = result;
    if (ioctl(c->uinput_fd, UI_END_FF_UPLOAD, &upload) < 0)
        log_debug("controller %d: UI_END_FF_UPLOAD failed: %s",
                  c->index + 1, strerror(errno));
}

static void controller_ff_erase(struct controller *c, int request_id)
{
    struct uinput_ff_erase erase;
    int result = 0;

    memset(&erase, 0, sizeof(erase));
    erase.request_id = request_id;

    if (ioctl(c->uinput_fd, UI_BEGIN_FF_ERASE, &erase) < 0) {
        log_debug("controller %d: UI_BEGIN_FF_ERASE failed: %s",
                  c->index + 1, strerror(errno));
        return;
    }

    if (erase.effect_id < (uint32_t)c->ff_max &&
        c->real_ff_id[erase.effect_id] >= 0) {
        if (c->primary_fd < 0) {
            c->real_ff_id[erase.effect_id] = -1;
        } else if (ioctl(c->primary_fd, EVIOCRMFF,
                         c->real_ff_id[erase.effect_id]) < 0 &&
                   errno != ENODEV) {
            /*
             * Keep the mapping so controller_destroy() can try again
             * when the virtual device is closed.
             */
            result = -errno;
        } else {
            c->real_ff_id[erase.effect_id] = -1;
        }
    }

    erase.retval = result;
    if (ioctl(c->uinput_fd, UI_END_FF_ERASE, &erase) < 0)
        log_debug("controller %d: UI_END_FF_ERASE failed: %s",
                  c->index + 1, strerror(errno));
}

static void controller_test_rumble(struct controller *c)
{
    struct ff_effect effect;
    struct input_event play;
    int id;

    if (!c->ff_enabled || c->primary_fd < 0) {
        log_warn("controller %d: cannot test rumble: force feedback "
                 "is not available", c->index + 1);
        return;
    }
    if (c->test_ff_id >= 0)
        return;

    memset(&effect, 0, sizeof(effect));
    effect.type = FF_RUMBLE;
    effect.id = -1;
    effect.replay.length = 700;
    effect.replay.delay = 0;
    effect.u.rumble.strong_magnitude = 0xffff;
    effect.u.rumble.weak_magnitude = 0xffff;

    if (ioctl(c->primary_fd, EVIOCSFF, &effect) < 0) {
        log_warn("controller %d: cannot upload rumble test: %s",
                 c->index + 1, strerror(errno));
        return;
    }

    id = effect.id;
    if (id < 0) {
        log_warn("controller %d: rumble test got invalid effect id",
                 c->index + 1);
        return;
    }

    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = (uint16_t)id;
    play.value = 1;

    if (write(c->primary_fd, &play, sizeof(play)) < 0) {
        log_warn("controller %d: cannot play rumble test: %s",
                 c->index + 1, strerror(errno));
        ioctl(c->primary_fd, EVIOCRMFF, id);
        return;
    }

    c->test_ff_id = id;
    c->test_ff_until = monotonic_ms() + 1000;
    log_info("controller %d: rumble test playing (effect %d)",
             c->index + 1, id);
}

static void controller_ff_play(struct controller *c,
                               const struct input_event *event)
{
    struct input_event output;
    int real_id;

    if (c->primary_fd < 0)
        return;

    memset(&output, 0, sizeof(output));
    output.type = EV_FF;

    if (event->code == FF_GAIN || event->code == FF_AUTOCENTER) {
        output.code = event->code;
        output.value = event->value;
        if (write(c->primary_fd, &output, sizeof(output)) < 0 &&
            errno != EAGAIN) {
            log_debug("controller %d: FF control write failed: %s",
                      c->index + 1, strerror(errno));
        }
        return;
    }

    if (event->code >= c->ff_max)
        return;

    real_id = c->real_ff_id[event->code];
    if (real_id < 0)
        return;

    output.code = (uint16_t)real_id;
    output.value = event->value;
    if (write(c->primary_fd, &output, sizeof(output)) < 0 &&
        errno != EAGAIN) {
        log_debug("controller %d: FF play write failed: %s",
                  c->index + 1, strerror(errno));
    }
}

/* ------------------------------------------------------------------------- */
/* Controller lifecycle                                                      */
/* ------------------------------------------------------------------------- */

static void controller_remove_member(struct daemon *d, struct controller *c,
                                     int slot)
{
    struct member *member = &c->members[slot];

    if (!member->used)
        return;

    epoll_ctl(d->epoll_fd, EPOLL_CTL_DEL, member->fd, NULL);
    if (member->grabbed)
        ioctl(member->fd, EVIOCGRAB, 0);
    close(member->fd);

    member->used = false;
    member->fd = -1;
    member->dirty = false;
    if (c->member_count > 0)
        c->member_count--;

    if (member->primary) {
        c->primary_fd = -1;
        c->dead = true;
    }

    if (c->member_count == 0)
        c->dead = true;
}

static void controller_destroy(struct daemon *d, struct controller *c)
{
    int i;

    if (!c->active)
        return;

    if (c->announced)
        log_info("controller %d: removed", c->index + 1);

    if (c->primary_fd >= 0) {
        if (c->test_ff_id >= 0) {
            ioctl(c->primary_fd, EVIOCRMFF, c->test_ff_id);
            c->test_ff_id = -1;
        }
        for (i = 0; i < c->ff_max; i++) {
            if (c->real_ff_id[i] >= 0) {
                ioctl(c->primary_fd, EVIOCRMFF, c->real_ff_id[i]);
                c->real_ff_id[i] = -1;
            }
        }
    }

    if (c->uinput_fd >= 0) {
        epoll_ctl(d->epoll_fd, EPOLL_CTL_DEL, c->uinput_fd, NULL);
        ioctl(c->uinput_fd, UI_DEV_DESTROY);
        close(c->uinput_fd);
        c->uinput_fd = -1;
    }

    for (i = 0; i < DS4_MAX_MEMBERS; i++) {
        struct member *member = &c->members[i];

        if (!member->used)
            continue;

        epoll_ctl(d->epoll_fd, EPOLL_CTL_DEL, member->fd, NULL);
        if (member->grabbed)
            ioctl(member->fd, EVIOCGRAB, 0);
        close(member->fd);
        member->used = false;
        member->fd = -1;
    }

    c->member_count = 0;
    c->primary_fd = -1;
    c->outcount = 0;
    c->outq_head = 0;
    c->outq_tail = 0;
    c->outq_count = 0;
    c->outq_blocked = false;
    c->outq_overflow = false;
    c->resync_pending = false;
    c->epollout_enabled = false;
    c->active = false;
    c->dead = false;
}

static bool controller_add_member(struct daemon *d, struct controller *c,
                                  struct device_info *di, bool primary)
{
    struct member *member;
    struct epoll_event event;
    clockid_t clock_id = CLOCK_MONOTONIC;
    int slot = -1;
    int fd = di->fd;
    int i;

    di->fd = -1;
    if (fd < 0)
        return false;

    for (i = 0; i < DS4_MAX_MEMBERS; i++) {
        if (!c->members[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        log_warn("controller %d: too many event nodes", c->index + 1);
        close(fd);
        return false;
    }

    member = &c->members[slot];
    memset(member, 0, sizeof(*member));
    member->used = true;
    member->primary = primary;
    member->fd = fd;
    member->touchpad = device_info_is_touchpad(di);
    snprintf(member->path, sizeof(member->path), "%s", di->path);
    snprintf(member->name, sizeof(member->name), "%s", di->name);

    if (ioctl(fd, EVIOCSCLOCKID, &clock_id) == 0)
        member->monotonic_clock = true;
    else
        log_debug("controller %d: cannot use monotonic timestamps on %s: %s",
                  c->index + 1, di->path, strerror(errno));

    if (!d->no_grab) {
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            log_error("controller %d: cannot grab %s: %s%s",
                      c->index + 1, di->path, strerror(errno),
                      errno == EBUSY ?
                      " (another program has grabbed it)" : "");
            close(fd);
            member->used = false;
            member->fd = -1;
            return false;
        }
        member->grabbed = true;
    } else {
        log_warn("controller %d: not grabbing %s (--no-grab)",
                 c->index + 1, di->path);
    }

    member->ref.type = REF_MEMBER;
    member->ref.member = slot;
    member->ref.generation = c->generation;
    member->ref.controller = c;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = &member->ref;

    if (epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        log_error("controller %d: epoll_ctl(%s): %s",
                  c->index + 1, di->path, strerror(errno));
        if (member->grabbed)
            ioctl(fd, EVIOCGRAB, 0);
        close(fd);
        member->used = false;
        member->fd = -1;
        return false;
    }

    if (primary)
        c->primary_fd = fd;
    c->member_count++;

    if (primary)
        log_info("controller %d: grabbed %s (gamepad)",
                 c->index + 1, di->path);
    else
        log_debug("controller %d: grabbed %s", c->index + 1, di->path);

    return true;
}

static struct controller *controller_create(struct daemon *d,
                                            struct device_info *primary,
                                            struct device_info *infos,
                                            int *members, int member_count,
                                            int primary_index)
{
    struct controller *c;
    struct epoll_event event;
    int i;

    c = allocate_controller(d);
    if (c == NULL) {
        log_error("too many controllers (maximum %d)",
                  DS4_MAX_CONTROLLERS);
        goto close_remaining;
    }

    copy_string(c->group, sizeof(c->group), primary->group);
    copy_string(c->name, sizeof(c->name), primary->name);
    copy_string(c->uniq, sizeof(c->uniq), primary->uniq);
    c->id = primary->id;
    c->swap_sticks_triggers = d->swap_sticks_triggers;
    c->epoll_fd = d->epoll_fd;

    for (i = 0; i < ABS_CNT; i++) {
        struct input_absinfo info;

        if (!bit_test(primary->absbits, (unsigned int)i))
            continue;
        if (ioctl(primary->fd, EVIOCGABS(i), &info) == 0) {
            c->abs[i].present = true;
            c->abs[i].info = info;
        }
    }

    c->has_hat = bit_test(primary->absbits, ABS_HAT0X) &&
                 bit_test(primary->absbits, ABS_HAT0Y);
    c->has_trigger_axes = bit_test(primary->absbits, ABS_Z) &&
                          bit_test(primary->absbits, ABS_RZ);
    memcpy(c->src_ffbits, primary->ffbits, sizeof(c->src_ffbits));

    if (!d->no_ff && bit_test(primary->ffbits, FF_RUMBLE)) {
        int effects = 0;

        if (ioctl(primary->fd, EVIOCGEFFECTS, &effects) < 0 || effects <= 0) {
            /*
             * A few HID drivers do not report the effect count even
             * though FF_RUMBLE is present.  Advertise a sane default
             * instead of disabling rumble completely.
             */
            effects = 16;
            log_debug("controller %d: EVIOCGEFFECTS unavailable; "
                      "assuming %d effect slots", c->index + 1, effects);
        }
        if (effects > FF_MAX_EFFECTS)
            effects = FF_MAX_EFFECTS;
        c->ff_enabled = true;
        c->ff_max = effects;
        log_info("controller %d: force feedback enabled (%d effect slots)",
                 c->index + 1, c->ff_max);
    } else if (!d->no_ff) {
        log_info("controller %d: force feedback not supported by this device",
                 c->index + 1);
    }

    for (i = 0; i < member_count; i++) {
        struct device_info *di = &infos[members[i]];
        bool is_primary = (members[i] == primary_index);

        if (di->fd < 0)
            continue;

        /* Only the gamepad and touchpad click feed the Xbox mapping. */
        if (!is_primary && !device_info_is_touchpad(di)) {
            log_debug("controller %d: ignoring unused secondary node %s (%s)",
                      c->index + 1, di->path, di->name);
            close(di->fd);
            di->fd = -1;
            continue;
        }

        if (!controller_add_member(d, c, di, is_primary) && is_primary) {
            log_error("controller %d: cannot grab primary gamepad %s",
                      c->index + 1, di->path);
            goto fail_controller;
        }
    }

    if (c->primary_fd < 0) {
        log_error("controller %d: primary gamepad was not added",
                  c->index + 1);
        goto fail_controller;
    }

    c->uinput_fd = uinput_create_device(&d->profile, c);
    if (c->uinput_fd < 0) {
        log_error("controller %d: cannot create virtual %s",
                  c->index + 1, d->profile.device_name);
        goto fail_controller;
    }

    c->uinput_ref.type = REF_UINPUT;
    c->uinput_ref.member = -1;
    c->uinput_ref.generation = c->generation;
    c->uinput_ref.controller = c;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = &c->uinput_ref;

    if (epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, c->uinput_fd, &event) < 0) {
        log_error("controller %d: cannot add uinput to epoll: %s",
                  c->index + 1, strerror(errno));
        goto fail_controller;
    }

    controller_resync(c);
    if (c->dead) {
        controller_destroy(d, c);
        goto close_remaining;
    }

    if (d->test_rumble)
        controller_test_rumble(c);

    c->announced = true;
    log_info("controller %d: %s (%04x:%04x) -> virtual %s",
             c->index + 1, c->name, c->id.vendor, c->id.product,
             d->profile.device_name);
    log_debug("controller %d: group %s, %d event node(s), rumble %s",
              c->index + 1, c->group, c->member_count,
              c->ff_enabled ? "enabled" : "disabled");

    return c;

fail_controller:
    controller_destroy(d, c);

close_remaining:
    for (i = 0; i < member_count; i++) {
        struct device_info *di = &infos[members[i]];

        if (di->fd >= 0) {
            close(di->fd);
            di->fd = -1;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Event loop helpers                                                        */
/* ------------------------------------------------------------------------- */

static void controller_handle_member_readable(struct daemon *d,
                                              struct controller *c, int slot)
{
    struct member *member;
    struct input_event events[64];
    uint64_t batch_now_us = 0;
    uint64_t batch_now_ns = 0;
    ssize_t bytes;
    size_t count;
    size_t i;

    if (slot < 0 || slot >= DS4_MAX_MEMBERS)
        return;

    member = &c->members[slot];
    if (!member->used || member->fd < 0)
        return;

    for (;;) {
        bytes = read(member->fd, events, sizeof(events));
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return;
            if (errno == ENODEV || errno == ENXIO) {
                if (member->primary)
                    log_info("controller %d: %s disconnected",
                             c->index + 1, member->path);
                else
                    log_debug("controller %d: %s disconnected",
                              c->index + 1, member->path);
                controller_remove_member(d, c, slot);
                return;
            }
            log_error("controller %d: read %s: %s",
                      c->index + 1, member->path, strerror(errno));
            controller_remove_member(d, c, slot);
            return;
        }

        if (bytes == 0) {
            controller_remove_member(d, c, slot);
            return;
        }

        count = (size_t)bytes / sizeof(events[0]);
        batch_now_us = 0;
        batch_now_ns = 0;
        if (member->primary &&
            ((g_log_level >= LOG_LVL_DEBUG && member->monotonic_clock) ||
             d->trace_records != NULL)) {
            batch_now_ns = monotonic_ns();
            if (batch_now_ns != 0)
                batch_now_us = batch_now_ns / 1000u;
        }
        for (i = 0; i < count; i++) {
            if (c->dead)
                return;

            if (batch_now_us != 0 && member->monotonic_clock) {
                uint64_t event_us =
                    (uint64_t)events[i].time.tv_sec * 1000000u +
                    (uint64_t)events[i].time.tv_usec;

                if (batch_now_us > event_us &&
                    batch_now_us - event_us >
                        c->diagnostics.input_lag_max_us)
                    c->diagnostics.input_lag_max_us =
                        batch_now_us - event_us;
            }

            if (member->primary) {
                uint64_t source_ns = 0;

                if (!c->input_report_open) {
                    c->input_report_seq++;
                    c->input_report_open = true;
                }
                if (member->monotonic_clock) {
                    source_ns = (uint64_t)events[i].time.tv_sec *
                                1000000000u +
                                (uint64_t)events[i].time.tv_usec * 1000u;
                }
                daemon_trace_add(d, TRACE_INPUT, c, c->input_report_seq,
                                source_ns, batch_now_ns, &events[i]);
                if (events[i].type == EV_SYN &&
                    events[i].code == SYN_REPORT)
                    c->input_report_open = false;
            }

            controller_track_input_event(c, member, &events[i]);

            if (events[i].type == EV_SYN &&
                events[i].code == SYN_DROPPED) {
                if (!member->syncing) {
                    log_warn("controller %d: input event queue overrun on "
                             "%s (%s; %s); discarding events through the "
                             "next report",
                             c->index + 1, member->path,
                             member->name,
                             controller_member_role(member));
                }
                member->syncing = true;
                member->dirty = false;
                if (member->primary)
                    c->outcount = 0;
                continue;
            }

            if (member->syncing) {
                if (events[i].type == EV_SYN &&
                    events[i].code == SYN_REPORT) {
                    member->syncing = false;
                    controller_resync_member_state(c, member);
                }
                continue;
            }

            if (member->primary)
                controller_handle_primary_event(c, &events[i]);
            else
                controller_handle_secondary_event(c, member, &events[i]);
        }

        if ((size_t)bytes < sizeof(events))
            return;
    }
}

static void controller_handle_uinput_readable(struct daemon *d,
                                              struct controller *c)
{
    struct input_event events[16];
    ssize_t bytes;
    size_t count;
    size_t i;

    (void)d;

    if (c->uinput_fd < 0)
        return;

    for (;;) {
        bytes = read(c->uinput_fd, events, sizeof(events));
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return;
            if (errno != ENODEV)
                log_error("controller %d: read uinput: %s",
                          c->index + 1, strerror(errno));
            c->dead = true;
            return;
        }

        if (bytes == 0) {
            c->dead = true;
            return;
        }

        count = (size_t)bytes / sizeof(events[0]);
        for (i = 0; i < count; i++) {
            if (events[i].type == EV_UINPUT) {
                if (events[i].code == UI_FF_UPLOAD)
                    controller_ff_upload(c, events[i].value);
                else if (events[i].code == UI_FF_ERASE)
                    controller_ff_erase(c, events[i].value);
            } else if (events[i].type == EV_FF) {
                controller_ff_play(c, &events[i]);
            }
        }

        if ((size_t)bytes < sizeof(events))
            return;
    }
}

static void daemon_reap(struct daemon *d)
{
    int i;

    for (i = 0; i < DS4_MAX_CONTROLLERS; i++) {
        if (d->controllers[i].active && d->controllers[i].dead)
            controller_destroy(d, &d->controllers[i]);
    }
}

static void daemon_drain_inotify(struct daemon *d, bool *rescan)
{
    char buffer[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        ssize_t bytes = read(d->inotify_fd, buffer, sizeof(buffer));
        char *pointer;

        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return;
            log_error("inotify read: %s", strerror(errno));
            return;
        }

        if (bytes == 0)
            return;

        pointer = buffer;
        while (pointer < buffer + bytes) {
            struct inotify_event *event = (struct inotify_event *)pointer;

            if (event->mask & IN_Q_OVERFLOW)
                *rescan = true;
            if (event->len > 0 &&
                strncmp(event->name, "event", 5) == 0)
                *rescan = true;

            pointer += sizeof(*event) + event->len;
        }
    }
}

static int daemon_acquire_lock(struct daemon *d)
{
    const char *paths[] = {
        "/run/ds4xbox/lock",
        "/run/ds4xbox.lock",
    };
    char fallback[DS4_PATH_MAX];
    int i;

    snprintf(fallback, sizeof(fallback), "/tmp/ds4xbox-%u.lock",
             (unsigned)getuid());

    for (i = 0; i < 3; i++) {
        const char *path = i == 0 ? paths[0] :
                           i == 1 ? paths[1] : fallback;
        int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);

        if (fd < 0)
            continue;

        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK) {
                log_error("another ds4xbox instance is already running "
                          "(lock %s)", path);
                close(fd);
                return -1;
            }
            log_warn("cannot lock %s: %s", path, strerror(errno));
            close(fd);
            continue;
        }

        if (ftruncate(fd, 0) == 0)
            dprintf(fd, "%ld\n", (long)getpid());

        d->lock_fd = fd;
        snprintf(d->lock_path, sizeof(d->lock_path), "%s", path);
        log_debug("lock acquired: %s", path);
        return 0;
    }

    log_warn("could not create a lock file; continuing without "
             "single-instance protection");
    d->lock_fd = -1;
    return 0;
}

static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
        g_running = 0;
    else if (signo == SIGHUP || signo == SIGUSR1)
        g_rescan = 1;
}

static void trigger_udev_subsystem(const char *subsystem)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        log_debug("cannot fork udevadm: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }

        execlp("udevadm", "udevadm", "trigger",
               "--subsystem-match", subsystem, "--action=change",
               (char *)NULL);
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        log_debug("udevadm trigger --subsystem-match %s exited with %d",
                  subsystem, WEXITSTATUS(status));
    }
}

static void trigger_udev_change(void)
{
    trigger_udev_subsystem("input");
    trigger_udev_subsystem("hidraw");
    trigger_udev_subsystem("usb");
}

/*
 * The udev rule only hides the original controller while this flag exists.
 * Creating/removing it and re-triggering udev means that stopping the daemon
 * gives the controller back to the rest of the system.
 */
static void daemon_set_active(struct daemon *d, bool active)
{
    static const char *dir = "/run/ds4xbox";
    static const char *path = "/run/ds4xbox/active";
    int fd;

    if (active) {
        if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
            log_debug("cannot create %s: %s", dir, strerror(errno));
        }

        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            log_warn("cannot create %s: %s; the original controller "
                     "will not be hidden", path, strerror(errno));
            d->active_flag = false;
            return;
        }

        if (write(fd, "1\n", 2) < 0)
            log_debug("cannot write %s: %s", path, strerror(errno));
        close(fd);
        d->active_flag = true;
        log_debug("created %s", path);
    } else {
        if (unlink(path) < 0 && errno != ENOENT)
            log_debug("cannot remove %s: %s", path, strerror(errno));
        d->active_flag = false;
        log_debug("removed %s", path);
    }

    trigger_udev_change();
}

/* ------------------------------------------------------------------------- */
/* Hotplug                                                                   */
/* ------------------------------------------------------------------------- */

static int scan_devices(struct daemon *d)
{
    struct device_info infos[DS4_MAX_DEVICES];
    bool grouped[DS4_MAX_DEVICES];
    static char failed_group[DS4_GROUP_MAX];
    static uint64_t failed_at;
    uint64_t scan_start_ms = monotonic_ms();
    int n;
    int i;

    n = enumerate_devices(infos, DS4_MAX_DEVICES, false);
    if (n < 0) {
        int active = count_active_controllers(d);

        log_debug("device scan failed after %llums",
                  (unsigned long long)(monotonic_ms() - scan_start_ms));
        return active;
    }

    memset(grouped, 0, sizeof(grouped));

    for (i = 0; i < n; i++) {
        int members[DS4_MAX_MEMBERS];
        int member_count = 0;
        int primary = -1;
        int j;

        if (grouped[i])
            continue;

        for (j = i; j < n; j++) {
            if (grouped[j])
                continue;
            if (strcmp(infos[i].group, infos[j].group) != 0)
                continue;

            grouped[j] = true;
            if (member_count < DS4_MAX_MEMBERS) {
                members[member_count++] = j;
            } else {
                log_debug("group %s: ignoring extra node %s",
                          infos[i].group, infos[j].path);
            }
        }

        for (j = 0; j < member_count; j++) {
            struct device_info *di = &infos[members[j]];

            if (di->is_playstation && di->has_gamepad_caps) {
                primary = members[j];
                break;
            }
        }

        if (primary < 0 && d->force && d->only_device[0] != '\0') {
            for (j = 0; j < member_count; j++) {
                struct device_info *di = &infos[members[j]];

                if (strcmp(di->path, d->only_device) == 0 &&
                    di->has_gamepad_caps) {
                    primary = members[j];
                    break;
                }
            }
        }

        if (primary < 0)
            continue;

        if (d->only_device[0] != '\0') {
            bool found = false;

            for (j = 0; j < member_count; j++) {
                if (strcmp(infos[members[j]].path,
                           d->only_device) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
        }

        if (failed_group[0] != '\0' &&
            strcmp(infos[primary].group, failed_group) == 0 &&
            monotonic_ms() - failed_at < 5000) {
            log_debug("group %s: retrying after previous failure",
                      infos[primary].group);
            continue;
        }

        {
            struct controller *c =
                find_controller_by_group(d, infos[primary].group);

            if (c != NULL) {
                for (j = 0; j < member_count; j++) {
                    struct device_info *di = &infos[members[j]];
                    bool is_primary = (members[j] == primary);

                    if (di->fd < 0)
                        continue;

                    if (controller_has_member(c, di->path)) {
                        close(di->fd);
                        di->fd = -1;
                        continue;
                    }

                    /* Do not attach motion sensors, audio jacks, etc. */
                    if (!is_primary && !device_info_is_touchpad(di)) {
                        close(di->fd);
                        di->fd = -1;
                        continue;
                    }

                    /*
                     * Never install a second primary in an existing
                     * controller.  If the old primary is gone the
                     * controller is marked dead and will be reaped.
                     */
                    if (is_primary && c->primary_fd >= 0) {
                        close(di->fd);
                        di->fd = -1;
                        continue;
                    }

                    if (!controller_add_member(d, c, di, is_primary) &&
                        is_primary) {
                        log_warn("controller %d: cannot add primary %s",
                                 c->index + 1, di->path);
                        c->dead = true;
                    }
                }
            } else if (controller_create(d, &infos[primary], infos,
                                         members, member_count,
                                         primary) == NULL) {
                snprintf(failed_group, sizeof(failed_group), "%s",
                         infos[primary].group);
                failed_at = monotonic_ms();
            } else {
                failed_group[0] = '\0';
            }
        }
    }

    for (i = 0; i < n; i++) {
        if (infos[i].fd >= 0) {
            close(infos[i].fd);
            infos[i].fd = -1;
        }
    }

    {
        int active = count_active_controllers(d);

        log_debug("device scan: %d event nodes, %llums, %d controller(s)",
                  n,
                  (unsigned long long)(monotonic_ms() - scan_start_ms),
                  active);
        return active;
    }
}

/* ------------------------------------------------------------------------- */
/* Public daemon API                                                         */
/* ------------------------------------------------------------------------- */

int daemon_run(struct daemon *d)
{
    struct sigaction action;
    bool waiting = false;
    uint64_t last_scan;
    int result = -1;

    g_running = 1;
    g_rescan = 0;

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGUSR1, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    d->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (d->epoll_fd < 0) {
        log_error("epoll_create1: %s", strerror(errno));
        goto out;
    }

    d->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (d->inotify_fd >= 0) {
        struct epoll_event event;

        d->inotify_wd = inotify_add_watch(d->inotify_fd, "/dev/input",
                                          IN_CREATE | IN_DELETE |
                                          IN_MOVED_TO | IN_MOVED_FROM);
        if (d->inotify_wd < 0) {
            log_warn("cannot watch /dev/input: %s", strerror(errno));
            close(d->inotify_fd);
            d->inotify_fd = -1;
        } else {
            d->inotify_ref.type = REF_INOTIFY;
            d->inotify_ref.member = -1;
            d->inotify_ref.generation = 0;
            d->inotify_ref.controller = NULL;

            memset(&event, 0, sizeof(event));
            event.events = EPOLLIN;
            event.data.ptr = &d->inotify_ref;

            if (epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD,
                          d->inotify_fd, &event) < 0) {
                log_warn("cannot add inotify to epoll: %s",
                         strerror(errno));
                close(d->inotify_fd);
                d->inotify_fd = -1;
                d->inotify_wd = -1;
            }
        }
    } else {
        log_warn("inotify_init1 failed: %s; using periodic scanning only",
                 strerror(errno));
    }

    if (daemon_acquire_lock(d) < 0)
        goto out;

    if (d->trace_prefix[0] != '\0') {
        char input_path[DS4_PATH_MAX];
        char output_path[DS4_PATH_MAX];

        d->trace_capacity = DS4_TRACE_RECORDS;
        d->trace_records = calloc(d->trace_capacity,
                                  sizeof(*d->trace_records));
        if (d->trace_records == NULL) {
            log_error("cannot allocate memory for timestamp trace: %s",
                      strerror(errno));
            goto out;
        }

        if (!trace_path(input_path, sizeof(input_path), d->trace_prefix,
                        "-input.csv") ||
            !trace_path(output_path, sizeof(output_path), d->trace_prefix,
                        "-output.csv")) {
            log_error("timestamp trace prefix is too long");
            free(d->trace_records);
            d->trace_records = NULL;
            d->trace_capacity = 0;
            goto out;
        }
        log_info("timestamp trace enabled (last %u records in memory); "
                 "will write %s and %s on exit",
                 DS4_TRACE_RECORDS, input_path, output_path);
    }

    if (geteuid() != 0) {
        log_warn("running as uid %u; install the udev rules and run as "
                 "root to hide the original controller from other programs",
                 (unsigned)geteuid());
    }
    if (d->no_grab)
        log_warn("--no-grab is enabled: other programs can still read "
                 "the original controller");
    if (d->no_ff)
        log_info("force feedback forwarding is disabled");

    if (geteuid() == 0) {
        if (!d->no_grab)
            daemon_set_active(d, true);
        else if (access("/run/ds4xbox/active", F_OK) == 0)
            daemon_set_active(d, false);
    }

    waiting = scan_devices(d) == 0;
    if (waiting)
        log_info("waiting for a DualShock 4 / DualSense controller");

    last_scan = monotonic_ms();

    while (g_running) {
        struct epoll_event events[32];
        int n;
        int i;

        n = epoll_wait(d->epoll_fd, events,
                       (int)(sizeof(events) / sizeof(events[0])), 1000);
        if (n < 0) {
            if (errno == EINTR) {
                n = 0;
            } else {
                log_error("epoll_wait: %s", strerror(errno));
                break;
            }
        }

        for (i = 0; i < n; i++) {
            struct epoll_ref *ref = events[i].data.ptr;
            struct controller *c;

            if (ref == NULL)
                continue;

            if (ref->type == REF_INOTIFY) {
                bool rescan = false;

                daemon_drain_inotify(d, &rescan);
                if (rescan)
                    g_rescan = 1;
                continue;
            }

            c = ref->controller;
            if (c == NULL || !c->active || c->dead)
                continue;
            if (ref->generation != c->generation)
                continue;

            if (ref->type == REF_MEMBER) {
                controller_handle_member_readable(d, c, ref->member);
            } else if (ref->type == REF_UINPUT) {
                if (events[i].events & EPOLLOUT)
                    controller_flush_output(c);
                if (!c->dead &&
                    (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)))
                    controller_handle_uinput_readable(d, c);
            }
        }

        for (i = 0; i < DS4_MAX_CONTROLLERS; i++) {
            struct controller *c = &d->controllers[i];

            if (!c->active || c->dead)
                continue;
            if (c->resync_pending) {
                c->resync_pending = false;
                controller_resync(c);
            }
            if (c->test_ff_id >= 0 &&
                monotonic_ms() >= c->test_ff_until) {
                if (c->primary_fd >= 0)
                    ioctl(c->primary_fd, EVIOCRMFF, c->test_ff_id);
                c->test_ff_id = -1;
            }
            controller_log_diagnostics(c);
        }

        /*
         * Enumerating every evdev node can block this single-threaded loop
         * long enough to overrun a controller's kernel event queue.  inotify
         * already reports event-node hotplug; only poll periodically when
         * that watch could not be installed.
         */
        if (g_rescan || (d->inotify_fd < 0 &&
                         monotonic_ms() - last_scan >= 1000)) {
            int active;

            g_rescan = 0;
            active = scan_devices(d);

            if (active > 0) {
                waiting = false;
            } else if (!waiting) {
                log_info("waiting for a controller");
                waiting = true;
            }
            last_scan = monotonic_ms();
        }

        daemon_reap(d);
    }

    result = 0;

out:
    daemon_cleanup(d);
    return result;
}

static void daemon_trace_dump(struct daemon *d)
{
    char input_path[DS4_PATH_MAX];
    char output_path[DS4_PATH_MAX];
    FILE *input_file = NULL;
    FILE *output_file = NULL;
    bool input_opened = false;
    bool output_opened = false;
    uint64_t input_rows = 0;
    uint64_t output_rows = 0;
    size_t i;
    bool input_error = false;
    bool output_error = false;

    if (d->trace_records == NULL)
        return;

    if (!trace_path(input_path, sizeof(input_path), d->trace_prefix,
                    "-input.csv") ||
        !trace_path(output_path, sizeof(output_path), d->trace_prefix,
                    "-output.csv")) {
        log_error("timestamp trace prefix is too long; cannot save traces");
        free(d->trace_records);
        d->trace_records = NULL;
        d->trace_capacity = 0;
        return;
    }

    input_file = fopen(input_path, "w");
    if (input_file == NULL) {
        log_error("cannot create %s: %s", input_path, strerror(errno));
        input_error = true;
    } else {
        input_opened = true;
        if (fprintf(input_file,
                    "controller,generation,report,source_ns,read_ns,"
                    "type,code,value\n") < 0)
            input_error = true;
    }

    output_file = fopen(output_path, "w");
    if (output_file == NULL) {
        log_error("cannot create %s: %s", output_path, strerror(errno));
        output_error = true;
    } else {
        output_opened = true;
        if (fprintf(output_file,
                    "controller,generation,report,write_ns,type,code,"
                    "value\n") < 0)
            output_error = true;
    }

    for (i = 0; i < d->trace_count; i++) {
        size_t index = (d->trace_start + i) % d->trace_capacity;
        const struct trace_record *record = &d->trace_records[index];

        if (record->stream == TRACE_INPUT && input_file != NULL &&
            !input_error) {
            if (fprintf(input_file,
                        "%u,%llu,%llu,%llu,%llu,%u,%u,%d\n",
                        record->controller,
                        (unsigned long long)record->generation,
                        (unsigned long long)record->report_seq,
                        (unsigned long long)record->timestamp_ns,
                        (unsigned long long)record->observed_ns,
                        record->type, record->code, record->value) < 0) {
                input_error = true;
            } else {
                input_rows++;
            }
        } else if (record->stream == TRACE_OUTPUT && output_file != NULL &&
                   !output_error) {
            if (fprintf(output_file,
                        "%u,%llu,%llu,%llu,%u,%u,%d\n",
                        record->controller,
                        (unsigned long long)record->generation,
                        (unsigned long long)record->report_seq,
                        (unsigned long long)record->timestamp_ns,
                        record->type, record->code, record->value) < 0) {
                output_error = true;
            } else {
                output_rows++;
            }
        }
    }

    if (input_file != NULL && fclose(input_file) < 0)
        input_error = true;
    if (output_file != NULL && fclose(output_file) < 0)
        output_error = true;

    if (input_error && input_opened)
        log_error("error writing input trace %s", input_path);
    if (output_error && output_opened)
        log_error("error writing output trace %s", output_path);
    log_info("timestamp trace processed: input=%llu rows, output=%llu rows",
             (unsigned long long)input_rows,
             (unsigned long long)output_rows);
    if (d->trace_overwritten > 0)
        log_warn("timestamp trace ring overwrote %llu old record(s)",
                 (unsigned long long)d->trace_overwritten);

    free(d->trace_records);
    d->trace_records = NULL;
    d->trace_capacity = 0;
    d->trace_count = 0;
    d->trace_start = 0;
}

void daemon_cleanup(struct daemon *d)
{
    int i;

    for (i = 0; i < DS4_MAX_CONTROLLERS; i++) {
        if (d->controllers[i].active)
            controller_destroy(d, &d->controllers[i]);
    }

    daemon_trace_dump(d);

    if (d->active_flag)
        daemon_set_active(d, false);

    if (d->inotify_fd >= 0) {
        if (d->inotify_wd >= 0)
            inotify_rm_watch(d->inotify_fd, d->inotify_wd);
        close(d->inotify_fd);
        d->inotify_fd = -1;
        d->inotify_wd = -1;
    }

    if (d->epoll_fd >= 0) {
        close(d->epoll_fd);
        d->epoll_fd = -1;
    }

    if (d->lock_fd >= 0) {
        close(d->lock_fd);
        d->lock_fd = -1;
    }
}

int list_devices(void)
{
    struct device_info infos[DS4_MAX_DEVICES];
    int n;
    int i;

    n = enumerate_devices(infos, DS4_MAX_DEVICES, true);
    if (n < 0)
        return 1;

    printf("%-22s %-6s %-9s %-10s %s\n",
           "DEVICE", "BUS", "VID:PID", "TYPE", "NAME");

    for (i = 0; i < n; i++) {
        struct device_info *di = &infos[i];

        if (!di->opened) {
            printf("%-22s (permission denied)\n", di->path);
            continue;
        }

        printf("%-22s %04x   %04x:%04x  %-10s %s\n",
               di->path, di->id.bustype, di->id.vendor, di->id.product,
               di->is_playstation ? "PS gamepad" :
               di->has_gamepad_caps ? "gamepad" : "other",
               di->name);

        if (di->phys[0] != '\0' || di->uniq[0] != '\0') {
            printf("  phys=%s uniq=%s\n", di->phys, di->uniq);
            printf("  group=%s\n", di->group);
        }
        fflush(stdout);
    }

    for (i = 0; i < n; i++) {
        if (infos[i].fd >= 0)
            close(infos[i].fd);
    }

    return 0;
}
