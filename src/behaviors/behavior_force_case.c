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

/* Shift modifier flags covering both left and right shift */
#define ZMK_SHIFT_MODS (MOD_LSFT | MOD_RSFT)

/* -----------------------------------------------------------------------
 * Shared helper.
 *
 * want_upper: true  → produce uppercase regardless of CapsLock
 *             false → produce lowercase regardless of CapsLock
 *
 * We bypass the ZMK event bus entirely and drive the HID report layer
 * directly — exactly what hid_listener.c does, but with full control
 * over the shift bit.
 *
 * This avoids every race between masked_modifiers/implicit_modifiers and
 * hid_listener's SET_MODIFIERS recalculation.
 *
 * Desired shift bit in the HID report:
 *   caps OFF + want upper → shift=1   (shift produces uppercase)
 *   caps OFF + want lower → shift=0   (bare  produces lowercase)
 *   caps ON  + want upper → shift=0   (bare  produces uppercase, caps does it)
 *   caps ON  + want lower → shift=1   (shift cancels caps → lowercase)
 *
 *   report_shift = want_upper XOR caps_active
 *
 * Physical shift is completely ignored for the keycode itself — it only
 * determines want_upper at the call site (binding handlers).  We mask it
 * out of the report so it has zero effect on the character produced.
 * ----------------------------------------------------------------------- */
static int send_key(uint32_t keycode, bool pressed, bool want_upper,
                    int64_t timestamp) {
    zmk_hid_indicators_t ind = zmk_hid_indicators_get_current_profile();
    bool caps_active  = (ind & ZMK_LED_CAPSLOCK_BIT) != 0;
    bool report_shift = want_upper ^ caps_active;

    /*
     * Step 1 — strip all physical shift out of the report.
     * zmk_hid_masked_modifiers_set suppresses explicit shift from
     * the report without touching the event bus.
     */
    zmk_hid_masked_modifiers_set(ZMK_SHIFT_MODS);

    /*
     * Step 2 — press or release the keycode directly in the HID layer.
     * zmk_hid_press/release update keyboard_report.body.keys but do NOT
     * recalculate modifiers, so our mask is still in effect.
     */
    int ret;
    if (pressed) {
        ret = zmk_hid_press(ZMK_HID_USAGE(HID_USAGE_KEY, keycode));
    } else {
        ret = zmk_hid_release(ZMK_HID_USAGE(HID_USAGE_KEY, keycode));
    }
    if (ret < 0) {
        zmk_hid_masked_modifiers_clear();
        return ret;
    }

    /*
     * Step 3 — set the shift modifier in the report to exactly what we
     * need, using register/unregister which update explicit_modifiers
     * and immediately recalculate the report via SET_MODIFIERS.
     * Because the mask is still active, ONLY our injected shift (if any)
     * appears in the outgoing report — physical shift remains suppressed.
     */
    if (report_shift) {
        zmk_hid_register_mods(MOD_LSFT);
    }

    /*
     * Step 4 — send the report to the host now, while the modifier
     * state is exactly as we want it.
     */
    ret = zmk_endpoints_send_report(HID_USAGE_KEY);

    /*
     * Step 5 — restore everything unconditionally.
     * Remove our injected shift first, then clear the mask so physical
     * shift reappears in the report for subsequent keypresses.
     */
    if (report_shift) {
        zmk_hid_unregister_mods(MOD_LSFT);
    }
    zmk_hid_masked_modifiers_clear();

    return ret;
}

/* -----------------------------------------------------------------------
 * FORCE-UPPER
 * Goal: always produce uppercase. With shift held, produce lowercase.
 *
 * shift not held → want_upper = true
 * shift held     → want_upper = false  (shift inverts)
 * ----------------------------------------------------------------------- */
#define DT_DRV_COMPAT zmk_behavior_force_upper

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_force_upper_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, true, !shift_held, event.timestamp);
}

static int on_force_upper_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, false, !shift_held, event.timestamp);
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
 * FORCE-LOWER
 * Goal: always produce lowercase. With shift held, produce uppercase.
 *
 * shift not held → want_upper = false
 * shift held     → want_upper = true   (shift inverts)
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_lower

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_force_lower_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, true, shift_held, event.timestamp);
}

static int on_force_lower_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, false, shift_held, event.timestamp);
}

static const struct behavior_driver_api force_lower_driver_api = {
    .binding_pressed  = on_force_lower_binding_pressed,
    .binding_released = on_force_lower_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_lower_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
