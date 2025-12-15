/**
 * @file ipc_defs.h
 * @brief Project Hub - Inter-Process Communication Definitions
 * * Defines the wire protocol, message types, and shared constants.
 */

#ifndef IPC_DEFS_H
#define IPC_DEFS_H

#include <stdint.h>
#include <string.h>

// ==============================================================================================
// SECTION: Constraints & Sizes
// ==============================================================================================

#define MAX_PACKET_SIZE 4096
#define HEADER_SIZE     3
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - HEADER_SIZE)

// ==============================================================================================
// SECTION: Message Types
// ==============================================================================================

#define MSG_HEARTBEAT   0x01
#define MSG_REQUEST     0x10 // Standard Request (Manager -> Module)
#define MSG_RESPONSE    0x20 // Standard Response (Module -> Manager)
#define MSG_LOG_ENTRY   0x30 // Log Data (Manager -> Network Module)
#define MSG_ERROR       0xEE // Error Report (Module -> Manager)

// ==============================================================================================
// SECTION: Protocol Helpers
// ==============================================================================================

static inline void ipc_serialize_header(uint8_t* buffer, uint8_t type, uint16_t len) {
    buffer[0] = type;
    buffer[1] = (len >> 8) & 0xFF;
    buffer[2] = len & 0xFF;
}

static inline int ipc_parse_header(const uint8_t* buffer, uint8_t* out_type, uint16_t* out_len) {
    if (!buffer || !out_type || !out_len) return -1;
    *out_type = buffer[0];
    *out_len = (buffer[1] << 8) | buffer[2];
    return 0;
}

#endif // IPC_DEFS_H