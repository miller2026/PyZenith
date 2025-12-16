/**
 * @file ipc.h
 * @brief Inter-Process Communication Protocol Definitions.
 *
 * Defines the fixed-size structure used for communication between
 * the unprivileged modules and the orchestration daemon via Unix Domain Sockets.
 */

#ifndef PROJECT_HUB_IPC_H
#define PROJECT_HUB_IPC_H

#include <stdint.h>
#include <stddef.h>

/** * @brief Packet size strictly aligned to 4KB Page size.
 * Ensures atomic reads/writes on many kernel configurations.
 */
#define IPC_PACKET_SIZE 4096

/** * @brief Calculate payload capacity.
 * Total Size - Status (4B) - Length (4B)
 */
#define PAYLOAD_CAP     (IPC_PACKET_SIZE - sizeof(int32_t) - sizeof(int32_t))

/**
 * @brief The IPC Packet Structure.
 * Sent over SOCK_DGRAM. Must be 4096 bytes exactly.
 */
typedef struct __attribute__((aligned(4096))) {
    int32_t status_code;          /**< 0 = Success, Non-Zero = Error Code */
    int32_t data_len;             /**< Length of valid bytes in payload */
    char    payload[PAYLOAD_CAP]; /**< Data buffer (Null-terminated) */
} IpcResponse;

// --- API ---

/**
 * @brief Initialize a response structure (zero-fill).
 * @param resp Pointer to the response structure.
 */
void ipc_init_response(IpcResponse* resp);

/**
 * @brief Populate response with error details.
 * @param resp Pointer to the response structure.
 * @param code Error code.
 * @param msg Error description string.
 */
void ipc_set_error(IpcResponse* resp, int code, const char* msg);

/**
 * @brief Populate response with success data.
 * @param resp Pointer to the response structure.
 * @param data The string data to send.
 */
void ipc_set_data(IpcResponse* resp, const char* data);

/**
 * @brief Send a packet over a socket (Atomic DGRAM).
 * @param socket_fd The file descriptor to write to.
 * @param resp The populated response structure.
 * @return 0 on success, -1 on failure.
 */
int ipc_send_packet(int socket_fd, const IpcResponse* resp);

/**
 * @brief Receive a packet from a socket (Atomic DGRAM).
 * @param socket_fd The file descriptor to read from.
 * @param resp The structure to populate.
 * @return 0 on success, -1 on failure/protocol violation.
 */
int ipc_receive_packet(int socket_fd, IpcResponse* resp);

#endif // PROJECT_HUB_IPC_H


