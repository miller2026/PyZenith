/**
 * @file ipc_defs.h
 * @brief Project Hub - Inter-Process Communication Definitions
 * * Defines the wire protocol, message types, and shared constants used 
 * for communication between the Main Daemon (Hub) and Child Modules.
 * * Protocol: Type-Length-Value (TLV)
 * Byte Order: Big Endian (Network Byte Order)
 */

#ifndef IPC_DEFS_H
#define IPC_DEFS_H

#include <stdint.h>

// ==============================================================================================
// SECTION: Constraints & Sizes
// ==============================================================================================

/**
 * @brief Maximum total size of a single packet (Header + Payload).
 * Strictly enforced to prevent buffer overflows.
 */
#define MAX_PACKET_SIZE 4096

/**
 * @brief Size of the TLV Header.
 * Format: [TYPE: 1 Byte] [LENGTH: 2 Bytes]
 */
#define HEADER_SIZE     3

/**
 * @brief Maximum allowed size for the payload.
 */
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - HEADER_SIZE)

// ==============================================================================================
// SECTION: Message Types
// ==============================================================================================

/** @brief Keep-alive signal (Optional) */
#define MSG_HEARTBEAT   0x01

/** @brief Command sent from Hub to Module */
#define MSG_REQUEST     0x10

/** @brief Data sent from Module to Hub */
#define MSG_RESPONSE    0x20

/** @brief Error report from Module to Hub */
#define MSG_ERROR       0xEE

// ==============================================================================================
// SECTION: Data Structures
// ==============================================================================================

/**
 * @brief Helper struct for interpreting raw packet buffers.
 * @note This struct is for convenience in memory operations. 
 * Wire format handling must strictly account for Endianness.
 */
typedef struct {
    uint8_t type;       ///< Message Type ID
    uint16_t length;    ///< Payload Length (Host Byte Order)
    uint8_t* value;     ///< Pointer to Payload data
} PacketView;

// ==============================================================================================
// SECTION: Macros
// ==============================================================================================

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#endif // IPC_DEFS_H
