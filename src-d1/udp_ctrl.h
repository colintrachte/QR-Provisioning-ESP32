/**
 * udp_ctrl.h — Low-latency UDP control socket for ESP8266.
 *
 * Packet format is byte-identical to the ESP32-S3 version so the
 * phone app needs zero changes.
 */

#pragma once
#include <Arduino.h>

#define UDP_CTRL_PORT        4210
#define UDP_CTRL_PACKET_LEN  8
#define UDP_CTRL_MAGIC       0xA5
#define UDP_CTRL_FLAG_ARMED  0x01
#define UDP_CTRL_FLAG_ESTOP  0x02
#define UDP_CTRL_SEQ_WINDOW  5

struct udp_ctrl_packet_t {
    uint8_t seq;
    int16_t x_fp;      /* fixed-point: range -10000..10000 = -1.0..1.0 */
    int16_t y_fp;
    bool armed;
    bool e_stop;
};

/* Decode a raw UDP packet. Returns false on magic/checksum/length mismatch. */
bool udp_ctrl_decode(const uint8_t *buf, uint8_t len, udp_ctrl_packet_t *out);

/* Start listening on UDP_CTRL_PORT. Safe to call multiple times (idempotent). */
void udp_ctrl_begin(void);

/* Stop the UDP socket. */
void udp_ctrl_stop(void);

/* Poll for packets. Call from loop() at whatever rate you like;
 * packets are buffered by the WiFi stack. */
void udp_ctrl_poll(void);

bool udp_ctrl_is_running(void);
uint16_t udp_ctrl_get_port(void);

/* Stats counters (diagnostic) */
struct udp_ctrl_stats_t {
    uint32_t rx_total;
    uint32_t rx_valid;
    uint32_t rx_bad_magic;
    uint32_t rx_bad_cksum;
    uint32_t rx_bad_seq;
    uint32_t rx_estop;
};
const udp_ctrl_stats_t* udp_ctrl_get_stats(void);
