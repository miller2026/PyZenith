/**
 * @file main.c
 * @brief Project Hub - Main Daemon Entry Point
 * * This file implements the central event loop, process management, and 
 * orchestration logic for the Project Hub system service.
 * * Architecture:
 * - Single-threaded reactor pattern using epoll.
 * - Manages child processes (Modules) via Unix Domain Sockets (DGRAM).
 * - Enforces strict isolation (UID/GID/SELinux) on child processes.
 * - Handles signals (SIGCHLD, SIGTERM) and global timeouts via file descriptors.
 */

#include "sal.h"
#include "modules.h"
#include "ipc_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>

// --- Configuration ---
#define MAX_EPOLL_EVENTS    10
#define GLOBAL_TIMEOUT_SEC  900 // 15 Minutes

// --- Global Context ---
// Consolidating globals into a context structure for better organization.
typedef struct {
    int epoll_fd;
    int signal_fd;
    int timer_fd;
    int active_module_count;
    int running; // Boolean flag for the main loop
} DaemonContext;

static DaemonContext g_ctx = {
    .epoll_fd = -1,
    .signal_fd = -1,
    .timer_fd = -1,
    .active_module_count = 0,
    .running = 0
};

// --- Business Logic State ---
typedef struct {
    int echo_ready;
    int long_task_done;
    int phone_data_retrieved;
} HubState;

static HubState g_logic_state = {0};

// --- Module Runtime State ---
typedef struct {
    int active;
    pid_t pid;
    int socket_fd;
    const ModuleDef* def;
} RunningModule;

static RunningModule g_modules[MODULE_COUNT];

// --- Sentinels for Epoll Pointers ---
// Used to distinguish system events from module events in epoll_data.ptr
static int SENTINEL_TIMER = 0;

// --- Forward Declarations ---
static int set_nonblocking(int fd);
static void send_to_module(int module_index, uint8_t type, const char* data);

// ==============================================================================================
// SECTION: Business Logic (The Orchestrator)
// ==============================================================================================

/**
 * @brief Handles incoming messages from modules.
 * Implements the core state machine of the application.
 */
static void logic_on_message(int mod_idx, uint8_t type, const uint8_t* data, uint16_t len) {
    const char* mod_name = g_modules[mod_idx].def->name;
    const char* TAG = "HubLogic";

    // 1. Generic Error Handling from Modules
    if (type == MSG_ERROR) {
        LOG_ERROR(TAG, "Module '%s' reported error: %.*s", mod_name, len, data);
        return;
    }

    // 2. Log Success Events
    if (len > 0 && len < 200) {
        LOG_INFO(TAG, "RX from %s: Type=0x%02X Data='%.*s'", mod_name, type, len, (const char*)data);
    } else {
        LOG_INFO(TAG, "RX from %s: Type=0x%02X (Len=%d)", mod_name, type, len);
    }

    // 3. State Machine & Orchestration using Enum IDs
    switch (g_modules[mod_idx].def->id) {
        case MOD_ECHO:
            if (type == MSG_RESPONSE) {
                g_logic_state.echo_ready = 1;
                LOG_INFO(TAG, "[State Update] Echo module is ready.");
            }
            break;

        case MOD_LONG_TASK:
            if (type == MSG_RESPONSE) {
                g_logic_state.long_task_done = 1;
                LOG_INFO(TAG, "[State Update] Long Task completed.");
                
                // Orchestration Action: If Echo is ready, notify it.
                if (g_logic_state.echo_ready) {
                    LOG_INFO(TAG, "[Action] Triggering Echo notification...");
                    send_to_module(MOD_ECHO, MSG_REQUEST, "LongTask Finished!"); 
                }
            }
            break;

        case MOD_PHONE_READER:
            if (type == MSG_RESPONSE) {
                g_logic_state.phone_data_retrieved = 1;
                LOG_INFO(TAG, "[State Update] Phone Data Retrieved Successfully: '%.*s'", len, (const char*)data);
            }
            break;
            
        case MOD_PROP_READER:
            if (type == MSG_RESPONSE) {
                LOG_INFO(TAG, "[State Update] System Property Value: '%.*s'", len, (const char*)data);
            }
            break;

        default:
            break;
    }
}

/**
 * @brief Handles module termination events.
 */
static void logic_on_exit(int mod_idx, int exit_code, int crashed) {
    const char* mod_name = g_modules[mod_idx].def->name;
    
    if (crashed) {
        LOG_ERROR("HubLogic", "CRITICAL: Module %s crashed (Signal %d)", mod_name, exit_code);
    } else {
        LOG_INFO("HubLogic", "Module %s exited cleanly (Code %d).", mod_name, exit_code);
    }
}

// ==============================================================================================
// SECTION: Process Management
// ==============================================================================================

/**
 * @brief Logic executed by the CHILD process immediately after fork.
 * Sets up isolation and executes the module entrypoint.
 * @note Does not return. Exits via _exit().
 */
static void run_child_process(int socket_fd, const ModuleDef* def) {
    // 1. Safety: Ensure child dies if parent (Hub) dies
    if (g_api->sys_prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
        _exit(1);
    }

    // 1.1 Race Condition Check: Ensure we weren't adopted by init before prctl was set
    if (getppid() == 1) {
        kill(getpid(), SIGKILL);
        _exit(1); 
    }

    // 2. Privilege Drop (Crucial Security Step)
    if (g_api->sys_setresgid(def->target_gid, def->target_gid, def->target_gid) != 0) {
        LOG_FATAL("HubChild", "Failed to set GID for %s", def->name);
        _exit(1);
    }
    if (g_api->sys_setresuid(def->target_uid, def->target_uid, def->target_uid) != 0) {
        LOG_FATAL("HubChild", "Failed to set UID for %s", def->name);
        _exit(1);
    }
    
    // 3. SELinux Transition
    if (g_api->selinux_setcon(def->target_selinux_context) != 0) {
        LOG_FATAL("HubChild", "Failed to set SELinux context for %s", def->name);
        _exit(1); 
    }

    // 4. Execution
    def->entrypoint(socket_fd);
    
    // 5. Cleanup
    close(socket_fd);
    _exit(0);
}

/**
 * @brief Reaps zombie processes.
 * Called when SIGCHLD is received.
 */
static void reap_zombies(void) {
    int status;
    pid_t pid;
    
    // Loop with WNOHANG to reap all currently dead children without blocking
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Map PID to Module
        for (int i = 0; i < MODULE_COUNT; i++) {
            if (g_modules[i].active && g_modules[i].pid == pid) {
                // Mark as inactive and close resource
                g_modules[i].active = 0;
                close(g_modules[i].socket_fd); 
                g_ctx.active_module_count--;

                // Determine exit reason
                int crashed = WIFSIGNALED(status);
                int code = crashed ? WTERMSIG(status) : WEXITSTATUS(status);
                
                // Notify Business Logic
                logic_on_exit(i, code, crashed);
            }
        }
    }
}

/**
 * @brief Spawns a single module.
 * Creates socket pair, forks, runs child logic, and registers parent socket with epoll.
 */
static void spawn_module(int index) {
    const ModuleDef* def = &MODULE_REGISTRY[index];
    int sv[2] = {-1, -1};
    pid_t pid = -1;

    // 1. Create Socket Pair
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        LOG_FATAL("HubCore", "Failed to create socketpair for %s: %s", def->name, strerror(errno));
        goto cleanup; // Nothing open yet, but consistent flow
    }

    // 2. Fork
    pid = fork();
    if (pid < 0) {
        LOG_FATAL("HubCore", "Failed to fork for %s: %s", def->name, strerror(errno));
        goto cleanup;
    }

    if (pid == 0) {
        // --- Child Process ---
        close(sv[0]); // Close parent end
        run_child_process(sv[1], def); // Does not return
    } else {
        // --- Parent Process ---
        close(sv[1]); // Close child end immediately
        sv[1] = -1;   // Mark as closed

        if (set_nonblocking(sv[0]) == -1) {
            LOG_FATAL("HubCore", "Failed to set non-blocking mode for %s", def->name);
            kill(pid, SIGKILL);
            goto cleanup;
        }

        // Register with Epoll
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = sv[0]; 
        ev.data.ptr = &g_modules[index]; 

        if (epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_ADD, sv[0], &ev) == -1) {
            LOG_FATAL("HubCore", "Failed to add module %s to epoll", def->name);
            kill(pid, SIGKILL);
            goto cleanup;
        }

        // Update State
        g_modules[index].active = 1;
        g_modules[index].pid = pid;
        g_modules[index].socket_fd = sv[0];
        g_modules[index].def = def;
        g_ctx.active_module_count++;

        LOG_INFO("HubCore", "Spawned module: %s (PID: %d)", def->name, pid);
    }
    
    return;

cleanup:
    // Resource cleanup in case of failure
    if (sv[0] != -1) close(sv[0]);
    if (sv[1] != -1) close(sv[1]);
    
    // If we forked but failed later in parent (e.g. epoll), we killed child above.
    // If fork failed, pid is -1, so we don't kill anything.
}

// ==============================================================================================
// SECTION: Infrastructure Setup
// ==============================================================================================

static int setup_signals(sigset_t* mask) {
    if (sigemptyset(mask) == -1) return -1;
    if (sigaddset(mask, SIGCHLD) == -1) return -1;
    if (sigaddset(mask, SIGTERM) == -1) return -1;
    if (sigaddset(mask, SIGINT) == -1) return -1;
    
    if (sigprocmask(SIG_BLOCK, mask, NULL) == -1) return -1;
    return 0;
}

static int setup_signalfd(const sigset_t* mask) {
    int sfd = signalfd(-1, mask, SFD_NONBLOCK);
    if (sfd == -1) return -1;

    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL }; 
    if (epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
        close(sfd);
        return -1;
    }
    return sfd;
}

static int setup_timerfd(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd == -1) return -1;

    struct itimerspec ts;
    ts.it_interval.tv_sec = 0; ts.it_interval.tv_nsec = 0; 
    ts.it_value.tv_sec = GLOBAL_TIMEOUT_SEC; ts.it_value.tv_nsec = 0;
    
    if (timerfd_settime(tfd, 0, &ts, NULL) == -1) {
        close(tfd);
        return -1;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = &SENTINEL_TIMER };
    if (epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_ADD, tfd, &ev) == -1) {
        close(tfd);
        return -1;
    }
    return tfd;
}

static int init_daemon(void) {
    if (sal_init() != 0) return -1;

    g_ctx.epoll_fd = epoll_create1(0);
    if (g_ctx.epoll_fd == -1) {
        LOG_FATAL("HubCore", "epoll_create1 failed: %s", strerror(errno));
        return -1;
    }

    sigset_t mask;
    if (setup_signals(&mask) != 0) {
        LOG_FATAL("HubCore", "Signal setup failed: %s", strerror(errno));
        return -1;
    }

    g_ctx.signal_fd = setup_signalfd(&mask);
    if (g_ctx.signal_fd == -1) {
        LOG_FATAL("HubCore", "signalfd setup failed: %s", strerror(errno));
        return -1;
    }

    g_ctx.timer_fd = setup_timerfd();
    if (g_ctx.timer_fd == -1) {
        LOG_FATAL("HubCore", "timerfd setup failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

// ==============================================================================================
// SECTION: Event Handlers
// ==============================================================================================

static void handle_signal_event(void) {
    struct signalfd_siginfo fdsi;
    ssize_t s = read(g_ctx.signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
    
    if (s != sizeof(struct signalfd_siginfo)) return;

    if (fdsi.ssi_signo == SIGCHLD) {
        reap_zombies();
    } 
    else if (fdsi.ssi_signo == SIGTERM || fdsi.ssi_signo == SIGINT) {
        LOG_INFO("HubCore", "Received termination signal. Requesting shutdown.");
        g_ctx.running = 0;
    }
}

static void handle_timer_event(void) {
    LOG_FATAL("HubCore", "Global Timeout Reached (%d seconds). Emergency Shutdown.", GLOBAL_TIMEOUT_SEC);
    g_ctx.running = 0;
}

static void handle_module_event(RunningModule* mod) {
    if (!mod->active) return;

    // Security: Allocate +1 byte to guarantee null termination for string logging
    uint8_t raw_buf[MAX_PACKET_SIZE + 1];
    ssize_t r = read(mod->socket_fd, raw_buf, MAX_PACKET_SIZE);

    if (r < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("HubCore", "Read error from %s: %s. Closing connection.", mod->def->name, strerror(errno));
            mod->active = 0;
            close(mod->socket_fd);
            g_ctx.active_module_count--;
        }
        return;
    }

    if (r == 0) return;

    if (r < HEADER_SIZE) {
        LOG_ERROR("HubCore", "Dropped undersized packet from %s (%zd bytes)", mod->def->name, r);
        return;
    }

    uint8_t type = raw_buf[0];
    uint16_t len = (raw_buf[1] << 8) | raw_buf[2];

    if (r != HEADER_SIZE + len) {
        LOG_ERROR("HubCore", "Packet Size Mismatch from %s. Expected %d, Got %zd", mod->def->name, HEADER_SIZE + len, r);
        return;
    }

    if (len > 0) raw_buf[HEADER_SIZE + len] = '\0';
    
    // Find index by pointer arithmetic or ID
    int mod_idx = mod - g_modules;
    logic_on_message(mod_idx, type, raw_buf + HEADER_SIZE, len);
}

// ==============================================================================================
// SECTION: Utilities & Main
// ==============================================================================================

static void send_to_module(int module_index, uint8_t type, const char* data) {
    if (!g_modules[module_index].active) return;
    
    int fd = g_modules[module_index].socket_fd;
    uint16_t len = data ? strlen(data) : 0;
    
    if (len + HEADER_SIZE > MAX_PACKET_SIZE) {
        LOG_ERROR("HubCore", "Cannot send message to %d: Payload too large", module_index);
        return;
    }

    uint8_t buffer[MAX_PACKET_SIZE];
    buffer[0] = type;
    buffer[1] = (len >> 8) & 0xFF;
    buffer[2] = len & 0xFF;
    if (len > 0) memcpy(buffer + HEADER_SIZE, data, len);

    if (write(fd, buffer, HEADER_SIZE + len) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
             LOG_FATAL("HubCore", "Critical IPC Failure: Failed to write to module %d: %s. Shutting down.", module_index, strerror(errno));
             g_ctx.running = 0; // Trigger shutdown
        }
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void shutdown_cleanup(void) {
    LOG_INFO("HubCore", "Initiating Shutdown Cleanup...");
    for (int i = 0; i < MODULE_COUNT; i++) {
        if (g_modules[i].active) {
            kill(g_modules[i].pid, SIGKILL);
        }
    }
    
    if (g_ctx.signal_fd != -1) close(g_ctx.signal_fd);
    if (g_ctx.timer_fd != -1) close(g_ctx.timer_fd);
    if (g_ctx.epoll_fd != -1) close(g_ctx.epoll_fd);
    
    sal_cleanup();
}

int main(void) {
    if (init_daemon() != 0) {
        return 1;
    }

    LOG_INFO("HubCore", "Daemon Initialized. Spawning modules...");
    g_ctx.running = 1;

    for (int i = 0; i < MODULE_COUNT; i++) {
        spawn_module(i);
        
        // Bootstrapping
        if (MODULE_REGISTRY[i].id == MOD_ECHO) {
             send_to_module(i, MSG_REQUEST, "Hello Hub");
        }
        else if (MODULE_REGISTRY[i].id == MOD_PHONE_READER) {
             send_to_module(i, MSG_REQUEST, "Start");
        }
        else if (MODULE_REGISTRY[i].id == MOD_PROP_READER) {
             send_to_module(i, MSG_REQUEST, "Start");
        }
    }

    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (g_ctx.running && g_ctx.active_module_count > 0) {
        int nfds = epoll_wait(g_ctx.epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        
        if (nfds == -1) {
            if (errno == EINTR) continue;
            LOG_FATAL("HubCore", "epoll_wait fatal error: %s", strerror(errno));
            break;
        }

        for (int n = 0; n < nfds; ++n) {
            void* ptr = events[n].data.ptr;

            if (ptr == NULL) {
                handle_signal_event();
            } 
            else if (ptr == &SENTINEL_TIMER) {
                handle_timer_event();
            }
            else {
                handle_module_event((RunningModule*)ptr);
            }
        }
    }

    shutdown_cleanup();
    return 0;
}
