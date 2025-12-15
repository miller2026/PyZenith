/**
 * @file modules.c
 * @brief Functional Module Implementations.
 * * Contains the business logic for the IMEI, Phone, and Network modules.
 * All I/O here is BLOCKING, as these run in their own isolated processes.
 */

#define _GNU_SOURCE

#include "modules.h"
#include "ipc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h> 

// ==============================================================================================
// HELPERS: NETWORK SIMULATION
// ==============================================================================================

static void send_log_to_server(const char* log) {
    // In a real implementation, this would open a TCP socket to a remote server.
    LOG_INFO("NetLib", "[Network TX] Uploading log: %s", log);
    usleep(50000); // Simulate network latency
}

static void request_data_from_server(const char* req, char* out_buf, size_t max_len) {
    LOG_INFO("NetLib", "[Network TX] Requesting data: %s", req);
    usleep(100000); // Simulate network latency
    snprintf(out_buf, max_len, "ServerResponse: Data for '%s' [ID: %d]", req, rand() % 9999);
}

// ==============================================================================================
// HELPERS: XML PARSER (Zero-Dependency)
// ==============================================================================================

static const char* xml_find_next_tag(const char* cursor) { 
    return strchr(cursor, '<'); 
}

static int xml_get_attribute(const char* tag_content, const char* attr_name, char* out_val, size_t max_len) {
    char search[64]; 
    snprintf(search, sizeof(search), "%s=", attr_name);
    
    const char* p = strstr(tag_content, search);
    if (!p) return -1;
    
    p += strlen(search);
    char quote = *p; 
    if (quote != '"' && quote != '\'') return -1;
    
    p++; 
    size_t i = 0; 
    while (*p && *p != quote && i < max_len - 1) { 
        out_val[i++] = *p++; 
    }
    out_val[i] = '\0'; 
    return 0;
}

static int xml_extract_value(const char* xml, const char* target_name, char* out_buf, size_t max_len) {
    const char* cursor = xml;
    while ((cursor = xml_find_next_tag(cursor)) != NULL) {
        // Skip comments or processing instructions
        if (cursor[1] == '/' || cursor[1] == '?' || cursor[1] == '!') { 
            cursor++; continue; 
        }
        
        const char* tag_end = strchr(cursor, '>'); 
        if (!tag_end) break; 
        
        size_t tag_len = tag_end - cursor; 
        char tag_content[512]; 
        if (tag_len >= sizeof(tag_content)) tag_len = sizeof(tag_content) - 1;
        
        memcpy(tag_content, cursor, tag_len); 
        tag_content[tag_len] = '\0';
        
        char name_val[128];
        // Look for name="target_name"
        if (xml_get_attribute(tag_content, "name", name_val, sizeof(name_val)) == 0) {
            if (strcmp(name_val, target_name) == 0) {
                // Strategy 1: Look for value="..."
                if (xml_get_attribute(tag_content, "value", out_buf, max_len) == 0) return 0; 
                
                // Strategy 2: Look for >InnerText<
                const char* content_start = tag_end + 1;
                const char* content_end = strchr(content_start, '<');
                if (content_end) {
                    size_t len = content_end - content_start;
                    if (len >= max_len) len = max_len - 1;
                    memcpy(out_buf, content_start, len); 
                    out_buf[len] = '\0'; 
                    return 0; 
                }
            }
        }
        cursor = tag_end + 1;
    }
    return -1; 
}

static int xml_get_string(const char* xml, const char* key, char* out_buf, size_t max_len) { 
    return xml_extract_value(xml, key, out_buf, max_len); 
}

static char* read_file_fully(const char* path, size_t* out_size) {
    int fd = open(path, O_RDONLY); 
    if (fd < 0) return NULL;
    
    size_t capacity = 4096; 
    size_t size = 0; 
    char* buf = malloc(capacity);
    
    if (!buf) { close(fd); return NULL; }

    while (1) {
        if (size + 1024 > capacity) {
            capacity *= 2; 
            char* new_buf = realloc(buf, capacity);
            if (!new_buf) { free(buf); close(fd); return NULL; }
            buf = new_buf;
        }
        
        ssize_t r = read(fd, buf + size, capacity - size - 1);
        if (r < 0) { 
            if (errno == EINTR) continue; 
            free(buf); close(fd); return NULL; 
        }
        if (r == 0) break; // EOF
        size += r;
    }
    
    buf[size] = '\0'; 
    if (out_size) *out_size = size; 
    close(fd); 
    return buf;
}

// ==============================================================================================
// MODULE ENTRY POINTS
// ==============================================================================================

void mod_imei_entry(int socket_fd) {
    const char* TAG = "ModIMEI";
    const char* PROP_KEY = "ro.id.imei";
    
    LOG_INFO(TAG, "Starting IMEI extraction...");
    
    IpcPacket req;
    // Blocking read: Wait for Hub to ask for data
    if (ipc_recv_packet(socket_fd, &req) < 0) return;

    IpcPacket resp = {0};
    resp.header.type = MSG_RESPONSE;
    resp.header.sender_id = MOD_IMEI;
    resp.header.request_id = req.header.request_id; // Echo ID

    char prop_val[128] = {0};
    
    // Check if System API loaded correctly
    if (!g_api->sys_prop_get) {
        resp.header.status = -1;
        ipc_set_payload(&resp, "SYMBOL_RESOLVER Error: sys_prop_get missing");
    } else {
        int len = g_api->sys_prop_get(PROP_KEY, prop_val);
        if (len <= 0 || strlen(prop_val) == 0) {
            resp.header.status = -1;
            ipc_set_payload(&resp, "Error: IMEI not found");
        } else {
            resp.header.status = 0;
            ipc_set_payload(&resp, prop_val);
            LOG_INFO(TAG, "IMEI Extracted: %s", prop_val);
        }
    }

    ipc_send_packet(socket_fd, &resp);
}

void mod_phone_entry(int socket_fd) {
    const char* TAG = "ModPhone";
    const char* FILE_PATH = "/data/data/com.android.phone/shared_prefs/com.android.phone_preferences.xml";
    const char* TARGET_KEY = "phone_number_value";
    
    LOG_INFO(TAG, "Starting Phone Number extraction...");

    IpcPacket req;
    if (ipc_recv_packet(socket_fd, &req) < 0) return;

    IpcPacket resp = {0};
    resp.header.type = MSG_RESPONSE;
    resp.header.sender_id = MOD_PHONE;
    resp.header.request_id = req.header.request_id;

    char* file_content = read_file_fully(FILE_PATH, NULL);
    if (!file_content) {
        resp.header.status = -1;
        ipc_set_payload(&resp, "Error: Read failed");
    } else {
        char value[256];
        if (xml_get_string(file_content, TARGET_KEY, value, sizeof(value)) != 0) {
            resp.header.status = -1;
            ipc_set_payload(&resp, "Error: Key not found");
        } else {
            resp.header.status = 0;
            ipc_set_payload(&resp, value);
        }
    }

    if (file_content) free(file_content);
    ipc_send_packet(socket_fd, &resp);
}

void mod_network_entry(int socket_fd) {
    const char* TAG = "ModNet";
    LOG_INFO(TAG, "Network Service Started. Waiting for commands...");
    srand(time(NULL));

    while (1) {
        IpcPacket req;
        // Blocking read: Wait for Hub command
        if (ipc_recv_packet(socket_fd, &req) < 0) {
            LOG_INFO(TAG, "Hub disconnected.");
            break;
        }

        IpcPacket resp = {0};
        resp.header.sender_id = MOD_NETWORK;
        resp.header.request_id = req.header.request_id;
        resp.header.status = 0;

        if (req.header.type == MSG_LOG_ENTRY) {
            LOG_INFO(TAG, "Processing Log Upload...");
            send_log_to_server((char*)req.data);
            // No response necessary for logs, loop back to wait
        } 
        else if (req.header.type == MSG_REQUEST) {
            LOG_INFO(TAG, "Processing Data Request: %s", (char*)req.data);
            
            char response_str[512];
            request_data_from_server((char*)req.data, response_str, sizeof(response_str));
            
            resp.header.type = MSG_RESPONSE;
            ipc_set_payload(&resp, response_str);
            
            ipc_send_packet(socket_fd, &resp);
        }
    }
}

// ==============================================================================================
// MODULE REGISTRY
// ==============================================================================================

const ModuleDef MODULE_REGISTRY[MODULE_COUNT] = {
    { MOD_IMEI,    "IMEI_Extractor",  MODULE_TYPE_ONESHOT, 1001, 1001, "u:r:hub_imei:s0", mod_imei_entry },
    { MOD_PHONE,   "Phone_Prefs",     MODULE_TYPE_ONESHOT, 1002, 1002, "u:r:hub_phone:s0", mod_phone_entry },
    { MOD_NETWORK, "Network_Service", MODULE_TYPE_SERVICE, 1003, 1003, "u:r:hub_net:s0",   mod_network_entry }
};