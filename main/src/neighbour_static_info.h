#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "neighbour.h"
#include "wireless.h"

void neighbour_static_info_rx(const wireless_packet_t *packet, const neighbour_t *neigh);
void neighbour_static_info_update(void);
void neighbour_static_info_get_ap_ssid(const neighbour_t *neigh, char *buf, size_t len);
bool neighbour_static_info_get_ap_password(const neighbour_t *neigh, char *buf, size_t len);
