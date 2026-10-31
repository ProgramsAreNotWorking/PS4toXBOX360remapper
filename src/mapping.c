/*
 * ds4xbox - axis/button mapping
 *
 * This file is released into the public domain (Unlicense).
 */
#include "common.h"

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

/*
 * Map a stick axis to the Xbox 360 signed 16-bit range.
 *
 * The kernel reports DualShock 4 sticks as 0..255 with 128 at rest on
 * hid-playstation.  Older drivers use -128..127 or -32768..32767.  The
 * centre must stay at 0, so the two halves are scaled independently.
 */
int map_stick_value(int value, const struct input_absinfo *info)
{
    int minimum;
    int maximum;
    int64_t out;

    if (info == NULL)
        return 0;

    minimum = info->minimum;
    maximum = info->maximum;

    if (maximum <= minimum)
        return 0;

    if (value <= minimum)
        return -32768;
    if (value >= maximum)
        return 32767;

    if (minimum < 0) {
        if (value < 0) {
            out = (int64_t)value * 32768 / -(int64_t)minimum;
        } else if (value > 0) {
            out = (int64_t)value * 32767 / maximum;
        } else {
            return 0;
        }
    } else {
        int centre = (minimum + maximum + 1) / 2;

        if (value < centre)
            out = (int64_t)(value - centre) * 32768 / (centre - minimum);
        else if (value > centre)
            out = (int64_t)(value - centre) * 32767 / (maximum - centre);
        else
            return 0;
    }

    return clamp_int((int)out, -32768, 32767);
}

/* Map a trigger axis to the Xbox 360 0..255 range. */
int map_trigger_value(int value, const struct input_absinfo *info)
{
    int minimum;
    int maximum;
    int64_t out;

    if (info == NULL)
        return 0;

    minimum = info->minimum;
    maximum = info->maximum;

    if (maximum <= minimum)
        return 0;

    if (value <= minimum)
        return 0;
    if (value >= maximum)
        return 255;

    out = (int64_t)(value - minimum) * 255 / (maximum - minimum);
    return clamp_int((int)out, 0, 255);
}

/* Map a hat axis to -1, 0 or 1. */
int map_hat_value(int value, const struct input_absinfo *info)
{
    int minimum;
    int maximum;
    double position;

    if (info == NULL)
        return 0;

    minimum = info->minimum;
    maximum = info->maximum;

    if (maximum <= minimum)
        return 0;

    if (value <= minimum)
        return -1;
    if (value >= maximum)
        return 1;

    position = (double)(value - minimum) / (double)(maximum - minimum);
    if (position < 0.25)
        return -1;
    if (position > 0.75)
        return 1;
    return 0;
}

/*
 * Translate a DualShock 4 button code to the virtual Xbox pad.
 *
 * Linux uses positional names for the face buttons, so Square (BTN_WEST)
 * must become Xbox X (BTN_NORTH) and Triangle (BTN_NORTH) must become
 * Xbox Y (BTN_WEST).  Cross and Circle keep their positions.
 */
int map_button_code(unsigned int code)
{
    switch (code) {
    case BTN_SOUTH:
        return BTN_SOUTH; /* Cross -> A */
    case BTN_EAST:
        return BTN_EAST; /* Circle -> B */
    case BTN_WEST:
        return BTN_NORTH; /* Square -> X */
    case BTN_NORTH:
        return BTN_WEST; /* Triangle -> Y */
    case BTN_TL:
        return BTN_TL;
    case BTN_TR:
        return BTN_TR;
    case BTN_SELECT:
        return BTN_SELECT; /* Share -> Back */
    case BTN_START:
        return BTN_START; /* Options -> Start */
    case BTN_MODE:
        return BTN_MODE; /* PS -> Guide */
    case BTN_THUMBL:
        return BTN_THUMBL;
    case BTN_THUMBR:
        return BTN_THUMBR;
    case BTN_LEFT:
        return BTN_SELECT; /* Touchpad click -> Back */
    default:
        return -1;
    }
}

/*
 * Translate a source axis to the virtual axis.  When
 * swap_sticks_triggers is set, controllers that expose the right stick
 * on ABS_Z/ABS_RZ and the triggers on ABS_RX/ABS_RY are supported.
 */
int map_axis_code(unsigned int code, bool swap_sticks_triggers)
{
    if (swap_sticks_triggers) {
        switch (code) {
        case ABS_X:
            return ABS_X;
        case ABS_Y:
            return ABS_Y;
        case ABS_RX:
            return ABS_Z;
        case ABS_RY:
            return ABS_RZ;
        case ABS_Z:
            return ABS_RX;
        case ABS_RZ:
            return ABS_RY;
        case ABS_HAT0X:
            return ABS_HAT0X;
        case ABS_HAT0Y:
            return ABS_HAT0Y;
        default:
            return -1;
        }
    }

    switch (code) {
    case ABS_X:
        return ABS_X;
    case ABS_Y:
        return ABS_Y;
    case ABS_RX:
        return ABS_RX;
    case ABS_RY:
        return ABS_RY;
    case ABS_Z:
        return ABS_Z;
    case ABS_RZ:
        return ABS_RZ;
    case ABS_HAT0X:
        return ABS_HAT0X;
    case ABS_HAT0Y:
        return ABS_HAT0Y;
    default:
        return -1;
    }
}

int map_axis_kind(int code, bool swap_sticks_triggers)
{
    if (swap_sticks_triggers) {
        switch (code) {
        case ABS_X:
        case ABS_Y:
        case ABS_Z:
        case ABS_RZ:
            return AXIS_STICK;
        case ABS_RX:
        case ABS_RY:
            return AXIS_TRIGGER;
        case ABS_HAT0X:
        case ABS_HAT0Y:
            return AXIS_HAT;
        default:
            return AXIS_NONE;
        }
    }

    switch (code) {
    case ABS_X:
    case ABS_Y:
    case ABS_RX:
    case ABS_RY:
        return AXIS_STICK;
    case ABS_Z:
    case ABS_RZ:
        return AXIS_TRIGGER;
    case ABS_HAT0X:
    case ABS_HAT0Y:
        return AXIS_HAT;
    default:
        return AXIS_NONE;
    }
}

bool is_dpad_button(unsigned int code)
{
    switch (code) {
    case BTN_DPAD_UP:
    case BTN_DPAD_DOWN:
    case BTN_DPAD_LEFT:
    case BTN_DPAD_RIGHT:
        return true;
    default:
        return false;
    }
}

int mapping_self_test(void)
{
    int failures = 0;
    struct input_absinfo stick_u8 = {
        .minimum = 0,
        .maximum = 255,
    };
    struct input_absinfo stick_s16 = {
        .minimum = -32768,
        .maximum = 32767,
    };
    struct input_absinfo stick_s8 = {
        .minimum = -128,
        .maximum = 127,
    };
    struct input_absinfo stick_u10 = {
        .minimum = 0,
        .maximum = 1023,
    };
    struct input_absinfo trigger_u8 = {
        .minimum = 0,
        .maximum = 255,
    };
    struct input_absinfo trigger_u10 = {
        .minimum = 0,
        .maximum = 1023,
    };
    struct input_absinfo hat = {
        .minimum = -1,
        .maximum = 1,
    };
    struct input_absinfo hat_u3 = {
        .minimum = 0,
        .maximum = 7,
    };

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "self-test failed: %s (%s:%d)\n", #condition,      \
                    __FILE__, __LINE__);                                       \
            failures++;                                                        \
        }                                                                      \
    } while (0)

    CHECK(map_stick_value(128, &stick_u8) == 0);
    CHECK(map_stick_value(0, &stick_u8) == -32768);
    CHECK(map_stick_value(255, &stick_u8) == 32767);
    CHECK(map_stick_value(127, &stick_u8) < 0);
    CHECK(map_stick_value(129, &stick_u8) > 0);

    CHECK(map_stick_value(0, &stick_s16) == 0);
    CHECK(map_stick_value(-32768, &stick_s16) == -32768);
    CHECK(map_stick_value(32767, &stick_s16) == 32767);
    CHECK(map_stick_value(-1, &stick_s16) == -1);
    CHECK(map_stick_value(1, &stick_s16) == 1);

    CHECK(map_stick_value(0, &stick_s8) == 0);
    CHECK(map_stick_value(-128, &stick_s8) == -32768);
    CHECK(map_stick_value(127, &stick_s8) == 32767);

    CHECK(map_stick_value(512, &stick_u10) == 0);
    CHECK(map_stick_value(0, &stick_u10) == -32768);
    CHECK(map_stick_value(1023, &stick_u10) == 32767);

    CHECK(map_trigger_value(0, &trigger_u8) == 0);
    CHECK(map_trigger_value(128, &trigger_u8) == 128);
    CHECK(map_trigger_value(255, &trigger_u8) == 255);
    CHECK(map_trigger_value(0, &trigger_u10) == 0);
    CHECK(map_trigger_value(1023, &trigger_u10) == 255);

    CHECK(map_hat_value(-1, &hat) == -1);
    CHECK(map_hat_value(0, &hat) == 0);
    CHECK(map_hat_value(1, &hat) == 1);
    CHECK(map_hat_value(0, &hat_u3) == -1);
    CHECK(map_hat_value(3, &hat_u3) == 0);
    CHECK(map_hat_value(7, &hat_u3) == 1);

    CHECK(map_button_code(BTN_SOUTH) == BTN_SOUTH);
    CHECK(map_button_code(BTN_EAST) == BTN_EAST);
    CHECK(map_button_code(BTN_WEST) == BTN_NORTH);
    CHECK(map_button_code(BTN_NORTH) == BTN_WEST);
    CHECK(map_button_code(BTN_TL) == BTN_TL);
    CHECK(map_button_code(BTN_TR) == BTN_TR);
    CHECK(map_button_code(BTN_SELECT) == BTN_SELECT);
    CHECK(map_button_code(BTN_START) == BTN_START);
    CHECK(map_button_code(BTN_MODE) == BTN_MODE);
    CHECK(map_button_code(BTN_THUMBL) == BTN_THUMBL);
    CHECK(map_button_code(BTN_THUMBR) == BTN_THUMBR);
    CHECK(map_button_code(BTN_LEFT) == BTN_SELECT);
    CHECK(map_button_code(BTN_TL2) == -1);
    CHECK(map_button_code(BTN_TR2) == -1);

    CHECK(map_axis_code(ABS_X, false) == ABS_X);
    CHECK(map_axis_code(ABS_Y, false) == ABS_Y);
    CHECK(map_axis_code(ABS_RX, false) == ABS_RX);
    CHECK(map_axis_code(ABS_RY, false) == ABS_RY);
    CHECK(map_axis_code(ABS_Z, false) == ABS_Z);
    CHECK(map_axis_code(ABS_RZ, false) == ABS_RZ);
    CHECK(map_axis_code(ABS_HAT0X, false) == ABS_HAT0X);
    CHECK(map_axis_code(ABS_HAT0Y, false) == ABS_HAT0Y);
    CHECK(map_axis_code(ABS_MT_POSITION_X, false) == -1);

    CHECK(map_axis_code(ABS_RX, true) == ABS_Z);
    CHECK(map_axis_code(ABS_RY, true) == ABS_RZ);
    CHECK(map_axis_code(ABS_Z, true) == ABS_RX);
    CHECK(map_axis_code(ABS_RZ, true) == ABS_RY);
    CHECK(map_axis_kind(ABS_X, false) == AXIS_STICK);
    CHECK(map_axis_kind(ABS_RX, false) == AXIS_STICK);
    CHECK(map_axis_kind(ABS_Z, false) == AXIS_TRIGGER);
    CHECK(map_axis_kind(ABS_HAT0X, false) == AXIS_HAT);
    CHECK(map_axis_kind(ABS_RX, true) == AXIS_TRIGGER);
    CHECK(map_axis_kind(ABS_Z, true) == AXIS_STICK);

    CHECK(is_dpad_button(BTN_DPAD_UP));
    CHECK(is_dpad_button(BTN_DPAD_DOWN));
    CHECK(is_dpad_button(BTN_DPAD_LEFT));
    CHECK(is_dpad_button(BTN_DPAD_RIGHT));
    CHECK(!is_dpad_button(BTN_SOUTH));

#undef CHECK

    return failures == 0 ? 0 : -1;
}
