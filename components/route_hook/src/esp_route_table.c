/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_route_table.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#define MAX_RIO_ROUTE 20

#define TAG "ROUTE_HOOK"

static esp_route_entry_t s_route_entries[MAX_RIO_ROUTE];

static esp_route_entry_t *find_route_entry(const esp_route_entry_t *route_entry)
{
    for (size_t i = 0; i < LWIP_ARRAYSIZE(s_route_entries); i++) {
        if (s_route_entries[i].netif == NULL) {
            break;
        }
        if (s_route_entries[i].netif == route_entry->netif &&
            s_route_entries[i].prefix_length == route_entry->prefix_length &&
            memcmp(s_route_entries[i].gateway.addr, route_entry->gateway.addr, sizeof(route_entry->gateway.addr)) ==
                0 &&
            memcmp(s_route_entries[i].prefix.addr, route_entry->prefix.addr, route_entry->prefix_length / 8) == 0) {
            return &s_route_entries[i];
        }
    }
    return NULL;
}

static esp_route_entry_t *find_empty_route_entry(void)
{
    for (size_t i = 0; i < LWIP_ARRAYSIZE(s_route_entries); i++) {
        if (s_route_entries[i].netif == NULL) {
            return &s_route_entries[i];
        }
    }
    return NULL;
}

static void route_timeout_handler(void *arg)
{
    esp_route_entry_t *route = (esp_route_entry_t *)arg;

    esp_route_table_remove_route_entry(route);
}

esp_route_entry_t *esp_route_table_add_route_entry(const esp_route_entry_t *route_entry)
{
    if (route_entry == NULL) {
        return NULL;
    }

    esp_route_entry_t *entry = find_route_entry(route_entry);

    if (entry == NULL) {
        entry = find_empty_route_entry();
        if (entry == NULL) {
            return NULL;
        }
        entry->netif = route_entry->netif;
        entry->gateway = route_entry->gateway;
        ip6_addr_assign_zone(&entry->gateway, IP6_UNICAST, entry->netif);
        entry->prefix = route_entry->prefix;
        entry->prefix_length = route_entry->prefix_length;
    } else {
        sys_untimeout(route_timeout_handler, entry);
    }
    entry->preference = route_entry->preference;
    entry->lifetime_seconds = route_entry->lifetime_seconds;
    if (entry->lifetime_seconds != UINT32_MAX) {
        sys_timeout(entry->lifetime_seconds * 1000, route_timeout_handler, entry);
    }
    return entry;
}

esp_err_t esp_route_table_remove_route_entry(esp_route_entry_t *route_entry)
{
    if (route_entry < &s_route_entries[0] || route_entry > &s_route_entries[LWIP_ARRAYSIZE(s_route_entries)]) {
        return ESP_ERR_INVALID_ARG;
    }
    route_entry->netif = NULL;
    for (esp_route_entry_t *moved = route_entry; moved < &s_route_entries[LWIP_ARRAYSIZE(s_route_entries) - 1];
         moved++) {
        *moved = *(moved + 1);
        if (moved->netif == NULL) {
            break;
        }
    }
    return ESP_OK;
}

static inline bool is_better_route(const esp_route_entry_t *lhs, const esp_route_entry_t *rhs)
{
    if (rhs == NULL) {
        return true;
    }
    if (lhs == NULL) {
        return false;
    }
    return (lhs->prefix_length > rhs->prefix_length) ||
        (lhs->prefix_length == rhs->prefix_length && lhs->preference > rhs->preference);
}

static inline bool route_match(const esp_route_entry_t *route, const ip6_addr_t *dest)
{
    return memcmp(dest, route->prefix.addr, route->prefix_length / 8) == 0;
}

struct netif *lwip_hook_ip6_route(const ip6_addr_t *src, const ip6_addr_t *dest)
{
    esp_route_entry_t *route = NULL;

    for (size_t i = 0; i < LWIP_ARRAYSIZE(s_route_entries); i++) {
        if (s_route_entries[i].netif == NULL) {
            break;
        }
        if (route_match(&s_route_entries[i], dest) && is_better_route(&s_route_entries[i], route)) {
            route = &s_route_entries[i];
        }
    }

    if (route) {
        return route->netif;
    } else {
        return NULL;
    }
}

const ip6_addr_t *lwip_hook_nd6_get_gw(struct netif *netif, const ip6_addr_t *dest)
{
    esp_route_entry_t *route = NULL;

    for (size_t i = 0; i < LWIP_ARRAYSIZE(s_route_entries); i++) {
        if (s_route_entries[i].netif == NULL) {
            break;
        }
        if (s_route_entries[i].netif == netif && route_match(&s_route_entries[i], dest) &&
            is_better_route(&s_route_entries[i], route)) {
            route = &s_route_entries[i];
        }
    }

    if (route) {
        return &route->gateway;
    } else {
        return NULL;
    }
}
