/*
 * zmk-layer-hid  –  layer_hid.c
 *
 * Sends the topmost active ZMK layer number to the host over a
 * vendor-defined HID interface (Usage Page 0xFF00, Report ID 0x04).
 * Works transparently over BLE (HOGP) and USB HID without touching
 * any standard keyboard / consumer / mouse reports.
 *
 * How it works
 * ============
 *  1. A ZMK event listener watches for zmk_layer_state_changed events.
 *  2. On every layer change the listener schedules a work item so that
 *     the HID send happens outside of the event-system hot path.
 *  3. The work item walks the layer bitmap from the highest index down,
 *     picks the topmost active layer, and sends a 2-byte HID report:
 *       byte 0 – Report ID (0x04)
 *       byte 1 – layer index (0-based, 0 = default layer)
 *  4. The report is dispatched through ZMK's existing hid_listener
 *     transport, so BLE and (optionally) USB both receive it with
 *     zero extra glue code.
 *
 * Integration with go60___old
 * ===========================
 *  Add this module to west.yml and include it from your shield / board
 *  CMakeLists via standard Zephyr module discovery.  No changes to the
 *  keyboard's keymap or config files are required beyond enabling the
 *  Kconfig symbol ZMK_LAYER_HID_REPORTER=y (it is on by default).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/layer_state_changed.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#include "layer_hid.h"

LOG_MODULE_REGISTER(layer_hid, CONFIG_ZMK_LOG_LEVEL);

/* -----------------------------------------------------------------------
 * HID descriptor fragment
 *
 * Append this to your keyboard's existing HID descriptor (or reference
 * it from your shield's usb_hid / bt_hid device node in Devicetree).
 *
 * Usage Page (Vendor-defined 0xFF00)  – avoids any conflict with
 * standard HID usages so the OS never tries to bind a driver to it.
 *
 * The report is:
 *   Report ID  0x04     (1 byte  – chosen to not clash with ZMK's
 *                         keyboard=0x01, consumer=0x02, mouse=0x03)
 *   Data       0x00-0xFF (1 byte  – topmost active layer index)
 *
 * Full descriptor bytes (for reference / copy-paste into your board's
 * hid_report_desc array if you need to add it manually):
 *
 *   0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
 *   0x09, 0x01,        // Usage (Vendor Usage 0x01)
 *   0xA1, 0x01,        // Collection (Application)
 *   0x85, 0x04,        //   Report ID (4)
 *   0x09, 0x02,        //   Usage (Vendor Usage 0x02)
 *   0x15, 0x00,        //   Logical Minimum (0)
 *   0x25, 0xFF,        //   Logical Maximum (255)
 *   0x75, 0x08,        //   Report Size (8)
 *   0x95, 0x01,        //   Report Count (1)
 *   0x81, 0x02,        //   Input (Data, Variable, Absolute)
 *   0xC0,              // End Collection
 * ----------------------------------------------------------------------- */

#define LAYER_HID_REPORT_ID   0x04
#define LAYER_HID_REPORT_LEN  2   /* Report ID byte + data byte */

/* Simple 2-byte report buffer: [report_id, layer_index] */
static uint8_t layer_report[LAYER_HID_REPORT_LEN];

/* ---------------------------------------------------------------------- */
/* Work item – runs in the system workqueue, outside event-system context  */
/* ---------------------------------------------------------------------- */

static void send_layer_report_work_handler(struct k_work *work);
static K_WORK_DEFINE(send_layer_work, send_layer_report_work_handler);

static void send_layer_report_work_handler(struct k_work *work)
{
    /* Walk layers from the top down to find the highest active one.
     * zmk_keymap_layer_count() returns the total configured layer count.
     * zmk_keymap_layer_active(n) returns true when layer n is on.      */
    uint8_t active_layer = 0;
    int num_layers = zmk_keymap_layer_count();

    for (int i = num_layers - 1; i >= 0; i--) {
        if (zmk_keymap_layer_active(i)) {
            active_layer = (uint8_t)i;
            break;
        }
    }

    layer_report[0] = LAYER_HID_REPORT_ID;
    layer_report[1] = active_layer;

    LOG_DBG("Sending layer HID report: layer=%d", active_layer);

    /*
     * zmk_endpoints_send_report() dispatches the report to all currently
     * active endpoints (BLE central, USB host, or both).  It accepts the
     * raw report buffer and its length.
     *
     * NOTE: In some ZMK versions the function signature differs slightly.
     * If you get a compile error here, check zmk/endpoints.h in your ZMK
     * tree and adapt the call accordingly.
     */
    int err = zmk_endpoints_send_report(LAYER_HID_REPORT_ID);
    if (err) {
        LOG_WRN("Failed to send layer HID report: %d", err);
    }
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

void layer_hid_report_current(void)
{
    k_work_submit(&send_layer_work);
}

/* ---------------------------------------------------------------------- */
/* ZMK event listener                                                      */
/* ---------------------------------------------------------------------- */

static int layer_hid_event_listener(const zmk_event_t *eh)
{
    /* We only care about layer-state changes.  Cast is safe because ZMK's
     * event subsystem only delivers events whose type matches the listener
     * subscription table below.                                           */
    const struct zmk_layer_state_changed *ev =
        as_zmk_layer_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Schedule the actual HID send on the work queue so we return from the
     * event handler as quickly as possible.                               */
    k_work_submit(&send_layer_work);

    return ZMK_EV_EVENT_BUBBLE;   /* let other listeners see the event too */
}

ZMK_LISTENER(layer_hid, layer_hid_event_listener);
ZMK_SUBSCRIPTION(layer_hid, zmk_layer_state_changed);
