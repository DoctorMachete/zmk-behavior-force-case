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

#define LSHIFT_USAGE 0xE1
#define RSHIFT_USAGE 0xE5

/* -----------------------------------------------------------------------
 * Per-key press state — snapshotted at press time, reused at release.
 * ----------------------------------------------------------------------- */
struct force_case_state {
    bool shift_held;
};

/* -----------------------------------------------------------------------
 * Detect sticky shift vs physical shift.
 * Sticky: in explicit_mods but NOT in pressed-keys bitmap.
 * Physical: in explicit_mods AND in pressed-keys bitmap.
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
 * Shared helper.
 *
 * Now that we have hid_listener.c and hid.c, the flow is exactly known:
 *
 * hid_listener_keycode_pressed:
 *   1. zmk_hid_press(usage)
 *   2. zmk_hid_register_mods(ev->explicit_modifiers)   [SET_MODIFIERS]
 *   3. zmk_hid_implicit_modifiers_press(ev->implicit)  [implicit = ev->implicit (=0), SET_MODIFIERS]
 *   4. zmk_endpoints_send_report()   ← sends WRONG report (wrong shift)
 *
 * hid_listener_keycode_released:
 *   1. zmk_hid_release(usage)
 *   2. zmk_endpoints_send_report()   (if SEPARATE_MOD_RELEASE_REPORT)
 *   3. zmk_hid_unregister_mods(ev->explicit_modifiers)
 *   4. zmk_hid_implicit_modifiers_release()  [implicit = 0, SET_MODIFIERS]
 *   5. zmk_endpoints_send_report()   ← sends WRONG report
 *
 * After raise_zmk_keycode_state_changed_from_encoded() returns:
 *   - implicit_modifiers = 0  (cleared by hid_listener step 3/4)
 *   - masked_modifiers = 0    (we never set it in this call)
 *   - key is in/out of report correctly
 *   - one wrong report already sent to host
 *
 * We then apply our correction and send a second report.
 * The host sees two consecutive reports; the second (correct) one wins.
 * On BLE this is fine — characteristic notifications, last value wins.
 * On USB HID this is fine — interrupt endpoint, host polls latest value.
 *
 * For sticky key: raise_... fires sticky key listener which processes
 * the real keycode correctly (records at press, releases at key-up).
 * sticky key's ZMK_EVENT_RAISE_AFTER also completes before raise returns,
 * so hid_listener runs a second time for the re-raised event — but that
 * second run also produces a wrong report, and our correction after the
 * raise covers all of them since we send last.
 *
 * report_shift = want_upper XOR caps_active
 * ----------------------------------------------------------------------- */
static int send_key(uint32_t keycode, bool pressed, bool want_upper) {
    /*
     * Raise real keycode on bus:
     *   - sticky key processes it (records keycode, schedules/fires release)
     *   - hid_listener updates key bitmap and sends a report (wrong shift)
     * All of this completes synchronously before raise returns.
     */
    raise_zmk_keycode_state_changed_from_encoded(keycode, pressed, k_uptime_get());

    /*
     * Now correct the modifier state and re-send.
     * implicit_modifiers and masked_modifiers are both 0 at this point
     * (hid_listener cleared them). The key is correctly in/out of bitmap.
     * We only need to fix the modifier byte and resend.
     */
    zmk_hid_indicators_t ind = zmk_hid_indicators_get_current_profile();
    bool caps_active  = (ind & ZMK_LED_CAPSLOCK_BIT) != 0;
    bool report_shift = want_upper ^ caps_active;

    zmk_hid_masked_modifiers_set(ZMK_SHIFT_MODS);
    if (report_shift) {
        zmk_hid_implicit_modifiers_press(MOD_LSFT);
    }

    int ret = zmk_endpoints_send_report(HID_USAGE_KEY);

    if (report_shift) {
        zmk_hid_implicit_modifiers_release();
    }
    zmk_hid_masked_modifiers_clear();

    return ret;
}

/* -----------------------------------------------------------------------
 * FORCE-UPPER (fucase)
 * Ignores CapsLock. Shift inverts: no shift → upper, shift → lower.
 * ----------------------------------------------------------------------- */
#define DT_DRV_COMPAT zmk_behavior_force_upper

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static struct force_case_state force_upper_state[DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)];

static int on_force_upper_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_upper_state[0];
    /* Snapshot before raise — sticky shift still in explicit_mods here */
    state->shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, true, !state->shift_held);
}

static int on_force_upper_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_upper_state[0];
    return send_key(binding->param1, false, !state->shift_held);
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
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_lower

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static struct force_case_state force_lower_state[DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)];

static int on_force_lower_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_lower_state[0];
    state->shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, true, state->shift_held);
}

static int on_force_lower_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    struct force_case_state *state = &force_lower_state[0];
    return send_key(binding->param1, false, state->shift_held);
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
 * Always uppercase. Ignores CapsLock and Shift entirely.
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_true_upper

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_force_true_upper_binding_pressed(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, true, true);
}

static int on_force_true_upper_binding_released(struct zmk_behavior_binding *binding,
                                                struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, false, true);
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
 * Always lowercase. Ignores CapsLock and Shift entirely.
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_true_lower

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_force_true_lower_binding_pressed(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, true, false);
}

static int on_force_true_lower_binding_released(struct zmk_behavior_binding *binding,
                                                struct zmk_behavior_binding_event event) {
    return send_key(binding->param1, false, false);
}

static const struct behavior_driver_api force_true_lower_driver_api = {
    .binding_pressed  = on_force_true_lower_binding_pressed,
    .binding_released = on_force_true_lower_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_true_lower_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
