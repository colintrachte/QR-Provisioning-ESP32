// prov_ui.h
#pragma once
#include <Arduino.h>

void prov_ui_init(const char *ap_ssid, const char *ap_password);
void prov_ui_show_boot(void);
void prov_ui_on_state_change(int state, void *ctx);
void prov_ui_on_connected(const char *ssid, const char *ip, void *ctx);
void prov_ui_set_client_count(int count);
void prov_ui_tick(void);
