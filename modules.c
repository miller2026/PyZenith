/**
 * @file modules.c
 * @brief Implementation of Mock Modules and Tasks
 * * Contains the business logic for the child processes.
 * Includes helper functions for IPC within the child context.
 */

#include "modules.h"
#include "ipc_defs.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h> // Required for open()

// ==============================================================================================
// SECTION: Child Process IPC Helpers
// ==============================================================================================

/**
 * @brief Sends a TLV packet to the Hub.
 * Handles bounds checking and logs transmission errors.
 */
static void send_tlv(int fd, uint8_t type, const void* data, uint16_t len) {
    // 1. Bounds Check
    if (len + HEADER_SIZE > MAX_PACKET_SIZE) {
        LOG_ERROR("HubChild", "TX Error: Packet size (%d) exceeds limit (%d)", 
                  len + HEADER_SIZE, MAX_PACKET_SIZE);
        return; 
    }

    uint8_t buffer[MAX_PACKET_SIZE];
    buffer[0] = type;
    buffer[1] = (len >> 8) & 0xFF; // Big Endian Length
    buffer[2] = len & 0xFF;

    if (len > 0 && data) {
        memcpy(buffer + HEADER_SIZE, data, len);
    }

    // 2. Write and Verify
    ssize_t sent = write(fd, buffer, HEADER_SIZE + len);
    
    if (sent < 0) {
        LOG_ERROR("HubChild", "TX Failed: %s", strerror(errno));
    } else if (sent != HEADER_SIZE + len) {
        LOG_ERROR("HubChild", "TX Partial Write: Sent %zd of %d bytes", sent, HEADER_SIZE + len);
    }
}

/**
 * @brief Reads a full TLV packet from the Hub.
 * Performs validation on header structure and payload size.
 * @return Payload length on success, -1 on failure.
 */
static int read_tlv(int fd, uint8_t* out_type, uint8_t* buffer, int max_len) {
    uint8_t raw_buf[MAX_PACKET_SIZE];
    
    // 1. Read from Socket
    ssize_t r = read(fd, raw_buf, sizeof(raw_buf));
    
    if (r < 0) {
        LOG_ERROR("HubChild", "RX Failed: %s", strerror(errno));
        return -1;
    }
    if (r == 0) {
        // EOF / Hub Closed Connection
        return -1; 
    }

    // 2. Validate Header Size
    if (r < HEADER_SIZE) {
        LOG_ERROR("HubChild", "RX Error: Packet too short (%zd bytes) to contain header", r);
        return -1; 
    }

    *out_type = raw_buf[0];
    uint16_t len = (raw_buf[1] << 8) | raw_buf[2];

    // 3. Validate Consistency
    if (r != HEADER_SIZE + len) {
        LOG_ERROR("HubChild", "RX Error: Size Mismatch. Header claims %d bytes, read %zd bytes", len, r - HEADER_SIZE);
        return -1; 
    }

    // 4. Validate Buffer Capacity
    if (len > max_len) {
        LOG_ERROR("HubChild", "RX Error: Buffer overflow. Payload %d > Capacity %d", len, max_len);
        return -1; 
    }
    
    if (len > 0) {
        memcpy(buffer, raw_buf + HEADER_SIZE, len);
    }
    return len;
}

// ==============================================================================================
// SECTION: Module 1 - Echo Service
// ==============================================================================================

void mod_echo_entry(int socket_fd) {
    LOG_INFO("HubMod_Echo", "Module started. Listening for echo requests...");
    
    uint8_t type;
    uint8_t buf[MAX_PAYLOAD_SIZE];

    while (1) {
        int len = read_tlv(socket_fd, &type, buf, sizeof(buf));
        if (len < 0) {
            LOG_INFO("HubMod_Echo", "Connection closed or error. Exiting.");
            break; 
        }

        // Simulate processing time
        usleep(10000);
        
        // Echo back
        send_tlv(socket_fd, MSG_RESPONSE, buf, len);
    }
}

// ==============================================================================================
// SECTION: Module 2 - Long Task (One-Shot)
// ==============================================================================================

void mod_long_task_entry(int socket_fd) {
    LOG_INFO("HubMod_LongTask", "Module started.");

    uint8_t type;
    uint8_t buf[MAX_PAYLOAD_SIZE];

    int len = read_tlv(socket_fd, &type, buf, sizeof(buf));
    if (len < 0) {
        LOG_ERROR("HubMod_LongTask", "Failed to receive start command.");
        return;
    }

    LOG_INFO("HubMod_LongTask", "Starting 5s simulation...");
    struct timespec req = {5, 0};
    nanosleep(&req, NULL);

    const char* msg = "Task Complete";
    send_tlv(socket_fd, MSG_RESPONSE, msg, strlen(msg));
    
    LOG_INFO("HubMod_LongTask", "Task finished. Exiting.");
}

// ==============================================================================================
// SECTION: Module 3 - Crasher
// ==============================================================================================

void mod_crasher_entry(int socket_fd) {
    (void)socket_fd; 
    LOG_INFO("HubMod_Crasher", "Module started. Preparing to crash...");

    usleep(100000); 
    
    LOG_INFO("HubMod_Crasher", "Goodbye cruel world!");
    volatile int* p = 0;
    *p = 0;
}

// ==============================================================================================
// SECTION: Module 4 - Phone File Reader
// ==============================================================================================

void mod_phone_reader_entry(int socket_fd) {
    const char* TAG = "HubMod_Phone";
    const char* file_path = "/data/data/com.android/phone/files/test.xml";
    LOG_INFO(TAG, "Module started.");

    uint8_t type;
    uint8_t buf[MAX_PAYLOAD_SIZE];
    int fd = -1;
    char file_buf[2048];
    const char* err_msg = NULL;

    // Wait for start command
    int len = read_tlv(socket_fd, &type, buf, sizeof(buf));
    if (len < 0) return;

    // 1. Open the file
    LOG_INFO(TAG, "Attempting to open: %s", file_path);
    fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR(TAG, "Failed to open file: %s (errno=%d)", strerror(errno), errno);
        err_msg = "Error: Could not open file";
        goto cleanup;
    }

    // 2. Read content
    ssize_t bytes_read = read(fd, file_buf, sizeof(file_buf) - 1);
    if (bytes_read < 0) {
        LOG_ERROR(TAG, "Failed to read file: %s", strerror(errno));
        err_msg = "Error: Could not read file";
        goto cleanup;
    }
    
    file_buf[bytes_read] = '\0'; // Null terminate for string operations
    LOG_INFO(TAG, "Read %zd bytes. Parsing...", bytes_read);

    // 3. Extract Value
    const char* key_pattern = "\"my_key_123\"";
    char* key_pos = strstr(file_buf, key_pattern);

    if (!key_pos) {
        LOG_ERROR(TAG, "Key '%s' not found in XML", key_pattern);
        err_msg = "Error: Key not found";
        goto cleanup;
    }

    // Find the closing '>'
    char* val_start = strchr(key_pos, '>');
    if (!val_start) {
        LOG_ERROR(TAG, "Malformed XML: No closing '>' after key");
        err_msg = "Error: Malformed XML (No closing >)";
        goto cleanup;
    }
    val_start++; // Move past '>'

    // Find the opening '<'
    char* val_end = strchr(val_start, '<');
    if (!val_end) {
        LOG_ERROR(TAG, "Malformed XML: No closing '<' after value");
        err_msg = "Error: Malformed XML (No closing <)";
        goto cleanup;
    }

    // 4. Send Response
    size_t val_len = val_end - val_start;
    
    // Cap length at buffer size
    if (val_len > sizeof(buf)) {
        LOG_ERROR(TAG, "Value too large (%zu bytes), truncating", val_len);
        val_len = sizeof(buf);
    }

    LOG_INFO(TAG, "Value extracted successfully. Sending response.");
    send_tlv(socket_fd, MSG_RESPONSE, val_start, val_len);

cleanup:
    // Resource cleanup
    if (fd >= 0) {
        close(fd);
    }
    
    // Error reporting
    if (err_msg != NULL) {
        send_tlv(socket_fd, MSG_ERROR, err_msg, strlen(err_msg));
    }
}

// ==============================================================================================
// SECTION: Module 5 - System Property Reader
// ==============================================================================================

/**
 * @brief Property Reader Module
 * Fetches the value of 'a.b.cdde' using the SAL (libc).
 */
void mod_prop_reader_entry(int socket_fd) {
    const char* TAG = "HubMod_Prop";
    const char* PROP_NAME = "a.b.cdde";
    LOG_INFO(TAG, "Module started.");

    uint8_t type;
    uint8_t buf[MAX_PAYLOAD_SIZE];

    // Wait for start command
    int len = read_tlv(socket_fd, &type, buf, sizeof(buf));
    if (len < 0) return;

    LOG_INFO(TAG, "Fetching system property: %s", PROP_NAME);

    // Bionic system properties are typically capped at 92 bytes.
    // We use a small buffer.
    char prop_val[128] = {0};
    
    if (g_api->sys_prop_get) {
        g_api->sys_prop_get(PROP_NAME, prop_val);
    } else {
        LOG_ERROR(TAG, "SAL system_property_get symbol not resolved!");
        const char* err = "SAL Error";
        send_tlv(socket_fd, MSG_ERROR, err, strlen(err));
        return;
    }

    // If property is unset, value is empty string.
    LOG_INFO(TAG, "Property value: '%s'", prop_val);
    
    send_tlv(socket_fd, MSG_RESPONSE, prop_val, strlen(prop_val));
}

// ==============================================================================================
// SECTION: Registry Definition
// ==============================================================================================

const ModuleDef MODULE_REGISTRY[MODULE_COUNT] = {
    [MOD_ECHO] = {
        .id = MOD_ECHO, 
        .name = "mod_echo", 
        .type = MODULE_TYPE_SERVICE,
        .target_uid = 2000, .target_gid = 2000, 
        .target_selinux_context = "u:r:shell:s0", 
        .entrypoint = mod_echo_entry
    },
    [MOD_LONG_TASK] = {
        .id = MOD_LONG_TASK, 
        .name = "mod_long_task", 
        .type = MODULE_TYPE_ONESHOT,
        .target_uid = 2000, .target_gid = 2000, 
        .target_selinux_context = "u:r:shell:s0",
        .entrypoint = mod_long_task_entry
    },
    [MOD_CRASHER] = {
        .id = MOD_CRASHER, 
        .name = "mod_crasher", 
        .type = MODULE_TYPE_ONESHOT,
        .target_uid = 2000, .target_gid = 2000, 
        .target_selinux_context = "u:r:shell:s0",
        .entrypoint = mod_crasher_entry
    },
    [MOD_PHONE_READER] = {
        .id = MOD_PHONE_READER,
        .name = "mod_phone_reader",
        .type = MODULE_TYPE_ONESHOT,
        .target_uid = 1000, .target_gid = 1000, 
        .target_selinux_context = "u:r:platform_app:s0", 
        .entrypoint = mod_phone_reader_entry
    },
    [MOD_PROP_READER] = {
        .id = MOD_PROP_READER,
        .name = "mod_prop_reader",
        .type = MODULE_TYPE_ONESHOT,
        // Using Shell UID/Context for general property reading.
        // Reading specific props might require specific domains.
        .target_uid = 2000, .target_gid = 2000, 
        .target_selinux_context = "u:r:shell:s0",
        .entrypoint = mod_prop_reader_entry
    }
};
