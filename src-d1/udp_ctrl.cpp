/**
 * udp_ctrl.cpp — Low-latency UDP control socket for ESP8266.
 *
 * Task model: cooperative polling from loop().
 * The ESP8266 has no second core, so a FreeRTOS task would just
 * preempt loop() anyway. Polling is simpler and uses less RAM.
 *
 * Coexistence with WebSocket: both call ctrl_drive_set_axes() and
 * ctrl_drive_feed_watchdog(). Whichever delivers a frame last wins.
 */

#include "udp_ctrl.h"
#include <WiFiUdp.h>

/* These hooks are provided by the motor/controller layer in main_d1mini.cpp */
extern void ctrl_drive_feed_watchdog(void);
extern void ctrl_drive_set_axes(float x, float y);
extern void ctrl_drive_emergency_stop(void);
extern void ctrl_drive_set_armed(bool armed);
extern bool ctrl_drive_is_armed(void);
extern void o_led_blink(int pattern);
extern void o_led_set(float brightness);

#define LED_PATTERN_HEARTBEAT   5
#define LED_PATTERN_SLOW_BLINK  2

static const char *TAG = "udp_ctrl";

static WiFiUDP           s_udp;
static bool              s_running  = false;
static uint16_t          s_port     = 0;
static uint8_t           s_last_seq = 0;
static bool              s_seq_init = false;
static udp_ctrl_stats_t  s_stats    = {0};

bool udp_ctrl_decode(const uint8_t *buf, uint8_t len, udp_ctrl_packet_t *out)
{
    if (len != UDP_CTRL_PACKET_LEN)           return false;
    if (buf[0] != UDP_CTRL_MAGIC)             return false;

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

static bool seq_accept(uint8_t seq)
{
    if (!s_seq_init) {
        s_last_seq = seq;
        s_seq_init = true;
        return true;
    }
    uint8_t dist = (uint8_t)((int16_t)seq - (int16_t)s_last_seq);
    if (dist == 0 || dist > UDP_CTRL_SEQ_WINDOW) return false;
    s_last_seq = seq;
    return true;
}

static void dispatch(const udp_ctrl_packet_t *pkt, IPAddress src)
{
    if (pkt->e_stop) {
        s_stats.rx_estop++;
        ctrl_drive_emergency_stop();
        ctrl_drive_set_armed(false);
        Serial.printf("[%s] E-STOP from %s\n", TAG, src.toString().c_str());
        return;
    }

    bool currently_armed = ctrl_drive_is_armed();
    if (pkt->armed && !currently_armed) {
        ctrl_drive_set_armed(true);
        o_led_blink(LED_PATTERN_HEARTBEAT);
        Serial.printf("[%s] Armed via UDP from %s\n", TAG, src.toString().c_str());
    } else if (!pkt->armed && currently_armed) {
        ctrl_drive_set_armed(false);
        o_led_blink(LED_PATTERN_SLOW_BLINK);
        Serial.printf("[%s] Disarmed via UDP\n", TAG);
    }

    ctrl_drive_feed_watchdog();
    ctrl_drive_set_axes(pkt->x_fp * (1.0f / 10000.0f),
                        pkt->y_fp * (1.0f / 10000.0f));
}

void udp_ctrl_begin(void)
{
    if (s_running) {
        Serial.printf("[%s] Already running on port %u\n", TAG, s_port);
        return;
    }
    memset(&s_stats, 0, sizeof(s_stats));
    s_seq_init = false;

    s_udp.begin(UDP_CTRL_PORT);
    s_port    = UDP_CTRL_PORT;
    s_running = true;
    Serial.printf("[%s] UDP control socket bound on port %u\n", TAG, s_port);
}

void udp_ctrl_stop(void)
{
    if (!s_running) return;
    s_running = false;
    ctrl_drive_emergency_stop();
    ctrl_drive_set_armed(false);
    s_udp.stop();
    s_port = 0;
    Serial.printf("[%s] UDP control stopped. Stats: valid=%lu bad_cksum=%lu "
                  "bad_seq=%lu estop=%lu\n",
                  TAG, s_stats.rx_valid, s_stats.rx_bad_cksum,
                  s_stats.rx_bad_seq, s_stats.rx_estop);
}

void udp_ctrl_poll(void)
{
    if (!s_running) return;

    int len = s_udp.parsePacket();
    if (!len) return;

    uint8_t buf[64];
    int n = s_udp.read(buf, sizeof(buf));
    IPAddress src = s_udp.remoteIP();

    s_stats.rx_total++;

    if (n != UDP_CTRL_PACKET_LEN) {
        /* Wrong size -- might be a broadcast probe */
        return;
    }
    if (buf[0] != UDP_CTRL_MAGIC) {
        s_stats.rx_bad_magic++;
        return;
    }

    udp_ctrl_packet_t pkt;
    if (!udp_ctrl_decode(buf, (uint8_t)n, &pkt)) {
        s_stats.rx_bad_cksum++;
        return;
    }

    if (!seq_accept(pkt.seq)) {
        s_stats.rx_bad_seq++;
        return;
    }

    s_stats.rx_valid++;
    dispatch(&pkt, src);
}

bool udp_ctrl_is_running(void) { return s_running; }
uint16_t udp_ctrl_get_port(void) { return s_port; }
const udp_ctrl_stats_t* udp_ctrl_get_stats(void) { return &s_stats; }
