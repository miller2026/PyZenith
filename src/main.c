/**
 * @file main.c
 * @brief Project Hub - Main Daemon Entry Point.
 * * Orchestrates child modules using epoll and non-blocking I/O.
 * Implements the core business logic state machine and process lifecycle management.
 */

#include "symbol_resolver.h"
#include "modules.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>

// ==============================================================================================
// CONFIGURATION
// ==============================================================================================

#define MAX_EPOLL_EVENTS    10
#define GLOBAL_TIMEOUT_SEC  900 // 15 Minutes shutdown timer

// ==============================================================================================
// DATA STRUCTURES
// ==============================================================================================

typedef struct {
    int epoll_fd;
    int signal_fd;
    int timer_fd;
    int active_module_count;
    volatile int running; 
} DaemonContext;

static DaemonContext g_ctx = { -1, -1, -1, 0, 0 };

/**
 * @brief Represents a running child process managed by the Hub.
 */
typedef struct {
    int active;
    pid_t pid;
    int socket_fd;
    const ModuleDef* def;
} RunningModule;

static RunningModule g_modules[MODULE_COUNT];

/**
 * @brief Global Business Logic State.
 * This structure holds the data collected from modules.
 * @warning Must be cleared in child processes to prevent heap pollution.
 */
typedef struct {
    char imei[128];
    char phone_number[128];
    int imei_received;      // 0=Waiting, 1=Success, 2=Error
    int phone_received;
    int final_request_sent; // Prevents duplicate submissions
} HubState;

static HubState g_logic_state = {
    .imei = {0},
    .phone_number = {0},
    .imei_received = 0,
    .phone_received = 0,
    .final_request_sent = 0
};

static int SENTINEL_TIMER = 0;

// ==============================================================================================
// FORWARD DECLARATIONS
// ==============================================================================================

static void send_to_module(int module_index, uint8_t type, const char* data);
static void check_and_send_final_data(void);

// ==============================================================================================
// BUSINESS LOGIC
// ==============================================================================================

static void log_to_network(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    LOG_INFO("HubLogic", "[NetLog] %s", buf);
    send_to_module(MOD_NETWORK, MSG_LOG_ENTRY, buf);
}

/**
 * @brief Checks if all required data (IMEI, Phone) is available.
 * If so, sends the aggregated report to the Network module.
 */
static void check_and_send_final_data(void) {
    if (g_logic_state.imei_received && g_logic_state.phone_received && !g_logic_state.final_request_sent) {
        LOG_INFO("HubLogic", "All data collected. Sending to Network Module.");
        
        char payload[1024];
        snprintf(payload, sizeof(payload), "IMEI:%s, PHONE:%s", 
                 g_logic_state.imei, g_logic_state.phone_number);
        
        send_to_module(MOD_NETWORK, MSG_REQUEST, payload);
        g_logic_state.final_request_sent = 1;
    }
}

/**
 * @brief State Machine: Processing incoming packets.
 */
static void logic_on_message(int mod_idx, const IpcPacket* packet) {
    const char* mod_name = g_modules[mod_idx].def->name;
    ModuleID mod_id = g_modules[mod_idx].def->id;

    // 1. Handle Errors reported by the Module
    if (packet->header.status != 0) {
        log_to_network("Module %s errored: %s", mod_name, packet->data);

        // Fail-safe: Mark data as missing/error so the flow continues
        if (mod_id == MOD_IMEI) {
            strncpy(g_logic_state.imei, "N/A", sizeof(g_logic_state.imei)-1);
            g_logic_state.imei_received = 2; 
            check_and_send_final_data();
        } 
        else if (mod_id == MOD_PHONE) {
            strncpy(g_logic_state.phone_number, "N/A", sizeof(g_logic_state.phone_number)-1);
            g_logic_state.phone_received = 2; 
            check_and_send_final_data();
        }
        else if (mod_id == MOD_NETWORK) {
            if (g_logic_state.final_request_sent) {
                LOG_FATAL("HubLogic", "Network failed during final transmission. Shutting down.");
                g_ctx.running = 0;
            }
        }
        return;
    }

    // 2. Handle Success Responses
    switch (mod_id) {
        case MOD_IMEI:
            if (packet->header.type == MSG_RESPONSE) {
                snprintf(g_logic_state.imei, sizeof(g_logic_state.imei), "%s", packet->data);
                g_logic_state.imei_received = 1; 
                log_to_network("Module %s has responded", mod_name);
                check_and_send_final_data();
            }
            break;
        case MOD_PHONE:
            if (packet->header.type == MSG_RESPONSE) {
                snprintf(g_logic_state.phone_number, sizeof(g_logic_state.phone_number), "%s", packet->data);
                g_logic_state.phone_received = 1; 
                log_to_network("Module %s has responded", mod_name);
                check_and_send_final_data();
            }
            break;
        case MOD_NETWORK:
            if (packet->header.type == MSG_RESPONSE) {
                LOG_INFO("HubLogic", "Network Transaction Complete. Server replied: '%s'", packet->data);
                g_ctx.running = 0; 
            }
            break;
        default:
            break;
    }
}

/**
 * @brief State Machine: Handling process exit.
 */
static void logic_on_exit(int mod_idx, int exit_code, int crashed) {
    const char* mod_name = g_modules[mod_idx].def->name;
    ModuleID mod_id = g_modules[mod_idx].def->id;

    if (crashed) {
        LOG_ERROR("HubLogic", "CRITICAL: Module %s crashed (Signal %d)", mod_name, exit_code);
        log_to_network("Module %s crashed", mod_name);
    } else {
        LOG_INFO("HubLogic", "Module %s exited cleanly.", mod_name);
    }

    // POLICY: Network Failure is Fatal
    if (mod_id == MOD_NETWORK) {
        LOG_FATAL("HubLogic", "CRITICAL: Network Module Died. Daemon cannot continue.");
        g_ctx.running = 0;
        return;
    }

    // POLICY: Info Extraction Failure is Non-Fatal (Mark N/A and continue)
    if (mod_id == MOD_IMEI && !g_logic_state.imei_received) {
        LOG_ERROR("HubLogic", "IMEI module exited without data. Defaulting to N/A.");
        strncpy(g_logic_state.imei, "N/A", sizeof(g_logic_state.imei)-1);
        g_logic_state.imei_received = 2;
        check_and_send_final_data();
    }
    else if (mod_id == MOD_PHONE && !g_logic_state.phone_received) {
        LOG_ERROR("HubLogic", "Phone module exited without data. Defaulting to N/A.");
        strncpy(g_logic_state.phone_number, "N/A", sizeof(g_logic_state.phone_number)-1);
        g_logic_state.phone_received = 2;
        check_and_send_final_data();
    }
}

// ==============================================================================================
// PROCESS MANAGEMENT
// ==============================================================================================

/**
 * @brief Prepares and executes the child process logic.
 * * Performs:
 * 1. "Leave-No-Trace" safety (PDEATHSIG).
 * 2. Sensitive memory clearing.
 * 3. File descriptor cleanup.
 * 4. Privilege dropping (SELinux, GID, UID).
 * 5. Execution of module entry point.
 */
static void run_child_process(int socket_fd, const ModuleDef* def) {
    // 1. Establish Parent-Death signal to ensure cleanup if Hub crashes
    if (g_api->sys_prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) _exit(1);
    
    // Safety check: Race condition where parent died before PR_SET_PDEATHSIG was set
    if (getppid() == 1) { 
        kill(getpid(), SIGKILL); 
        _exit(1); 
    }

    // 2. Clear Sensitive Data from Heap
    // The child inherits the parent's heap. We must zero out the business logic state
    // so Module A doesn't see Module B's potential future data (or stale data).
    memset(&g_logic_state, 0, sizeof(g_logic_state));

    // 3. Close Inherited File Descriptors
    // Optimized to avoid iterating 32k FDs on Android.
    int max_fd = (int)sysconf(_SC_OPEN_MAX);
    // Optimization: If max_fd is unreasonably large (common on Android), cap it for startup speed
    // unless the app actually uses thousands of FDs.
    if (max_fd > 1024) max_fd = 1024; 
    
    for (int fd = 3; fd < max_fd; ++fd) {
        if (fd != socket_fd) close(fd);
    }

    // 4. Apply Isolation (Order matters: Context -> Group -> User)
    if (g_api->selinux_setcon(def->target_selinux_context) != 0) _exit(1);
    if (g_api->sys_setresgid(def->target_gid, def->target_gid, def->target_gid) != 0) _exit(1);
    if (g_api->sys_setresuid(def->target_uid, def->target_uid, def->target_uid) != 0) _exit(1);

    // 5. Run Module
    def->entrypoint(socket_fd);
    
    close(socket_fd);
    _exit(0);
}

static void reap_zombies(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MODULE_COUNT; i++) {
            if (g_modules[i].active && g_modules[i].pid == pid) {
                g_modules[i].active = 0;
                close(g_modules[i].socket_fd); 
                g_ctx.active_module_count--;
                logic_on_exit(i, WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status), WIFSIGNALED(status));
            }
        }
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void spawn_module(int index) {
    const ModuleDef* def = &MODULE_REGISTRY[index];
    int sv[2] = {-1, -1};
    pid_t pid = -1;

    // socketpair creates BLOCKING sockets by default
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) goto cleanup;
    
    pid = fork();
    if (pid < 0) goto cleanup;

    if (pid == 0) {
        close(sv[0]); 
        // Child uses sv[1], which remains BLOCKING
        run_child_process(sv[1], def);
    } else {
        close(sv[1]); sv[1] = -1;
        
        // Parent uses sv[0], set to NON-BLOCKING for epoll
        if (set_nonblocking(sv[0]) == -1) { 
            kill(pid, SIGKILL); goto cleanup; 
        }

        struct epoll_event ev;
        ev.events = EPOLLIN; 
        ev.data.fd = sv[0]; 
        ev.data.ptr = &g_modules[index]; 

        if (epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_ADD, sv[0], &ev) == -1) {
            kill(pid, SIGKILL); goto cleanup;
        }

        g_modules[index].active = 1;
        g_modules[index].pid = pid;
        g_modules[index].socket_fd = sv[0];
        g_modules[index].def = def;
        g_ctx.active_module_count++;
        LOG_INFO("HubCore", "Spawned module: %s (PID: %d)", def->name, pid);
    }
    return;

cleanup:
    if (sv[0] != -1) close(sv[0]);
    if (sv[1] != -1) close(sv[1]);
    LOG_FATAL("HubCore", "Failed to spawn module %s", def->name);
    g_ctx.running = 0;
}

// ==============================================================================================
// IPC & EVENT HANDLING (NON-BLOCKING)
// ==============================================================================================

static void handle_module_read(RunningModule* mod) {
    IpcPacket packet;
    
    // Attempt Non-Blocking Read
    int ret = ipc_recv_packet(mod->socket_fd, &packet);

    if (ret == 0) {
        logic_on_message(mod - g_modules, &packet);
        return;
    }

    // Handle EWOULDBLOCK/EAGAIN gracefully
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
    }

    // Handle Real Errors or EOF
    if (errno == 0) {
        // EOF (Clean close)
    } else {
        LOG_ERROR("HubCore", "Read error from %s: %s", mod->def->name, strerror(errno));
    }

    // Close Connection
    mod->active = 0;
    close(mod->socket_fd);
    g_ctx.active_module_count--;
}

static void send_to_module(int module_index, uint8_t type, const char* data) {
    RunningModule* mod = &g_modules[module_index];
    if (!mod->active) return;
    
    IpcPacket pkt = {0};
    pkt.header.type = type;
    pkt.header.sender_id = -1; // Hub ID
    pkt.header.status = 0;
    
    ipc_set_payload(&pkt, data);

    // Non-Blocking Write Attempt
    int ret = ipc_send_packet(mod->socket_fd, &pkt);

    if (ret != 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Drop packet strategy to prevent head-of-line blocking
            LOG_ERROR("HubCore", "Dropped packet to %s (Buffer Full)", mod->def->name);
        } else {
            LOG_FATAL("HubCore", "Write failed: %s", strerror(errno));
            g_ctx.running = 0;
        }
    }
}

// ==============================================================================================
// INITIALIZATION
// ==============================================================================================

static void shutdown_cleanup(void) {
    LOG_INFO("HubCore", "Shutdown...");
    for (int i = 0; i < MODULE_COUNT; i++) {
        if (g_modules[i].active) {
            kill(g_modules[i].pid, SIGKILL);
            close(g_modules[i].socket_fd);
        }
    }
    if (g_ctx.signal_fd != -1) close(g_ctx.signal_fd);
    if (g_ctx.timer_fd != -1) close(g_ctx.timer_fd);
    if (g_ctx.epoll_fd != -1) close(g_ctx.epoll_fd);
    symbol_resolver_cleanup();
}

static int init_daemon(void) {
    if (symbol_resolver_init() != 0) return -1;
    if ((g_ctx.epoll_fd = epoll_create1(EPOLL_CLOEXEC)) == -1) return -1;
    
    // Setup Signal Masking (SIGCHLD/TERM/INT)
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGCHLD); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    
    // Setup Signal FD
    if ((g_ctx.signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)) == -1) return -1;
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_ADD, g_ctx.signal_fd, &ev);

    // Setup Watchdog Timer
    if ((g_ctx.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) == -1) return -1;
    struct itimerspec ts = { {0,0}, {GLOBAL_TIMEOUT_SEC, 0} };
    timerfd_settime(g_ctx.timer_fd, 0, &ts, NULL);
    struct epoll_event ev_t = { .events = EPOLLIN, .data.ptr = &SENTINEL_TIMER };
    epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_ADD, g_ctx.timer_fd, &ev_t);

    return 0;
}

int main(void) {
    if (init_daemon() != 0) return 1;

    LOG_INFO("HubCore", "Daemon Initialized.");
    g_ctx.running = 1;

    // Spawn all modules
    for (int i = 0; i < MODULE_COUNT; i++) {
        spawn_module(i);
    }
    
    log_to_network("Daemon started running");

    // Kickstart processes
    for (int i = 0; i < MODULE_COUNT; i++) {
        if (MODULE_REGISTRY[i].id == MOD_IMEI || MODULE_REGISTRY[i].id == MOD_PHONE) {
            send_to_module(i, MSG_REQUEST, "Start");
        }
    }

    struct epoll_event events[MAX_EPOLL_EVENTS];
    while (g_ctx.running && g_ctx.active_module_count > 0) {
        int nfds = epoll_wait(g_ctx.epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        if (nfds == -1 && errno != EINTR) break;

        for (int n = 0; n < nfds; ++n) {
            void* ptr = events[n].data.ptr;
            
            if (ptr == NULL) {
                // Signal Event
                struct signalfd_siginfo fdsi;
                if (read(g_ctx.signal_fd, &fdsi, sizeof(fdsi)) == sizeof(fdsi)) {
                    if (fdsi.ssi_signo == SIGCHLD) reap_zombies();
                    else g_ctx.running = 0;
                }
            } 
            else if (ptr == &SENTINEL_TIMER) {
                // Timer Event
                LOG_FATAL("HubCore", "Global Timeout Reached"); 
                g_ctx.running = 0;
            } 
            else {
                // Module I/O Event
                RunningModule* mod = (RunningModule*)ptr;
                if (!mod->active) continue;
                if (events[n].events & EPOLLIN) handle_module_read(mod);
            }
        }
    }

    shutdown_cleanup();
    return 0;
}