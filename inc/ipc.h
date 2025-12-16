#ifndef HUB_IPC_H
#define HUB_IPC_H

#include <stdint.h>
#include <assert.h>

#define IPC_PAGE_SIZE 4096

typedef enum {
    MSG_REQ  = 0x10,
    MSG_RESP = 0x20,
    MSG_LOG  = 0x30,
} msg_type_t;

typedef struct {
    uint32_t type;      // msg_type_t
    uint32_t req_id;    // Correlation ID
    int32_t  sender;    // Module ID
    int32_t  status;    // 0 = OK, <0 = Err
    uint32_t len;       // Payload length
} ipc_header_t;

#define IPC_PAYLOAD_CAP (IPC_PAGE_SIZE - sizeof(ipc_header_t))

typedef struct {
    ipc_header_t head;
    uint8_t      data[IPC_PAYLOAD_CAP];
} ipc_packet_t;

_Static_assert(sizeof(ipc_packet_t) == IPC_PAGE_SIZE, "Packet must align to 4KB page");

/* API */
void ipc_set_str(ipc_packet_t* pkt, const char* str);
int  ipc_send(int fd, const ipc_packet_t* pkt);
int  ipc_recv(int fd, ipc_packet_t* pkt);

#endif