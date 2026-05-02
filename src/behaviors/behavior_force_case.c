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

/* -----------------------------------------------------------------------
 * Sticky shift mechanics:
 *
 * When &sk LSFT fires, it raises a keycode_state_changed PRESS for LSHIFT
 * on the event bus. hid_listener processes it and calls zmk_hid_register_mod(),
 * putting it into explicit_modifiers. It then waits for the next key's
 * keycode_state_changed RELEASE event, at which point it raises its own
 * LSHIFT release event to clean up.
 *
 * Because we bypass the event bus (zmk_hid_press + zmk_endpoints_send_report
 * directly), sticky key never sees a keycode_state_changed release for our
 * key and never fires its own LSHIFT release. Shift gets stuck.
 *
 * Fix: after sending our report, raise a keycode_state_changed RELEASE for
 * LSHIFT on the bus. This is exactly what sticky key is listening for —
 * it will fire its cleanup and clear LSHIFT from explicit_modifiers cleanly.
 * This release event is only raised if shift was actually held (sticky or
 * physical) so we don't inject spurious releases.
 *
 * We snapshot shift_held BEFORE masking, use it to determine want_upper
 * at the call site, and pass it into send_key so it knows whether to
 * fire the release event.
 * ----------------------------------------------------------------------- */
static int send_key(uint32_t keycode, bool pressed, bool want_upper, bool shift_was_held) {
    zmk_hid_indicators_t ind = zmk_hid_indicators_get_current_profile();
    bool caps_active  = (ind & ZMK_LED_CAPSLOCK_BIT) != 0;
    bool report_shift = want_upper ^ caps_active;

    /* Mask all physical/sticky shift out of the report */
    zmk_hid_masked_modifiers_set(ZMK_SHIFT_MODS);

    /* Inject shift via implicit if needed — survives the mask */
    if (report_shift) {
        zmk_hid_implicit_modifiers_press(MOD_LSFT);
    }

    /* Update key state directly, bypassing the event bus */
    int ret;
    if (pressed) {
        ret = zmk_hid_press(ZMK_HID_USAGE(HID_USAGE_KEY, keycode));
    } else {
        ret = zmk_hid_release(ZMK_HID_USAGE(HID_USAGE_KEY, keycode));
    }

    if (ret == 0) {
        ret = zmk_endpoints_send_report(HID_USAGE_KEY);
    }

    /* Restore implicit and mask unconditionally */
    if (report_shift) {
        zmk_hid_implicit_modifiers_release();
    }
    zmk_hid_masked_modifiers_clear();

    /*
     * On key RELEASE: if shift was held when the key was pressed, raise a
     * LSHIFT release event on the bus now. This is what sticky key is
     * waiting for — it will respond by releasing its hold on LSHIFT and
     * clearing it from explicit_modifiers. Without this, sticky shift
     * never releases and gets stuck.
     *
     * We do this on release (not press) to mirror normal key sequencing:
     * sticky key releases its modifier after the modified key is released.
     */
    if (!pressed && shift_was_held) {
        raise_zmk_keycode_state_changed_from_encoded(LSHIFT, false, k_uptime_get());
    }

    return ret;
}

/* -----------------------------------------------------------------------
 * FORCE-UPPER (fucase)
 * Ignores CapsLock. Shift inverts: no shift → upper, shift → lower.
 * ----------------------------------------------------------------------- */
#define DT_DRV_COMPAT zmk_behavior_force_upper

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_force_upper_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, true, !shift_held, shift_held);
}

static int on_force_upper_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, false, !shift_held, shift_held);
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

static int on_force_lower_binding_pressed(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, true, shift_held, shift_held);
}

static int on_force_lower_binding_released(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    bool shift_held = (zmk_hid_get_explicit_mods() & ZMK_SHIFT_MODS) != 0;
    return send_key(binding->param1, false, shift_held, shift_held);
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
