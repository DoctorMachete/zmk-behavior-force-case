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
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
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

/*
 * A virtual position safely above any real key position on any board.
 * Used to raise synthetic position events that trigger sticky key release
 * without touching any real key binding.
 */
#define FORCE_CASE_VIRTUAL_POSITION 0xFFFF

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
 *   → key
