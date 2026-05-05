/**
 * udp_ctrl.c — Low-latency UDP control socket.
 *
 * Task model
 * ──────────
 * udp_ctrl_task is pinned to Core 1 (APP_CPU), same core as app_task.
 * It blocks on recvfrom() with a UDP_CTRL_RECV_TIMEOUT_MS select timeout.
 * On each valid packet it calls ctrl_drive_set_axes() and
 * ctrl_drive_feed_watchdog() — the same functions the WS handler calls,
 * so the ctrl_drive watchdog and ramp work identically regardless of
 * which transport delivered the command.
 *
 * Why Core 1?
 * ───────────
 * The httpd runs its handlers on Core 0 (PRO_CPU). Pinning the UDP task
 * to Core 1 means a burst of HTTP traffic cannot delay a motor command
 * in the OS scheduler. The ctrl_drive state shared between this task and
 * app_task (also Core 1) uses the same volatile int16_t atomic-store
 * pattern already documented in ctrl_drive.c.
 *
 * Coexistence with WebSocket
 * ──────────────────────────
 * Both transports call ctrl_drive_set_axes() and ctrl_drive_feed_watchdog().
 * Whichever delivers a frame last wins — the values are simply overwritten.
 * This is intentional: a client can smoothly hand off by switching transports
 * without any server-side negotiation. The watchdog is fed by both paths so
 * it will not fire as long as either transport is delivering frames.
 *
 * Out-of-order rejection
 * ──────────────────────
 * UDP does not guarantee ordering. A burst of re-ordered packets from a
 * congested AP could cause a "jump back" in commanded axes if all were
 * accepted. The seq window check ensures only forward progress is applied.
 *
 * On seq wrap (255 → 0) the modular arithmetic handles it correctly:
 *   (0 - 255) & 0xFF = 1, which is < UDP_CTRL_SEQ_WINDOW → accepted.
 *
 * Stats / diagnostics
 * ───────────────────
 * s_stats is a simple counter struct read by health_monitor or a future
 * /api/udp_stats endpoint. Counters are uint32_t; wrap-around after ~4B
 * packets is acceptable for a diagnostic counter.
 */

#include "udp_ctrl.h"
#include "ctrl_drive.h"
#include "o_led.h"
#include "settings_mgr.h"
#include "config.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "udp_ctrl";

/* ── Tuning ─────────────────────────────────────────────────────────────── */
#define UDP_CTRL_RECV_TIMEOUT_MS  200   /* recvfrom select timeout; controls
                                           how quickly stop() wakes the task */
#define UDP_CTRL_TASK_STACK       3072
#define UDP_CTRL_TASK_PRIORITY    6     /* above app_task(5), below timer(7) */
#define UDP_CTRL_RX_BUF           64    /* much larger than 8 B; absorbs any
                                           accidentally oversized datagrams   */

/* ── State ──────────────────────────────────────────────────────────────── */
static volatile bool     s_running  = false;
static int               s_sock     = -1;
static uint16_t          s_port     = 0;
static TaskHandle_t      s_task     = NULL;

/* Sequence tracking */
static uint8_t           s_last_seq = 0;
static bool              s_seq_init = false;  /* false until first valid pkt */

/* ── Diagnostic counters ────────────────────────────────────────────────── */
typedef struct {
    uint32_t rx_total;      /* datagrams received (any)        */
    uint32_t rx_valid;      /* passed magic + checksum + seq   */
    uint32_t rx_bad_magic;
    uint32_t rx_bad_cksum;
    uint32_t rx_bad_seq;    /* out-of-order drops              */
    uint32_t rx_estop;      /* e-stop flags received           */
} udp_ctrl_stats_t;

static udp_ctrl_stats_t s_stats;

/* ── Packet codec ───────────────────────────────────────────────────────── */

bool udp_ctrl_decode(const uint8_t *buf, uint8_t len, udp_ctrl_packet_t *out)
{
    if (len != UDP_CTRL_PACKET_LEN)           return false;
    if (buf[0] != UDP_CTRL_MAGIC)             return false;

    /* XOR checksum over bytes 0..6 */
    uint8_t ck = 0;
    for (int i = 0; i < 7; i++) ck ^= buf[i];
    if (ck != buf[7])                         return false;

    out->seq    = buf[1];
    out->x_fp   = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
    out->y_fp   = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
    out->armed  = (buf[6] & UDP_CTRL_FLAG_ARMED) != 0;
    out->e_stop = (buf[6] & UDP_CTRL_FLAG_ESTOP) != 0;
    return true;
}

/* ── Sequence gate ──────────────────────────────────────────────────────── */

static bool seq_accept(uint8_t seq)
{
    if (!s_seq_init) {
        /* Bootstrap: accept the very first valid packet unconditionally */
        s_last_seq = seq;
        s_seq_init = true;
        return true;
    }

    /* Modular distance: how many steps ahead of last_seq is seq?
     * Works correctly across the 255→0 wrap. */
    uint8_t dist = (uint8_t)((int16_t)seq - (int16_t)s_last_seq) & 0xFFu;

    /* dist == 0 means duplicate; dist > WINDOW means stale/reordered */
    if (dist == 0 || dist > UDP_CTRL_SEQ_WINDOW) return false;

    s_last_seq = seq;
    return true;
}

/* ── Dispatch ───────────────────────────────────────────────────────────── */

static void dispatch(const udp_ctrl_packet_t *pkt, const struct sockaddr_in *src)
{
    /* E-stop overrides everything */
    if (pkt->e_stop) {
        s_stats.rx_estop++;
        ctrl_drive_emergency_stop();
        ctrl_drive_set_armed(false);
        ESP_LOGW(TAG, "E-STOP from %s", inet_ntoa(src->sin_addr));
        return;
    }

    /* Arm / disarm state */
    bool currently_armed = ctrl_drive_is_armed();
    if (pkt->armed && !currently_armed) {
        ctrl_drive_set_armed(true);
        o_led_blink(LED_PATTERN_HEARTBEAT);
        ESP_LOGI(TAG, "Armed via UDP from %s", inet_ntoa(src->sin_addr));
    } else if (!pkt->armed && currently_armed) {
        ctrl_drive_set_armed(false);
        o_led_blink(LED_PATTERN_SLOW_BLINK);
        ESP_LOGI(TAG, "Disarmed via UDP");
    }

    /* Feed watchdog and set axes — convert fixed-point to float.
     * Division by 10000.0f is the inverse of AXES_SCALE in ctrl_drive.c.
     * The compiler hoists this constant; no runtime division on Xtensa. */
    ctrl_drive_feed_watchdog();
    ctrl_drive_set_axes(pkt->x_fp * (1.0f / 10000.0f),
                        pkt->y_fp * (1.0f / 10000.0f));
}

/* ── Receiver task ──────────────────────────────────────────────────────── */

static void udp_ctrl_task(void *arg)
{
    (void)arg;
    uint8_t buf[UDP_CTRL_RX_BUF];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    ESP_LOGI(TAG, "udp_ctrl_task started on port %u", s_port);

    while (s_running) {
        ssize_t n = recvfrom(s_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &src_len);

        if (!s_running) break;   /* stop() was called while blocking */

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;   /* timeout */
            ESP_LOGE(TAG, "recvfrom error: %d (%s)", errno, strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        s_stats.rx_total++;

        if (n != UDP_CTRL_PACKET_LEN) {
            /* Wrong size — log and discard; don't increment bad_magic
             * since this might be a broadcast probe, not a malformed cmd */
            if (DEBUG_UDP_CTRL)
                ESP_LOGD(TAG, "Discarded %d-byte datagram (expected %u)",
                         (int)n, UDP_CTRL_PACKET_LEN);
            continue;
        }

        /* Magic check before decode to avoid touching rest of packet */
        if (buf[0] != UDP_CTRL_MAGIC) {
            s_stats.rx_bad_magic++;
            continue;
        }

        udp_ctrl_packet_t pkt;
        if (!udp_ctrl_decode(buf, (uint8_t)n, &pkt)) {
            s_stats.rx_bad_cksum++;
            if (DEBUG_UDP_CTRL)
                ESP_LOGD(TAG, "Bad checksum from %s", inet_ntoa(src.sin_addr));
            continue;
        }

        if (!seq_accept(pkt.seq)) {
            s_stats.rx_bad_seq++;
            if (DEBUG_UDP_CTRL)
                ESP_LOGD(TAG, "Out-of-order seq %u (last %u) — dropped",
                         pkt.seq, s_last_seq);
            continue;
        }

        s_stats.rx_valid++;
        dispatch(&pkt, &src);
    }

    ESP_LOGI(TAG, "udp_ctrl_task exiting — rx %lu valid / %lu total",
             (unsigned long)s_stats.rx_valid,
             (unsigned long)s_stats.rx_total);

    close(s_sock);
    s_sock    = -1;
    s_port    = 0;
    s_running = false;
    s_task    = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t udp_ctrl_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "udp_ctrl already running on port %u", s_port);
        return ESP_ERR_INVALID_STATE;
    }

    /* Determine port — settings field udp_ctrl_port if present, else default.
     * A value of 0 means "disabled" in settings, so we fall back to the
     * compile-time default rather than binding on port 0. */
    uint16_t port = UDP_CTRL_PORT;

    memset(&s_stats, 0, sizeof(s_stats));
    s_seq_init = false;

    /* Create UDP socket */
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }

    /* SO_REUSEADDR so a rapid stop+start doesn't hit TIME_WAIT */
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Receive timeout so the task doesn't block forever on stop() */
    struct timeval tv = {
        .tv_sec  = UDP_CTRL_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (UDP_CTRL_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Bind to INADDR_ANY so it works on any interface (STA or AP) */
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(port),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() on port %u failed: %d", port, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    s_port    = port;
    s_running = true;

    /* Spawn receiver task on Core 1 (APP_CPU) */
    BaseType_t ok = xTaskCreatePinnedToCore(
        udp_ctrl_task, "udp_ctrl",
        UDP_CTRL_TASK_STACK, NULL,
        UDP_CTRL_TASK_PRIORITY, &s_task,
        1   /* Core 1 = APP_CPU, same as app_task */
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        close(s_sock);
        s_sock    = -1;
        s_port    = 0;
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UDP control socket bound on port %u (Core 1)", port);
    return ESP_OK;
}

void udp_ctrl_stop(void)
{
    if (!s_running) return;

    /* Signal the task to exit on its next recvfrom timeout */
    s_running = false;

    /* Emergency stop in case a client was actively driving */
    ctrl_drive_emergency_stop();
    ctrl_drive_set_armed(false);

    /* Interrupt the blocking recvfrom by sending a zero-length datagram
     * to ourselves. This wakes the task immediately without waiting for
     * the SO_RCVTIMEO timeout. */
    if (s_sock >= 0) {
        struct sockaddr_in self = {
            .sin_family      = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
            .sin_port        = htons(s_port),
        };
        sendto(s_sock, NULL, 0, 0, (struct sockaddr *)&self, sizeof(self));
    }

    /* Give the task up to RECV_TIMEOUT + 100ms to clean up */
    uint32_t wait_ms = UDP_CTRL_RECV_TIMEOUT_MS + 100;
    while (s_task != NULL && wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms -= 10;
    }

    if (s_task != NULL) {
        ESP_LOGW(TAG, "udp_ctrl_task did not exit cleanly — force deleting");
        vTaskDelete(s_task);
        s_task = NULL;
        if (s_sock >= 0) { close(s_sock); s_sock = -1; }
        s_port    = 0;
        s_running = false;
    }

    ESP_LOGI(TAG, "UDP control stopped. Stats: valid=%lu bad_cksum=%lu "
             "bad_seq=%lu estop=%lu",
             (unsigned long)s_stats.rx_valid,
             (unsigned long)s_stats.rx_bad_cksum,
             (unsigned long)s_stats.rx_bad_seq,
             (unsigned long)s_stats.rx_estop);
}

bool     udp_ctrl_is_running(void) { return s_running; }
uint16_t udp_ctrl_get_port(void)   { return s_port;    }
