#include "lldp_cdp_parser.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

// LLDP TLV types
#define LLDP_TLV_END         0
#define LLDP_TLV_CHASSIS_ID  1
#define LLDP_TLV_PORT_ID     2
#define LLDP_TLV_TTL         3
#define LLDP_TLV_PORT_DESC   4
#define LLDP_TLV_SYS_NAME    5
#define LLDP_TLV_SYS_DESC    6
#define LLDP_TLV_ORG_SPECIFIC 127  // Org specific

// LLDP-MED TIA OUI and subtypes
#define LLDP_MED_OUI_0    0x00
#define LLDP_MED_OUI_1    0x12
#define LLDP_MED_OUI_2    0xBB
#define LLDP_MED_SUBTYPE_HW_REV  0x0A  // Hardware revision = model name

// CDP TLV types
#define CDP_TLV_DEVICE_ID    0x0001
#define CDP_TLV_PORT_ID      0x0003
#define CDP_TLV_VLAN         0x000A
#define CDP_TLV_PLATFORM     0x0006

// ─── EDP (Extreme Discovery Protocol) ───────────────────────────────────────
// LLC/SNAP: AA AA 03 | OUI: 00 E0 2B | PID: 00 BB
// EDP header: version(1) + reserved(1) + length(2) + checksum(2) + seqnum(2) + devtype(2)
// Then TLVs: type(2) + length(2) + data

#define EDP_TLV_NULL        0x0000
#define EDP_TLV_DISPLAY     0x0001  // hostname/display string
#define EDP_TLV_INFO        0x0002  // device info including platform
#define EDP_TLV_VLAN        0x0005  // VLAN info

bool parse_edp(const uint8_t *data, uint16_t len, neighbor_info_t *info) {
    const uint8_t *ptr = data + 14;  // skip ethernet header
    const uint8_t *end = data + len;

    // Verify LLC header: AA AA 03
    if (ptr + 8 > end) return false;
    if (ptr[0] != 0xAA || ptr[1] != 0xAA || ptr[2] != 0x03) return false;

    // Verify SNAP OUI: 00 E0 2B and PID: 00 BB
    if (ptr[3] != 0x00 || ptr[4] != 0xE0 || ptr[5] != 0x2B) return false;
    if (ptr[6] != 0x00 || ptr[7] != 0xBB) return false;
    ptr += 8;  // skip LLC + SNAP

    // Skip EDP header: version(1) + reserved(1) + length(2) + checksum(2) + seqnum(2) + devtype(2)
    if (ptr + 10 > end) return false;
    ptr += 10;

    memset(info, 0, sizeof(neighbor_info_t));
    bool found_something = false;

    while (ptr + 4 <= end) {
        uint16_t tlv_type = (ptr[0] << 8) | ptr[1];
        uint16_t tlv_len  = (ptr[2] << 8) | ptr[3];
        ptr += 4;

        if (tlv_type == EDP_TLV_NULL) break;
        if (ptr + tlv_len > end) break;

        switch (tlv_type) {
            case EDP_TLV_DISPLAY: {
                // Display string = hostname
                uint16_t copy_len = tlv_len < MAX_STRING_LEN - 1 ?
                                    tlv_len : MAX_STRING_LEN - 1;
                memcpy(info->hostname, ptr, copy_len);
                info->hostname[copy_len] = '\0';
                found_something = true;
                break;
            }
            case EDP_TLV_INFO: {
                // Bytes 0-1: slot, 2-3: port (1-based)
                if (tlv_len >= 4) {
                    snprintf(info->port, MAX_STRING_LEN, "%d/%d",
                             (ptr[0] << 8) | ptr[1],
                             (ptr[2] << 8) | ptr[3]);
                }
                // Bytes 4+ contain version string
                if (tlv_len > 4) {
                    uint16_t copy_len = (tlv_len - 4) < MAX_STRING_LEN - 1 ?
                                        (tlv_len - 4) : MAX_STRING_LEN - 1;
                    memcpy(info->platform, ptr + 4, copy_len);
                    info->platform[copy_len] = '\0';
                }
                found_something = true;
                break;
            }
            case EDP_TLV_VLAN: {
                // Bytes 0-1: VLAN ID
                if (tlv_len >= 2) {
                    uint16_t vlan_id = (ptr[0] << 8) | ptr[1];
                    // Only set if not already set (first VLAN wins)
                    if (info->vlan[0] == '\0') {
                        snprintf(info->vlan, MAX_STRING_LEN, "%d", vlan_id);
                    }
                }
                break;
            }
        }
        ptr += tlv_len;
    }

    info->valid = found_something;
    return found_something;
}

// ─── FDP (Foundry/Ruckus Discovery Protocol) ────────────────────────────────
// Very similar to CDP but dst MAC is 00:00:00:CC:CC:CC
// LLC/SNAP with Foundry OUI, PID 0x2000
// Header: version(1) + ttl(1) + checksum(2) then TLVs same as CDP format

#define FDP_TLV_DEVICE_ID   0x0001
#define FDP_TLV_PORT_ID     0x0003
#define FDP_TLV_PLATFORM    0x0006
#define FDP_TLV_VLAN        0x000A

bool parse_fdp(const uint8_t *data, uint16_t len, neighbor_info_t *info) {
    const uint8_t *ptr = data + 14;
    const uint8_t *end = data + len;

    // Verify LLC: AA AA 03
    if (ptr + 8 > end) return false;
    if (ptr[0] != 0xAA || ptr[1] != 0xAA || ptr[2] != 0x03) return false;

    // Foundry OUI: 00 00 0C (same as Cisco) but dst MAC is different
    // Some implementations use 00:00:00:CC:CC:CC dst with Foundry OUI
    ptr += 8;  // skip LLC + SNAP

    // Skip FDP header: version(1) + ttl(1) + checksum(2)
    if (ptr + 4 > end) return false;
    ptr += 4;

    memset(info, 0, sizeof(neighbor_info_t));
    bool found_something = false;

    while (ptr + 4 <= end) {
        uint16_t tlv_type = (ptr[0] << 8) | ptr[1];
        uint16_t tlv_len  = (ptr[2] << 8) | ptr[3];

        if (tlv_len < 4) break;
        ptr += 4;
        uint16_t data_len = tlv_len - 4;

        if (ptr + data_len > end) break;

        switch (tlv_type) {
            case FDP_TLV_DEVICE_ID: {
                uint16_t copy_len = data_len < MAX_STRING_LEN - 1 ?
                                    data_len : MAX_STRING_LEN - 1;
                memcpy(info->hostname, ptr, copy_len);
                info->hostname[copy_len] = '\0';
                found_something = true;
                break;
            }
            case FDP_TLV_PORT_ID: {
                uint16_t copy_len = data_len < MAX_STRING_LEN - 1 ?
                                    data_len : MAX_STRING_LEN - 1;
                memcpy(info->port, ptr, copy_len);
                info->port[copy_len] = '\0';
                found_something = true;
                break;
            }
            case FDP_TLV_PLATFORM: {
                uint16_t copy_len = data_len < MAX_STRING_LEN - 1 ?
                                    data_len : MAX_STRING_LEN - 1;
                memcpy(info->platform, ptr, copy_len);
                info->platform[copy_len] = '\0';
                break;
            }
            case FDP_TLV_VLAN: {
                if (data_len >= 2) {
                    uint16_t vlan_id = (ptr[0] << 8) | ptr[1];
                    snprintf(info->vlan, MAX_STRING_LEN, "%d", vlan_id);
                }
                break;
            }
        }
        ptr += data_len;
    }

    info->valid = found_something;
    return found_something;
}

// ─── NDP/SONMP (Nortel Discovery Protocol) ──────────────────────────────────
// Fixed format - no TLVs
// After ethernet header: segment(4) + chassis(4) + backplane(1) + port(1) + version(1)
// Hostname not carried in NDP - use source MAC as identifier

bool parse_ndp(const uint8_t *data, uint16_t len, neighbor_info_t *info) {
    const uint8_t *ptr = data + 14;  // skip ethernet header
    const uint8_t *end = data + len;

    // NDP minimum payload is 11 bytes
    if (ptr + 11 > end) return false;

    memset(info, 0, sizeof(neighbor_info_t));

    // Source MAC as hostname (NDP doesn't carry hostname)
    snprintf(info->hostname, MAX_STRING_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             data[6], data[7], data[8], data[9], data[10], data[11]);

    // Slot and port
    uint8_t slot = ptr[8];
    uint8_t port = ptr[9];
    snprintf(info->port, MAX_STRING_LEN, "%d/%d", slot, port);

    // NDP doesn't carry VLAN or platform in basic form
    snprintf(info->platform, MAX_STRING_LEN, "Nortel");

    info->valid = true;
    return true;
}

// ─── MNDP (MikroTik Neighbor Discovery Protocol) ────────────────────────────
// UDP broadcast to port 5678
// Ethernet(14) + IP(20) + UDP(8) + MNDP header(4) + TLVs
// MNDP header: type(1) + ttl(1) + seqnum(2)
// TLVs: type(2) + length(2) + data (all big-endian)

#define MNDP_TLV_MAC        0x0001  // MAC address
#define MNDP_TLV_IDENTITY   0x0005  // hostname/identity
#define MNDP_TLV_VERSION    0x0007  // RouterOS version
#define MNDP_TLV_PLATFORM   0x000C  // board/platform name was 0x0008 in older version
#define MNDP_TLV_INTERFACE  0x0010  // interface name was 0x000B in older version

bool parse_mndp(const uint8_t *data, uint16_t len, neighbor_info_t *info) {
    // Skip Ethernet(14) + IP(20) + UDP(8) = 42 bytes
    // But IP header can vary so check IHL
    if (len < 42) return false;

    const uint8_t *ip = data + 14;

    // Verify IPv4
    if ((ip[0] & 0xF0) != 0x40) return false;

    // Verify UDP protocol
    if (ip[9] != 0x11) return false;

    // Get IP header length
    uint8_t ihl = (ip[0] & 0x0F) * 4;

    const uint8_t *udp = ip + ihl;
    if (udp + 8 > data + len) return false;

    // Verify dst port 5678
    uint16_t dst_port = (udp[2] << 8) | udp[3];
    if (dst_port != 5678) return false;

    // Skip UDP header
    const uint8_t *mndp = udp + 8;
    const uint8_t *end  = data + len;

    // Skip MNDP header: type(1) + ttl(1) + seqnum(2)
    if (mndp + 4 > end) return false;
    mndp += 4;

    memset(info, 0, sizeof(neighbor_info_t));
    bool found_something = false;

    while (mndp + 4 <= end) {
        uint16_t tlv_type = (mndp[0] << 8) | mndp[1];
        uint16_t tlv_len  = (mndp[2] << 8) | mndp[3];
        mndp += 4;

        if (mndp + tlv_len > end) break;

        switch (tlv_type) {
            case MNDP_TLV_IDENTITY: {
                uint16_t copy_len = tlv_len < MAX_STRING_LEN - 1 ?
                                    tlv_len : MAX_STRING_LEN - 1;
                memcpy(info->hostname, mndp, copy_len);
                info->hostname[copy_len] = '\0';
                found_something = true;
                break;
            }
            case MNDP_TLV_INTERFACE: {
                uint16_t copy_len = tlv_len < MAX_STRING_LEN - 1 ?
                                    tlv_len : MAX_STRING_LEN - 1;
                memcpy(info->port, mndp, copy_len);
                info->port[copy_len] = '\0';
                found_something = true;
                break;
            }
            case MNDP_TLV_PLATFORM: {
                uint16_t copy_len = tlv_len < MAX_STRING_LEN - 1 ?
                                    tlv_len : MAX_STRING_LEN - 1;
                memcpy(info->platform, mndp, copy_len);
                info->platform[copy_len] = '\0';
                break;
            }
            case MNDP_TLV_VERSION: {
                // Use version as VLAN field since MNDP has no VLAN
                //uint16_t copy_len = tlv_len < MAX_STRING_LEN - 1 ?
                //                    tlv_len : MAX_STRING_LEN - 1;
                //memcpy(info->vlan, mndp, copy_len);
                //info->vlan[copy_len] = '\0';
                info->vlan[0] = '\0'; // Clear VLAN field since MNDP doesn't have VLAN, or could put version here if desired
                break;
            }
        }
        mndp += tlv_len;
    }

    info->valid = found_something;
    return found_something;
}

bool parse_lldp(const uint8_t *data, uint16_t len, neighbor_info_t *info) {
    // Skip ethernet header (14 bytes) + EtherType (already confirmed 0x88CC)
    // LLDP starts right after ethernet header
    const uint8_t *ptr = data + 14;
    const uint8_t *end = data + len;

    memset(info, 0, sizeof(neighbor_info_t));
    bool found_something = false;

    while (ptr + 2 <= end) {
        // TLV header: 7 bits type, 9 bits length
        uint16_t tlv_header = (ptr[0] << 8) | ptr[1];
        uint8_t  tlv_type   = (tlv_header >> 9) & 0x7F;
        uint16_t tlv_len    = tlv_header & 0x01FF;
        ptr += 2;

        if (ptr + tlv_len > end) break;
        if (tlv_type == LLDP_TLV_END) break;

        switch (tlv_type) {
            case LLDP_TLV_SYS_NAME: {
                uint16_t copy_len = tlv_len < MAX_STRING_LEN - 1 ? tlv_len : MAX_STRING_LEN - 1;
                memcpy(info->hostname, ptr, copy_len);
                info->hostname[copy_len] = '\0';
                found_something = true;
                break;
            }
            case LLDP_TLV_PORT_ID: {
//                ESP_LOGI("LLDP", "Port ID subtype: %d", ptr[0]);
                if (ptr[0] != 0x07) { // Only handle if subtype is not 7 (which is "interface name" and often empty or SNMP Interface ID which is not useful), otherwise try to use port description
                    uint16_t copy_len = (tlv_len - 1) < MAX_STRING_LEN - 1 ? (tlv_len - 1) : MAX_STRING_LEN - 1;
                    memcpy(info->port, ptr + 1, copy_len);
                    info->port[copy_len] = '\0';
                    found_something = true;
                } else {
                    // If it is subtype 7, we can try to use port description as fallback
                    info->port[0] = '\0'; // Clear port for now, will fill from port desc if needed
                }
                break;
            }
            case LLDP_TLV_PORT_DESC: {
                // Only use port desc if port ID was subtype 7
                if (info->port[0] == '\0') {
                    uint16_t copy_len = tlv_len < MAX_STRING_LEN - 1 ? tlv_len : MAX_STRING_LEN - 1;
                    memcpy(info->port, ptr, copy_len);
                    info->port[copy_len] = '\0';
                }
                break;
            }
            case LLDP_TLV_ORG_SPECIFIC: {
                if (tlv_len < 4) break;

                uint8_t oui0    = ptr[0];
                uint8_t oui1    = ptr[1];
                uint8_t oui2    = ptr[2];
                uint8_t subtype = ptr[3];

                // VLAN check - IEEE 802.1 OUI
                if (oui0 == 0x00 && oui1 == 0x80 && oui2 == 0xC2 && subtype == 0x01) {
                    if (tlv_len >= 6) {
                        uint16_t vlan_id = (ptr[4] << 8) | ptr[5];
                        snprintf(info->vlan, MAX_STRING_LEN, "%d", vlan_id);
                        found_something = true;
                        ESP_LOGI("LLDP", "VLAN ID: %d", vlan_id);
                    }
                }
                // LLDP-MED TIA-1057 hardware revision = model name
                else if (oui0 == LLDP_MED_OUI_0 && oui1 == LLDP_MED_OUI_1 && oui2 == LLDP_MED_OUI_2 && subtype == LLDP_MED_SUBTYPE_HW_REV) {
                    // Data starts at ptr[4], length is tlv_len - 4
                        uint16_t data_len = tlv_len - 4;
                        uint16_t copy_len = data_len < MAX_STRING_LEN - 1 ? data_len : MAX_STRING_LEN - 1;
                        memcpy(info->platform, ptr + 4, copy_len);
                        info->platform[copy_len] = '\0';
                        found_something = true;
                        ESP_LOGI("LLDP", "Platform/HW revision: %s", info->platform);
                }
                break;
            }
        }
        ptr += tlv_len;
    }

    info->valid = found_something;
    return found_something;
}

bool parse_cdp(const uint8_t *data, uint16_t len, neighbor_info_t *info) {
    // CDP frame structure:
    // Ethernet header (14) + LLC (3) + SNAP (5) + CDP header (4) + TLVs
    // LLC: AA AA 03
    // SNAP OUI: 00 00 0C, Protocol: 20 00
    const uint8_t *ptr = data + 14;  // skip ethernet header
    const uint8_t *end = data + len;

    // Verify LLC header
    if (ptr + 8 > end) return false;
    if (ptr[0] != 0xAA || ptr[1] != 0xAA || ptr[2] != 0x03) return false;

    // Verify SNAP OUI and protocol
    if (ptr[3] != 0x00 || ptr[4] != 0x00 || ptr[5] != 0x0C) return false;
    if (ptr[6] != 0x20 || ptr[7] != 0x00) return false;

    ptr += 8;  // skip LLC + SNAP

    // Skip CDP header: version(1) + TTL(1) + checksum(2)
    if (ptr + 4 > end) return false;
    ptr += 4;

    memset(info, 0, sizeof(neighbor_info_t));
    bool found_something = false;

    while (ptr + 4 <= end) {
        uint16_t tlv_type = (ptr[0] << 8) | ptr[1];
        uint16_t tlv_len  = (ptr[2] << 8) | ptr[3];  // includes 4-byte header

        if (tlv_len < 4) break;
        ptr += 4;
        uint16_t data_len = tlv_len - 4;

        if (ptr + data_len > end) break;

        switch (tlv_type) {
            case CDP_TLV_DEVICE_ID: {
                uint16_t copy_len = data_len < MAX_STRING_LEN - 1 ? data_len : MAX_STRING_LEN - 1;
                memcpy(info->hostname, ptr, copy_len);
                info->hostname[copy_len] = '\0';
                found_something = true;
                break;
            }
            case CDP_TLV_PORT_ID: {
                uint16_t copy_len = data_len < MAX_STRING_LEN - 1 ? data_len : MAX_STRING_LEN - 1;
                memcpy(info->port, ptr, copy_len);
                info->port[copy_len] = '\0';
                found_something = true;
                break;
            }
            case CDP_TLV_PLATFORM: {
                uint16_t copy_len = data_len < MAX_STRING_LEN - 1 ? data_len : MAX_STRING_LEN - 1;
                memcpy(info->platform, ptr, copy_len);
                info->platform[copy_len] = '\0';
                found_something = true;
                break;
            }
            case CDP_TLV_VLAN: {
                if (data_len >= 2) {
                    uint16_t vlan_id = (ptr[0] << 8) | ptr[1];
                    snprintf(info->vlan, MAX_STRING_LEN, "%d", vlan_id);
                    found_something = true;
                }
                break;
            }
        }
        ptr += data_len;
    }

    info->valid = found_something;
    return found_something;
}