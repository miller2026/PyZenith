/**
 * @file ipc.c
 * @brief Implementation of robust atomic IPC routines.
 */

#include "ipc.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

void ipc_init_response(IpcResponse* resp) {
    if (!resp) return;
    memset(resp, 0, sizeof(IpcResponse));
}

void ipc_set_error(IpcResponse* resp, int code, const char* msg) {
    if (!resp) return;
    
    resp->status_code = code;
    resp->data_len = 0;
    
    if (msg) {
        strncpy(resp->payload, msg, PAYLOAD_CAP - 1);
        resp->payload[PAYLOAD_CAP - 1] = '\0';
        resp->data_len = (int32_t)strlen(resp->payload);
    }
}

void ipc_set_data(IpcResponse* resp, const char* data) {
    if (!resp) return;

    resp->status_code = 0;
    if (data) {
        strncpy(resp->payload, data, PAYLOAD_CAP - 1);
        resp->payload[PAYLOAD_CAP - 1] = '\0';
        resp->data_len = (int32_t)strlen(resp->payload);
    } else {
        resp->data_len = 0;
    }
}

int ipc_send_packet(int socket_fd, const IpcResponse* resp) {
    if (socket_fd < 0 || !resp) return -1;
    
    ssize_t sent;
    do {
        // SOCK_DGRAM guarantees atomic message boundaries.
        // We do NOT loop for partial writes unless interrupted by signal.
        sent = send(socket_fd, resp, sizeof(IpcResponse), 0);
    } while (sent < 0 && errno == EINTR);
    
    // Strict Protocol: Must send exactly packet size
    return (sent == sizeof(IpcResponse)) ? 0 : -1;
}

int ipc_receive_packet(int socket_fd, IpcResponse* resp) {
    if (socket_fd < 0 || !resp) return -1;

    ssize_t received;
    do {
        // SOCK_DGRAM: Recv waits for a full message.
        received = recv(socket_fd, resp, sizeof(IpcResponse), 0);
    } while (received < 0 && errno == EINTR);
    
    // Check for Protocol Violation or Truncation
    if (received != sizeof(IpcResponse)) {
        return -1; 
    }
    
    // Security: Defensive Null Termination
    // Even if sender was malicious, we clamp the length.
    if (resp->data_len < 0 || resp->data_len >= PAYLOAD_CAP) {
        resp->data_len = PAYLOAD_CAP - 1;
    }
    resp->payload[resp->data_len] = '\0';
    
    return 0;
}


