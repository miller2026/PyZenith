/**
 * @file ipc.h
 * @brief Inter-Process Communication (IPC) Protocol Definitions.
 * * Defines the Fixed-Struct protocol used for communication between the Hub and Modules.
 * Enforces a strict 4096-byte packet size to align with kernel pages for atomic I/O.
 */

#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <assert.h>
#include <sys/types.h>

// ==============================================================================================
// CONFIGURATION
// ==============================================================================================

/**
 * @brief Packet size fixed to the standard Memory Page Size (4KB).
 * This ensures efficient kernel mapping and atomic DGRAM writes.
 */
#define IPC_PACKET_SIZE 4096

// ==============================================================================================
// MESSAGE TYPES
// ==============================================================================================

#define MSG_REQUEST     0x10 ///< Command from Hub to Module
#define MSG_RESPONSE    0x20 ///< Data response from Module to Hub
#define MSG_LOG_ENTRY   0x30 ///< Log message to be sent to the Network Module

// ==============================================================================================
// STRUCTURES
// ==============================================================================================

/**
 * @brief Standard IPC Header.
 * Contains metadata for routing, status reporting, and payload management.
 */
typedef struct {
    uint32_t type;       ///< Message Type (MSG_*)
    uint32_t request_id; ///< Correlation ID for request/response pairs
    int32_t  sender_id;  ///< ID of the sending module (or -1 for Hub)
    int32_t  status;     ///< Status Code: 0 = Success, Non-zero = Error
    uint32_t data_len;   ///< Length of valid data in the payload
} IpcHeader;

/**
 * @brief Maximum payload capacity.
 * Calculated dynamically to ensure the total struct size is exactly IPC_PACKET_SIZE.
 */
#define IPC_MAX_PAYLOAD (IPC_PACKET_SIZE - sizeof(IpcHeader))

/**
 * @brief The Unified IPC Packet.
 * Passed by value or pointer between Hub and Modules.
 */
typedef struct {
    IpcHeader header;
    uint8_t   data[IPC_MAX_PAYLOAD];
} IpcPacket;

// Compile-time assertion to guarantee struct alignment matches page size.
_Static_assert(sizeof(IpcPacket) == IPC_PACKET_SIZE, "IpcPacket size mismatch! Must be 4096 bytes.");

// ==============================================================================================
// API
// ==============================================================================================

/**
 * @brief Safely copies a string into the packet payload.
 * * Automatically handles truncation and updates the `data_len` header field.
 * * @param packet Pointer to the target packet.
 * @param str    Null-terminated string to copy.
 */
void ipc_set_payload(IpcPacket* packet, const char* str);

/**
 * @brief Sends a full packet to a file descriptor.
 * * This function works for both Blocking and Non-Blocking sockets.
 * * @param fd     Socket file descriptor.
 * @param packet Pointer to the packet to send.
 * @return 0 on success, -1 on failure (errno set).
 */
int ipc_send_packet(int fd, const IpcPacket* packet);

/**
 * @brief Receives a full packet with strict size validation.
 * * Enforces reading exactly IPC_PACKET_SIZE bytes. Handles null-termination safety.
 * * @param fd         Socket file descriptor.
 * @param out_packet Pointer to the destination packet struct.
 * @return 0 on success, -1 on failure or EOF.
 * If return is -1 and errno is 0, it indicates EOF (Connection Closed).
 */
int ipc_recv_packet(int fd, IpcPacket* out_packet);

#endif // IPC_H