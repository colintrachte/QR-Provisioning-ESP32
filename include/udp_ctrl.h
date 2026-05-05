/**
 * udp_ctrl.h — Low-latency UDP control socket for drive axes.
 *
 * Complements the WebSocket path. Intended for native clients (Python
 * scripts, ROS nodes, gamepad daemons) where raw UDP is available.
 * Browser clients continue to use WebSocket.
 *
 * Packet format (8 bytes, all little-endian)
 * ──────────────────────────────────────────
 *  [0]     uint8   magic     0xA5 — rejects stray broadcast traffic
 *  [1]     uint8   seq       wraps 0–255; receiver drops out-of-order
 *  [2..3]  int16   x_fp      x-axis × 10000  (range −10000..10000)
 *  [4..5]  int16   y_fp      y-axis × 10000  (range −10000..10000)
 *  [6]     uint8   flags     bit 0 = armed, bit 1 = e-stop, bits 2–7 reserved
 *  [7]     uint8   checksum  XOR of bytes [0..6]
 *
 * Fixed-point scale matches ctrl_drive internals (AXES_SCALE = 10000) so
 * x_fp and y_fp are forwarded directly with no float conversion in the hot
 * path — the receiver divides by 10000 only when calling ctrl_drive_set_axes().
 *
 * Flag semantics
 * ──────────────
 *  armed=1, e_stop=0  → arm + set axes (normal driving)
 *  armed=0, e_stop=0  → disarm (motors idle, watchdog still fed)
 *  e_stop=1           → emergency stop regardless of armed bit;
 *                        client must send armed=1 again to resume
 *
 * Out-of-order rejection
 * ──────────────────────
 * The receiver tracks the last accepted seq. A packet is accepted only if
 *   (seq - last_seq) & 0xFF  < UDP_CTRL_SEQ_WINDOW
 * This handles the wrap-around at 255→0 and rejects packets that arrive
 * more than SEQ_WINDOW steps behind the current head (stale network buffer).
 * Default window = 64 (half the seq space), tunable below.
 *
 * Port
 * ────
 * Default UDP_CTRL_PORT 4210. Override in config.h or via settings_mgr
 * (udp_ctrl_port, 0 = disabled at runtime).
 *
 * Security
 * ────────
 * No authentication beyond the magic byte and checksum. Appropriate for
 * trusted LAN use only — same threat model as the HTTP control page.
 * A shared-secret HMAC variant is left as a future exercise.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Packet constants ───────────────────────────────────────────────────────*/
#define UDP_CTRL_MAGIC       0xA5u
#define UDP_CTRL_PACKET_LEN  8u
#define UDP_CTRL_SEQ_WINDOW  64u   /* accept seq within this many steps ahead */

/* Flags byte bit positions */
#define UDP_CTRL_FLAG_ARMED   (1u << 0)
#define UDP_CTRL_FLAG_ESTOP   (1u << 1)

/* ── Default port ───────────────────────────────────────────────────────────
 * Can be overridden in config.h. Runtime override via settings_mgr field
 * udp_ctrl_port (0 = disable). */
#ifndef UDP_CTRL_PORT
#  define UDP_CTRL_PORT  4210
#endif

/* ── Parsed packet ──────────────────────────────────────────────────────────*/
typedef struct {
    uint8_t  seq;
    int16_t  x_fp;    /* ×10000 fixed-point, range −10000..10000 */
    int16_t  y_fp;
    bool     armed;
    bool     e_stop;
} udp_ctrl_packet_t;

/* ── Public API ─────────────────────────────────────────────────────────────*/

/**
 * Start the UDP control socket and receiver task.
 *
 * Binds to INADDR_ANY on UDP_CTRL_PORT (or settings_get()->udp_ctrl_port
 * if that field exists). Spawns udp_ctrl_task pinned to Core 1 alongside
 * app_task so UDP receive never contends with the httpd on Core 0.
 *
 * Call after wifi_manager reports STA connected (same timing as app_server_start).
 * Safe to call multiple times — subsequent calls are no-ops if already running.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (already running), or socket error.
 */
esp_err_t udp_ctrl_start(void);

/**
 * Stop the receiver task and close the socket.
 * Disarms drive and triggers an emergency stop before closing.
 */
void udp_ctrl_stop(void);

/** True if the receiver task is running and socket is bound. */
bool udp_ctrl_is_running(void);

/**
 * Return the port currently bound, or 0 if not running.
 * Useful for logging and the /api/status endpoint.
 */
uint16_t udp_ctrl_get_port(void);

/**
 * Decode and validate a raw 8-byte buffer into a udp_ctrl_packet_t.
 * Returns true if magic, length, and checksum are valid.
 * Pure function — no side effects. Exposed for unit testing.
 */
bool udp_ctrl_decode(const uint8_t *buf, uint8_t len, udp_ctrl_packet_t *out);
