#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_STRING_LEN 32

typedef struct {
    char hostname[MAX_STRING_LEN];
    char port[MAX_STRING_LEN];
    char vlan[MAX_STRING_LEN];
    char platform[MAX_STRING_LEN];
    bool valid;
} neighbor_info_t;

typedef enum {
    PROTO_LLDP,
    PROTO_CDP,
    PROTO_EDP,
    PROTO_FDP,
    PROTO_NDP,
    PROTO_MNDP
} proto_type_t;

// Protocol name helper
static inline const char *proto_name(proto_type_t proto) {
    switch (proto) {
        case PROTO_LLDP: return "LLDP";
        case PROTO_CDP:  return "CDP";
        case PROTO_EDP:  return "EDP";
        case PROTO_FDP:  return "FDP";
        case PROTO_NDP:  return "NDP";
        case PROTO_MNDP: return "MNDP";
        default:         return "???";
    }
}

// Parse EDP packet, returns true if successful
bool parse_edp(const uint8_t *data, uint16_t len, neighbor_info_t *info);
// Parse FDP packet, returns true if successful
bool parse_fdp(const uint8_t *data, uint16_t len, neighbor_info_t *info);
// Parse NDP packet, returns true if successful
bool parse_ndp(const uint8_t *data, uint16_t len, neighbor_info_t *info);
// Parse MNDP packet, returns true if successful
bool parse_mndp(const uint8_t *data, uint16_t len, neighbor_info_t *info);
// Parse LLDP packet, returns true if successful
bool parse_lldp(const uint8_t *data, uint16_t len, neighbor_info_t *info);
// Parse CDP packet, returns true if successful
bool parse_cdp(const uint8_t *data, uint16_t len, neighbor_info_t *info);