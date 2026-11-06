// Minimal DHCP server for the Wi-Fi setup hotspot (v8): hands out 192.168.4.16-23 on the AP interface.
#pragma once
struct netif;
void dhcpd_start(struct netif* nif);
void dhcpd_stop(void);
