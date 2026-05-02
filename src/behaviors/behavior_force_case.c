/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/hid_indicators.h>
#include <zmk/endpoints.h>

#include <dt-bindings/zmk/modifiers.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#define ZMK_LED_CAPSLOCK_BIT BIT(1) // *** from https://github.com/darknao/zmk/blob/2fad527cc5abed5bb59b4d4a4b0ee511d0e514e9/app/src/rgb_underglow.c#L320 ***

#define ZMK_SHIFT_MODS (MOD_LSFT | MOD_RSFT)

/* HID usage codes for Left/Right Shift keys (USB HID keyboard page) */
#define LSHIFT_USAGE 0xE1
#define RSHIFT_USAGE 0xE5

/* -----------------------------------------------------------------------
 * Per-key press state — snapshotted at press time, consumed at release.
 * ----------------------------------------------------------------------- */
struct force_case_state {
    bool shift_held;
    bool shift_sticky;
};

/* -----------------------------------------------------------------------
 * Detect sticky shift vs physical shift.
 *
 * Physical shift: hid_listener called zmk_hid_press(LSHIFT/RSHIFT usage)
 *   → keycode appears in the pressed-keys bitmap.
 * Sticky shift: behavior_sticky_key only called zmk_hid_register_mod()
 *   → keycode NOT in the pressed-keys bitmap.
 * ----------------------------------------------------------------------- */
static bool is_sticky_shift(void) {
    if (!(zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS)) {
        return false;
    }
    bool lshift_key_pressed = zmk_hid_is_pressed(ZMK_HID_USAGE(HID_USAGE_KEY, LSHIFT_USAGE));
    bool rshift_key_pressed = zmk_hid_is_pressed(ZMK_HID_USAGE(HID_USAGE_KEY, RSHIFT_USAGE));
    return !lshift_key_pressed && !rshift_key_pressed;
}

/* -----------------------------------------------------------------------
 * Directly consume sticky shift by unregistering its modifier from the
 * HID layer and sending an updated report.
 *
 * We don't try to interact with sticky key's internal state machine via
 * events. Instead we do exactly what sticky key's cleanup would do:
 * call zmk_hid_unregister_mods() for whichever shift bits are active
 * in explicit_modifiers but NOT in the pressed-keys bitmap (i.e. sticky).
 *
 * zmk_hid_unregister_mods decrements the ref-count and removes the bit
 * from explicit_modifiers when it hits zero. We then send the report so
 * the host sees the updated (no shift) modifier state immediately.
 *
 * Sticky key's own cleanup will still fire eventually when its timeout
 * expires or its position event fires, but since the mod ref-count will
 * already be zero, its unregister call will be a no-op.
 * ----------------------------------------------------------------------- */
static void consume_sticky_shift(void) {
    zmk_mod_flags_t active = zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS;
    if (!active) {
        return;
    }
    /* Unregister only the shift bits that are sticky (not in pressed keys) */
    if ((active & MOD_LSFT) &&
        !zmk_hid_is_pressed(ZMK_HID_USAGE(HID_USAGE_KEY, LSHIFT_USAGE))) {
        zmk_hid_unregister_mods(MOD_LSFT);
    }
    if ((active & MOD_RSFT) &&
        !zmk_hid_is_pressed(ZMK_HID_USAGE(HID_USAGE_KEY, RSHIFT_USAGE))) {
        zmk_hid_unregister_mods(MOD_RSFT);
    }
    /* Send the updated modifier report so the host sees shift is gone */
    zmk_endpoints_send_report(HID_USAGE_KEY);
}

/* -----------------------------------------------------------------------
 * Shared helper.
 *
 * want_upper:       true  → produce uppercase regardless of CapsLock
 *                   false → produce lowercase regardless of CapsLock
 * shift_was_sticky: true  → directly consume sticky shift after sending
 *                   false → leave shift state untouched
 *
 * report_shift = want_upper XOR caps_active:
 *   caps=0 want_upper=1 → shift=1
 *   caps=0 want_upper=0 → shift=0
 *   caps=1 want_upper=1 → shift=0
 *   caps=1 want_upper=0 → shift=1
 * ----------------------------------------------------------------------- */
static int send_key(uint32_t keycode, bool pressed, bool want_upper,
                    bool shift_was_sticky) {
    zmk_hid_indicators_t ind = zmk_hid_indicators_get_current_profile();
    bool caps_active  = (ind & ZMK_LED_CAPSLOCK_BIT) != 0;
    bool report_shift = want_upper ^ caps_active;

    zmk_hid_masked_modifiers_set(ZMK_SHIFT_MODS);

    if (report_shift) {
        zmk_hid_implicit_modifiers_press(MOD_LSFT);
    }

    int ret;
    if (pressed) {
        ret = zmk_hid_press(ZMK_HID_USAGE(HID_USAGE_KEY, keycode));
    } else {
        ret = zmk_hid_release(ZMK_HID_USAGE(HID_USAGE_KEY, keycode));
    }

    if (ret == 0) {
        ret = zmk_endpoints_send_report(HID_USAGE_KEY);
    }

    if (report_shift) {
        zmk_hid_implicit_modifiers_release();
    }
    zmk_hid_masked_modifiers_clear();

    /*
     * On key PRESS: if shift was sticky, directly consume it now by
     * unregistering it from explicit_modifiers and sending a report.
     * This immediately removes shift from the HID state without needing
     * any event bus interaction at all.
     */
    if (pressed && shift_was_sticky) {
        consume_sticky_shift();
    }

    return ret;
}

/* -----------------------------------------------------------------------
 * FORCE-UPPER (fucase)
 * Ignores CapsLock. Shift inverts: no shift → upper, shift → lower.
 * Sticky shift consumed on press; physical shift untouched.
 * ----------------------------------------------------------------------- */
#define DT_DRV_COMPAT zmk_behavior_force_upper

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static struct force_case_state force_upper_state[DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)];

static int on_force_upper_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_upper_state[0];
    state->shift_held   = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    state->shift_sticky = is_sticky_shift();
    return send_key(binding->param1, true, !state->shift_held, state->shift_sticky);
}

static int on_force_upper_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_upper_state[0];
    return send_key(binding->param1, false, !state->shift_held, false);
}

static const struct behavior_driver_api force_upper_driver_api = {
    .binding_pressed  = on_force_upper_binding_pressed,
    .binding_released = on_force_upper_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_upper_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

/* -----------------------------------------------------------------------
 * FORCE-LOWER (flcase)
 * Ignores CapsLock. Shift inverts: no shift → lower, shift → upper.
 * Sticky shift consumed on press; physical shift untouched.
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_lower

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static struct force_case_state force_lower_state[DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)];

static int on_force_lower_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_lower_state[0];
    state->shift_held   = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    state->shift_sticky = is_sticky_shift();
    return send_key(binding->param1, true, state->shift_held, state->shift_sticky);
}

static int on_force_lower_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_lower_state[0];
    return send_key(binding->param1, false, state->shift_held, false);
}

static const struct behavior_driver_api force_lower_driver_api = {
    .binding_pressed  = on_force_lower_binding_pressed,
    .binding_released = on_force_lower_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_lower_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

/* -----------------------------------------------------------------------
 * FORCE-TRUE-UPPER (ftucase)
 * Always outputs uppercase. Ignores BOTH CapsLock and Shift entirely.
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_true_upper

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_force_true_upper_binding_pressed(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, true, true, false);
}

static int on_force_true_upper_binding_released(struct zmk_behavior_binding *binding,
                                                struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, false, true, false);
}

static const struct behavior_driver_api force_true_upper_driver_api = {
    .binding_pressed  = on_force_true_upper_binding_pressed,
    .binding_released = on_force_true_upper_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_true_upper_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

/* -----------------------------------------------------------------------
 * FORCE-TRUE-LOWER (ftlcase)
 * Always outputs lowercase. Ignores BOTH CapsLock and Shift entirely.
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_true_lower

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_force_true_lower_binding_pressed(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, true, false, false);
}

static int on_force_true_lower_binding_released(struct zmk_behavior_binding *binding,
                                                struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, false, false, false);
}

static const struct behavior_driver_api force_true_lower_driver_api = {
    .binding_pressed  = on_force_true_lower_binding_pressed,
    .binding_released = on_force_true_lower_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_true_lower_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
