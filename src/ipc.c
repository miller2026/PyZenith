#define _GNU_SOURCE

#include "ipc.h"
#include "../sal/symbol_resolver.h" // Needed for logging
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

void ipc_set_str(ipc_packet_t* pkt, const char* str) {
    if (!pkt || !str) return;

    int n = snprintf((char*)pkt->data, IPC_PAYLOAD_CAP, "%s", str);
    
    // Handle truncation if string > 4KB
    if (n >= IPC_PAYLOAD_CAP) {
        pkt->head.len = IPC_PAYLOAD_CAP - 1;
        // Warn about truncation (requires access to logging system)
        if (sys && sys->log) {
            sys->log(5, "IPC", "Payload truncated: req=%d, cap=%d", n, IPC_PAYLOAD_CAP);
        }
    } else {
        pkt->head.len = n;
    }
}

int ipc_send(int fd, const ipc_packet_t* pkt) {
    if (!pkt) {
        errno = EINVAL;
        return -1;
    }

    ssize_t sent = write(fd, pkt, sizeof(ipc_packet_t));
    
    if (sent == sizeof(ipc_packet_t)) return 0;
    
    // Partial writes on atomic DGRAM sockets are errors
    if (sent >= 0) errno = EIO;
    return -1;
}

int ipc_recv(int fd, ipc_packet_t* pkt) {
    if (!pkt) {
        errno = EINVAL;
        return -1;
    }

    ssize_t r = read(fd, pkt, sizeof(ipc_packet_t));

    if (r == sizeof(ipc_packet_t)) {
        // Force null-termination for safety
        uint32_t len = pkt->head.len;
        uint32_t idx = (len < IPC_PAYLOAD_CAP) ? len : (IPC_PAYLOAD_CAP - 1);
        pkt->data[idx] = '\0';
        return 0;
    }

    if (r == 0) errno = 0; // EOF
    else if (r > 0) errno = EIO; // Partial read

    return -1;
}