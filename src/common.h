/*
 * ds4xbox - DualShock 4 / DualSense to Xbox 360/One remapper for Linux
 *
 * This file is released into the public domain (Unlicense).
 */
#ifndef DS4XBOX_COMMON_H
#define DS4XBOX_COMMON_H

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DS4XBOX_VERSION "1.0.0"

#define SONY_VENDOR 0x054c
#define MICROSOFT_VENDOR 0x045e

#define DS4_NAME_MAX 256
#define DS4_GROUP_MAX (DS4_NAME_MAX + 8)
#define DS4_PATH_MAX 4096
#define DS4_MAX_CONTROLLERS 8
#define DS4_MAX_MEMBERS 8
#define DS4_MAX_DEVICES 128
#define DS4_OUTPUT_EVENTS 64
#define DS4_OUTPUT_FRAMES 128
#define DS4_TRACE_RECORDS 262144

#define DS4_BITS_TO_LONGS(nr) (((nr) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))

enum log_level {
    LOG_LVL_ERROR = 0,
    LOG_LVL_WARN,
    LOG_LVL_INFO,
    LOG_LVL_DEBUG,
};

extern enum log_level g_log_level;

void log_msg(enum log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_error(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_LVL_WARN, __VA_ARGS__)
#define log_info(...) log_msg(LOG_LVL_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)

static inline bool bit_test(const unsigned long *bits, unsigned int bit)
{
    return !!(bits[bit / (8 * sizeof(unsigned long))] &
              (1UL << (bit % (8 * sizeof(unsigned long)))));
}

static inline void bit_set(unsigned long *bits, unsigned int bit)
{
    bits[bit / (8 * sizeof(unsigned long))] |= 1UL << (bit % (8 * sizeof(unsigned long)));
}

static inline void bit_clear(unsigned long *bits, unsigned int bit)
{
    bits[bit / (8 * sizeof(unsigned long))] &= ~(1UL << (bit % (8 * sizeof(unsigned long))));
}

struct axis_info {
    bool present;
    struct input_absinfo info;
};

struct profile {
    const char *name;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
    const char *device_name;
};

struct device_info {
    char path[DS4_PATH_MAX];
    char name[DS4_NAME_MAX];
    char phys[DS4_NAME_MAX];
    char uniq[DS4_NAME_MAX];
    char group[DS4_GROUP_MAX];
    struct input_id id;
    bool has_gamepad_caps;
    bool is_playstation;
    bool opened;
    int fd;
    unsigned long keybits[DS4_BITS_TO_LONGS(KEY_CNT)];
    unsigned long absbits[DS4_BITS_TO_LONGS(ABS_CNT)];
    unsigned long ffbits[DS4_BITS_TO_LONGS(FF_CNT)];
};

struct controller;
struct daemon;

enum trace_stream {
    TRACE_INPUT = 1,
    TRACE_OUTPUT,
};

struct trace_record {
    uint64_t timestamp_ns;
    uint64_t observed_ns;
    uint64_t report_seq;
    uint64_t generation;
    int32_t value;
    uint16_t type;
    uint16_t code;
    uint8_t controller;
    uint8_t stream;
};

struct epoll_ref {
    int type;
    int member;
    uint64_t generation;
    struct controller *controller;
};

enum epoll_ref_type {
    REF_INOTIFY = 1,
    REF_MEMBER,
    REF_UINPUT,
};

struct member {
    bool used;
    bool primary;
    bool grabbed;
    bool touchpad;
    bool dirty;
    bool syncing;
    bool monotonic_clock;
    char path[DS4_PATH_MAX];
    char name[DS4_NAME_MAX];
    int fd;
    uint64_t stats_events;
    uint64_t stats_abs_events;
    uint64_t stats_reports;
    uint64_t stats_syn_dropped;
    uint64_t stats_recoveries;
    struct epoll_ref ref;
};

struct output_frame {
    struct input_event events[DS4_OUTPUT_EVENTS];
    unsigned int count;
    unsigned int abs_remaining;
    uint64_t report_seq;
};

struct controller_diagnostics {
    uint64_t last_log_ms;
    uint64_t input_reports;
    uint64_t input_abs_sum;
    uint64_t input_abs_current;
    uint64_t input_abs_min;
    uint64_t input_abs_max;
    uint64_t input_abs_seen;
    uint64_t input_abs_discarded;
    uint64_t input_lag_max_us;
    uint64_t input_recovery_reports;
    uint64_t syn_dropped;
    uint64_t abs_coalesced;
    uint64_t frames_enqueued;
    uint64_t frames_written;
    uint64_t frames_dropped;
    uint64_t abs_enqueued;
    uint64_t abs_written;
    uint64_t abs_dropped;
    uint64_t queue_peak;
};

struct controller {
    bool active;
    bool dead;
    bool announced;
    struct daemon *owner;
    uint64_t generation;
    uint64_t input_report_seq;
    bool input_report_open;
    bool swap_sticks_triggers;
    int index;
    char group[DS4_GROUP_MAX];
    char name[DS4_NAME_MAX];
    char uniq[DS4_NAME_MAX];
    struct input_id id;
    int primary_fd;
    int uinput_fd;
    bool ff_enabled;
    int ff_max;
    int real_ff_id[FF_MAX_EFFECTS];
    struct axis_info abs[ABS_CNT];
    bool has_trigger_axes;
    bool has_hat;
    unsigned long src_ffbits[DS4_BITS_TO_LONGS(FF_CNT)];
    struct member members[DS4_MAX_MEMBERS];
    int member_count;
    struct input_event outbuf[DS4_OUTPUT_EVENTS];
    int outcount;
    struct output_frame outq[DS4_OUTPUT_FRAMES];
    unsigned int outq_head;
    unsigned int outq_tail;
    unsigned int outq_count;
    unsigned int outq_dropped;
    bool outq_blocked;
    bool outq_overflow;
    bool resync_pending;
    bool epollout_enabled;
    int epoll_fd;
    int test_ff_id;
    uint64_t test_ff_until;
    unsigned int dpad_state;
    int last_hat_x;
    int last_hat_y;
    struct controller_diagnostics diagnostics;
    struct epoll_ref uinput_ref;
};

struct daemon {
    int epoll_fd;
    int inotify_fd;
    int inotify_wd;
    int lock_fd;
    char lock_path[DS4_PATH_MAX];
    struct controller controllers[DS4_MAX_CONTROLLERS];
    struct profile profile;
    bool no_grab;
    bool no_ff;
    bool force;
    bool swap_sticks_triggers;
    bool verbose;
    bool quiet;
    bool active_flag;
    bool test_rumble;
    uint64_t next_generation;
    char trace_prefix[DS4_PATH_MAX];
    struct trace_record *trace_records;
    size_t trace_capacity;
    size_t trace_count;
    size_t trace_start;
    uint64_t trace_overwritten;
    struct epoll_ref inotify_ref;
    char only_device[DS4_PATH_MAX];
};

/* mapping.c */
int map_stick_value(int value, const struct input_absinfo *info);
int map_trigger_value(int value, const struct input_absinfo *info);
int map_hat_value(int value, const struct input_absinfo *info);
int map_button_code(unsigned int code);
int map_axis_code(unsigned int code, bool swap_sticks_triggers);
int map_axis_kind(int code, bool swap_sticks_triggers);
bool is_dpad_button(unsigned int code);
int mapping_self_test(void);

enum axis_kind {
    AXIS_NONE = 0,
    AXIS_STICK,
    AXIS_TRIGGER,
    AXIS_HAT,
};

/* daemon.c */
int daemon_run(struct daemon *d);
void daemon_cleanup(struct daemon *d);
int list_devices(void);

/* main.c helpers */
const struct profile *profile_by_name(const char *name);
void print_usage(FILE *out);

#endif /* DS4XBOX_COMMON_H */
