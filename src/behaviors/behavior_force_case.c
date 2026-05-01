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

#include <dt-bindings/zmk/modifiers.h>

#define ZMK_LED_CAPSLOCK_BIT BIT(1) // *** from https://github.com/darknao/zmk/blob/2fad527cc5abed5bb59b4d4a4b0ee511d0e514e9/app/src/rgb_underglow.c#L320 ***

/* Shift modifier flags covering both left and right shift */
#define ZMK_SHIFT_MODS (MOD_LSFT | MOD_RSFT)

/* -----------------------------------------------------------------------
 * Shared helper.
 *
 * want_upper: true  → produce uppercase
 *             false → produce lowercase
 *
 * We never raise fake LSHIFT press/release events onto the bus.
 *
 * Two cases to handle, both solved purely at the HID report layer:
 *
 * MASK case (natural=upper, want=lower, or natural=lower, want=upper
 *   with shift held):
 *   zmk_hid_masked_modifiers_set(ZMK_SHIFT_MODS) hides the held shift
 *   from the report. The keycode goes out bare → host sees lowercase
 *   (caps OFF) or uppercase (caps ON, shift masked out cancels the
 *   shift-cancels-caps effect... wait, that's wrong — see analysis below).
 *
 * The fundamental issue is that masked_modifiers only suppresses shift.
 * It cannot suppress CapsLock, which lives on the HOST side. So we must
 * always reason about what the host will produce given (caps, shift in
 * report) and correct from there.
 *
 * Correction table — what to put in the HID report's shift bit so the
 * host produces the desired case:
 *
 *   caps OFF, want upper → shift=1  (shift gives uppercase)
 *   caps OFF, want lower → shift=0  (bare gives lowercase)
 *   caps ON,  want upper → shift=0  (bare gives uppercase, caps does it)
 *   caps ON,  want lower → shift=1  (shift cancels caps → lowercase)
 *
 * Therefore: report_shift = caps_active XOR want_upper XOR true
 *          = caps_active XNOR want_upper
 *          = !(caps_active XOR want_upper)
 *
 * Simpler: report_shift = (caps_active == want_upper)
 *   caps OFF, want upper → 0==1 → false → wait that's wrong...
 *
 * Let's just use the explicit truth table:
 *   caps=0 want_upper=1 → shift=1
 *   caps=0 want_upper=0 → shift=0
 *   caps=1 want_upper=1 → shift=0
 *   caps=1 want_upper=0 → shift=1
 *
 * report_shift = want_upper XOR caps_active
 *
 * So regardless of what shift is physically held, we need the HID report
 * to show shift = (want_upper XOR caps_active).
 *
 * We achieve this with only HID-layer calls (no bus events):
 *   • Always mask out physical shift: zmk_hid_masked_modifiers_set()
 *   • Then if report_shift needed: zmk_hid_register_mods(MOD_LSFT)
 *     to add it back silently, undone with zmk_hid_unregister_mods()
 *     after the event.
 * ----------------------------------------------------------------------- */
static int send_key(uint32_t keycode, bool pressed, bool want_upper,
                    int64_t timestamp) {
    zmk_hid_indicators_t ind = zmk_hid_indicators_get_current_profile();
    bool caps_active = (ind & ZMK_LED_CAPSLOCK_BIT) != 0;

    /*
     * Desired shift bit in the HID report:
     *   caps OFF + want upper → shift=1
     *   caps OFF + want lower → shift=0
     *   caps ON  + want upper → shift=0
     *   caps ON  + want lower → shift=1
     * => report_shift = want_upper XOR caps_active
     */
    bool report_shift = want_upper ^ caps_active;

    /*
     * Always mask all physical shift out of the report first.
     * This neutralises whatever the user is physically holding,
     * giving us a clean slate to set the shift bit exactly as needed.
     */
    zmk_hid_masked_modifiers_set(ZMK_SHIFT_MODS);

    if (report_shift) {
        /*
         * Inject shift directly into the explicit modifier register.
         * zmk_hid_register_mods() increments the ref-count for LSFT
         * and updates the HID report — no bus event fired.
         */
        zmk_hid_register_mods(MOD_LSFT);
    }

    int ret = raise_zmk_keycode_state_changed_from_encoded(keycode, pressed, timestamp);

    /* Restore everything unconditionally so we never leave dirty state */
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
