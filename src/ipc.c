/**
 * @file ipc.c
 * @brief IPC Protocol Implementation.
 * * Implements the unified I/O logic for the fixed-struct protocol.
 * Ensures strict boundary checks and memory safety.
 */

#define _GNU_SOURCE

#include "ipc.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

// ==============================================================================================
// HELPERS
// ==============================================================================================

void ipc_set_payload(IpcPacket* packet, const char* str) {
    if (!packet || !str) return;
    
    // snprintf protects against buffer overflows.
    // It returns the length that WOULD have been written.
    int written = snprintf((char*)packet->data, IPC_MAX_PAYLOAD, "%s", str);
    
    if (written >= IPC_MAX_PAYLOAD) {
        // Truncated: buffer full, last byte is null terminator.
        packet->header.data_len = IPC_MAX_PAYLOAD - 1; 
    } else {
        packet->header.data_len = written;
    }
}

// ==============================================================================================
// I/O OPERATIONS
// ==============================================================================================

int ipc_send_packet(int fd, const IpcPacket* packet) {
    if (!packet) {
        errno = EINVAL;
        return -1;
    }

    // Atomic write of the full page.
    ssize_t sent = write(fd, packet, sizeof(IpcPacket));
    
    if (sent == sizeof(IpcPacket)) {
        return 0;
    }

    // If we are here, either an error occurred (sent < 0) or a partial write occurred.
    // Partial writes on DGRAM sockets are protocol violations.
    if (sent >= 0) {
        errno = EIO; 
    }
    
    return -1;
}

int ipc_recv_packet(int fd, IpcPacket* out_packet) {
    if (!out_packet) {
        errno = EINVAL;
        return -1;
    }

    ssize_t r = read(fd, out_packet, sizeof(IpcPacket));

    if (r == sizeof(IpcPacket)) {
        // SAFETY: Force Null-Termination immediately upon receipt.
        // This protects against logic errors in the receiver even if the sender is malicious.
        if (out_packet->header.data_len < IPC_MAX_PAYLOAD) {
            out_packet->data[out_packet->header.data_len] = '\0';
        } else {
            out_packet->data[IPC_MAX_PAYLOAD - 1] = '\0';
        }
        return 0;
    }

    if (r == 0) {
        errno = 0; // EOF indication
        return -1;
    }

    if (r < 0) {
        // errno is already set by read()
        return -1;
    }

    // Partial read (r > 0 but != size)
    errno = EIO;
    return -1;
}