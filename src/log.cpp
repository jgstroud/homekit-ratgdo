// Copyright 2023 Brandon Matthews <thenewwazoo@optimaltour.us>
// All rights reserved. GPLv3 License

#include <stdint.h>

#include "log.h"
#include "secplus2.h"
#include "wifi.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <WiFiUdp.h>

#define SYSLOG_SERVER "syslog"
#define SYSLOG_PORT 514

#define DEVICE_HOSTNAME "ratgdo"
#define APP_NAME "ratgdo"

WiFiUDP udpClient;

Syslog syslog(udpClient, SYSLOG_SERVER, SYSLOG_PORT, DEVICE_HOSTNAME, APP_NAME, LOG_KERN);

void print_packet(uint8_t pkt[SECPLUS2_CODE_LEN]) {
    RINFO("decoded packet: [%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X]",
            pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], pkt[8], pkt[9],
            pkt[10], pkt[11], pkt[12], pkt[13], pkt[14], pkt[15], pkt[16], pkt[17], pkt[18]);
}

#else // UNIT_TEST

void print_packet(uint8_t pkt[SECPLUS2_CODE_LEN]) {}

#endif // UNIT_TEST
