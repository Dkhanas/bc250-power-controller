// Catch-all DNS server for the setup hotspot (v8): every name resolves to the Pico (192.168.4.1), so phones
// show the setup page by themselves ("captive portal") and any address typed in the browser opens it.
#pragma once
struct netif;
void dnsd_start(struct netif* nif);
void dnsd_stop(void);
