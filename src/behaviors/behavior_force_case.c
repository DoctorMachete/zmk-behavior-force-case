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
#include <zmk/hid.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid_indicators.h>

/* -----------------------------------------------------------------------
 * Shared helper
 *
 * Determines whether LSHIFT should be added around the keycode event,
 * and whether the physical shift (if held) must be suppressed.
 *
 * Parameters:
 *   keycode      - the letter keycode to send
 *   pressed      - true = key down, false = key up
 *   need_shift   - true  = we want to produce uppercase via LSHIFT
 *                  false = we want bare keycode (no added shift)
 *   shift_held   - true if the user is physically holding LSHIFT or RSHIFT
 *   timestamp    - event timestamp
 *
 * When need_shift == false AND shift_held == true, the physical shift is
 * masked for the duration of the keycode event so the host sees no shift,
 * giving us a clean bare keycode regardless of what the user is pressing.
 * The mask is lifted immediately after, leaving the shift state intact for
 * every other concurrent or subsequent behavior.
 * ----------------------------------------------------------------------- */
static int send_key(uint32_t keycode, bool pressed, bool need_shift,
                    bool shift_held, int64_t timestamp) {
    int ret;

    if (pressed) {
        if (need_shift) {
            /*
             * We need uppercase. Add our own LSHIFT. If the user is already
             * holding shift, zmk_hid_register_mods() simply increments its
             * reference count — harmless and correct.
             */
            ret = raise_zmk_keycode_state_changed_from_encoded(LSHIFT, true,
                                                               timestamp);
            if (ret < 0) {
                return ret;
            }
        } else if (shift_held) {
            /*
             * We need a bare keycode, but physical shift is held. Mask it
             * out so the host report sees no shift during this keycode event.
             * zmk_hid_masked_mods_set() does NOT touch reference counts;
             * clearing it later fully restores the modifier state.
             */
            zmk_hid_masked_mods_set(MOD_LSFT | MOD_RSFT);
        }
    }

    ret = raise_zmk_keycode_state_changed_from_encoded(keycode, pressed,
                                                       timestamp);
    if (ret < 0) {
        /* Clean up any state we may have set before returning */
        if (pressed && need_shift) {
            raise_zmk_keycode_state_changed_from_encoded(LSHIFT, false,
                                                        timestamp);
        } else if (pressed && shift_held) {
            zmk_hid_masked_mods_clear(MOD_LSFT | MOD_RSFT);
        }
        return ret;
    }

    if (!pressed) {
        if (need_shift) {
            ret = raise_zmk_keycode_state_changed_from_encoded(LSHIFT, false,
                                                               timestamp);
        } else if (shift_held) {
            /* Lift the mask — physical shift is restored to the report */
            zmk_hid_masked_mods_clear(MOD_LSFT | MOD_RSFT);
        }
    }

    return ret;
}

/* -----------------------------------------------------------------------
 * Shared state reader
 *
 * Returns the two facts both behaviors need:
 *   *caps_active  - true if the host's CapsLock LED is on
 *   *shift_held   - true if LSHIFT or RSHIFT is physically held
 * ----------------------------------------------------------------------- */
static void read_input_state(bool *caps_active, bool *shift_held) {
    zmk_hid_indicators_t ind = zmk_hid_indicators_get_current_profile();
    *caps_active = (ind & ZMK_HID_INDICATORS_CAPSLOCK) != 0;

    zmk_mods_t mods = zmk_hid_get_explicit_mods();
    *shift_held = (mods & (MOD_LSFT | MOD_RSFT)) != 0;
}

/* -----------------------------------------------------------------------
 * FORCE-UPPER  (fucase)
 *
 * Default: always produce uppercase.
 * With shift held: invert → always produce lowercase.
 *
 *   need_shift = !caps_active XOR shift_held
 * ----------------------------------------------------------------------- */
#define DT_DRV_COMPAT zmk_behavior_force_upper

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_fucase_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    bool caps_active, shift_held;
    read_input_state(&caps_active, &shift_held);
    bool need_shift = (!caps_active) ^ shift_held;
    return send_key(binding->param1, true, need_shift, shift_held,
                    event.timestamp);
}

static int on_fucase_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    bool caps_active, shift_held;
    read_input_state(&caps_active, &shift_held);
    bool need_shift = (!caps_active) ^ shift_held;
    return send_key(binding->param1, false, need_shift, shift_held,
                    event.timestamp);
}

static const struct behavior_driver_api force_upper_driver_api = {
    .binding_pressed  = on_fucase_pressed,
    .binding_released = on_fucase_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_upper_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

/* -----------------------------------------------------------------------
 * FORCE-LOWER  (flcase)
 *
 * Default: always produce lowercase.
 * With shift held: invert → always produce uppercase.
 *
 *   need_shift = caps_active XOR shift_held
 * ----------------------------------------------------------------------- */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_force_lower

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_flcase_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    bool caps_active, shift_held;
    read_input_state(&caps_active, &shift_held);
    bool need_shift = caps_active ^ shift_held;
    return send_key(binding->param1, true, need_shift, shift_held,
                    event.timestamp);
}

static int on_flcase_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    bool caps_active, shift_held;
    read_input_state(&caps_active, &shift_held);
    bool need_shift = caps_active ^ shift_held;
    return send_key(binding->param1, false, need_shift, shift_held,
                    event.timestamp);
}

static const struct behavior_driver_api force_lower_driver_api = {
    .binding_pressed  = on_flcase_pressed,
    .binding_released = on_flcase_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &force_lower_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
