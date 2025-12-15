/**
 * @file main.c
 * @brief Project Hub - Main Daemon Entry Point
 */

#include "symbol_resolver.h"
#include "modules.h"
#include "ipc_defs.h"
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

// --- Configuration ---
#define MAX_EPOLL_EVENTS    10
#define GLOBAL_TIMEOUT_SEC  900 // 15 Minutes
#define TX_BUFFER_SIZE      (MAX_PACKET_SIZE * 4)

// --- Global Context ---
typedef struct {
    int epoll_fd;
    int signal_fd;
    int timer_fd;
    int active_module_count;
    int running; 
} DaemonContext;

static DaemonContext g_ctx = { -1, -1, -1, 0, 0 };

// --- Module Runtime State ---
typedef struct {
    int active;
    pid_t pid;
    int socket_fd;
    const ModuleDef* def;
    
    // Output Buffer
    uint8_t tx_buf[TX_BUFFER_SIZE];
    size_t tx_len;
    size_t tx_pos;
} RunningModule;

static RunningModule g_modules[MODULE_COUNT];

// --- Business Logic State ---
typedef struct {
    char imei[128];
    char phone_number[128];
    int imei_received; // 0=Waiting, 1=Success, 2=Error
    int phone_received;
    int final_request_sent;
} HubState;

static HubState g_logic_state = {
    .imei = {0},
    .phone_number = {0},
    .imei_received = 0,
    .phone_received = 0,
    .final_request_sent = 0
};

// --- Sentinels ---
static int SENTINEL_TIMER = 0;

// --- Forward Declarations ---
static void send_to_module(int module_index, uint8_t type, const char* data);
static int update_epoll_flags(int module_idx, uint32_t flags);
static void check_and_send_final_data(void);

// ==============================================================================================
// SECTION: Business Logic Helpers
// ==============================================================================================

/**
 * @brief Helper to send a log entry to the networking module.
 */
static void log_to_network(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Also log locally
    LOG_INFO("HubLogic", "[NetLog] %s", buf);
    send_to_module(MOD_NETWORK, MSG_LOG_ENTRY, buf);
}

/**
 * @brief Checks if all data collection modules are done, then triggers the network upload.
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

// ==============================================================================================
// SECTION: Business Logic Events
// ==============================================================================================

static void logic_on_message(int mod_idx, uint8_t type, const uint8_t* data, uint16_t len) {
    const char* mod_name = g_modules[mod_idx].def->name;
    ModuleID mod_id = g_modules[mod_idx].def->id;

    // --- 1. Handle Errors (Generic) ---
    if (type == MSG_ERROR) {
        log_to_network("Module %s errored: %.*s", mod_name, len, data);

        // Store fallback values for data modules
        if (mod_id == MOD_IMEI) {
            strncpy(g_logic_state.imei, "N/A", sizeof(g_logic_state.imei)-1);
            g_logic_state.imei_received = 2; // Error state
            check_and_send_final_data();
        } 
        else if (mod_id == MOD_PHONE) {
            strncpy(g_logic_state.phone_number, "N/A", sizeof(g_logic_state.phone_number)-1);
            g_logic_state.phone_received = 2; // Error state
            check_and_send_final_data();
        }
        else if (mod_id == MOD_NETWORK) {
            // Critical: If Network errors during final phase, shutdown
            if (g_logic_state.final_request_sent) {
                LOG_FATAL("HubLogic", "Network failed during final transmission. Shutting down.");
                g_ctx.running = 0;
            }
        }
        return;
    }

    // --- 2. Handle Responses ---
    switch (mod_id) {
        case MOD_IMEI:
            if (type == MSG_RESPONSE) {
                snprintf(g_logic_state.imei, sizeof(g_logic_state.imei), "%.*s", len, data);
                g_logic_state.imei_received = 1; // Success
                log_to_network("Module %s has responded", mod_name);
                check_and_send_final_data();
            }
            break;

        case MOD_PHONE:
            if (type == MSG_RESPONSE) {
                snprintf(g_logic_state.phone_number, sizeof(g_logic_state.phone_number), "%.*s", len, data);
                g_logic_state.phone_received = 1; // Success
                log_to_network("Module %s has responded", mod_name);
                check_and_send_final_data();
            }
            break;

        case MOD_NETWORK:
            if (type == MSG_RESPONSE) {
                // This is the confirmation from the server flow.
                LOG_INFO("HubLogic", "Network Transaction Complete. Server replied: '%.*s'", len, data);
                g_ctx.running = 0; // Mission Accomplished
            }
            break;

        default:
            break;
    }
}

static void logic_on_exit(int mod_idx, int exit_code, int crashed) {
    const char* mod_name = g_modules[mod_idx].def->name;
    ModuleID mod_id = g_modules[mod_idx].def->id;

    if (crashed) {
        LOG_ERROR("HubLogic", "CRITICAL: Module %s crashed (Signal %d)", mod_name, exit_code);
        log_to_network("Module %s crashed", mod_name);
    } else {
        LOG_INFO("HubLogic", "Module %s exited cleanly.", mod_name);
    }

    // Fail-safe: If a module exits without sending data and wasn't marked received yet
    if (mod_id == MOD_IMEI && !g_logic_state.imei_received) {
        LOG_ERROR("HubLogic", "IMEI module exited without data.");
        log_to_network("Module %s failed silently", mod_name);
        strncpy(g_logic_state.imei, "N/A", sizeof(g_logic_state.imei)-1);
        g_logic_state.imei_received = 2;
        check_and_send_final_data();
    }
    else if (mod_id == MOD_PHONE && !g_logic_state.phone_received) {
        LOG_ERROR("HubLogic", "Phone module exited without data.");
        log_to_network("Module %s failed silently", mod_name);
        strncpy(g_logic_state.phone_number, "N/A", sizeof(g_logic_state.phone_number)-1);
        g_logic_state.phone_received = 2;
        check_and_send_final_data();
    }
}

// ==============================================================================================
// SECTION: Process Management
// ==============================================================================================

static void run_child_process(int socket_fd, const ModuleDef* def) {
    if (g_api->sys_prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) _exit(1);
    if (getppid() == 1) { kill(getpid(), SIGKILL); _exit(1); }

    if (g_api->sys_setresgid(def->target_gid, def->target_gid, def->target_gid) != 0) _exit(1);
    if (g_api->sys_setresuid(def->target_uid, def->target_uid, def->target_uid) != 0) _exit(1);
    if (g_api->selinux_setcon(def->target_selinux_context) != 0) _exit(1);

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

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) goto cleanup;
    pid = fork();
    if (pid < 0) goto cleanup;

    if (pid == 0) {
        close(sv[0]); 
        run_child_process(sv[1], def);
    } else {
        close(sv[1]); sv[1] = -1;
        if (set_nonblocking(sv[0]) == -1) { kill(pid, SIGKILL); goto cleanup; }

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
        g_modules[index].tx_len = 0;
        g_modules[index].tx_pos = 0;
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
// SECTION: IPC & Buffer Management
// ==============================================================================================

static int update_epoll_flags(int module_idx, uint32_t flags) {
    struct epoll_event ev;
    ev.events = flags;
    ev.data.ptr = &g_modules[module_idx];
    
    if (epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_MOD, g_modules[module_idx].socket_fd, &ev) == -1) {
        LOG_ERROR("HubCore", "Failed to update epoll flags: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void handle_module_write(RunningModule* mod) {
    if (!mod->active || mod->tx_len == 0) return;

    ssize_t wrote = write(mod->socket_fd, mod->tx_buf + mod->tx_pos, mod->tx_len);
    
    if (wrote < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; 
        LOG_FATAL("HubCore", "Write failed on socket: %s", strerror(errno));
        g_ctx.running = 0; 
        return;
    }

    mod->tx_pos += wrote;
    mod->tx_len -= wrote;

    if (mod->tx_len == 0) {
        mod->tx_pos = 0;
        int mod_idx = mod - g_modules;
        update_epoll_flags(mod_idx, EPOLLIN);
    }
}

static void send_to_module(int module_index, uint8_t type, const char* data) {
    RunningModule* mod = &g_modules[module_index];
    if (!mod->active) return;
    
    uint16_t len = data ? strlen(data) : 0;
    
    uint8_t packet[MAX_PACKET_SIZE];
    ipc_serialize_header(packet, type, len);
    if (len > 0) memcpy(packet + HEADER_SIZE, data, len);
    
    size_t packet_size = HEADER_SIZE + len;

    if (mod->tx_len == 0) {
        ssize_t sent = write(mod->socket_fd, packet, packet_size);
        
        if (sent == packet_size) return; 

        if (sent < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
            LOG_FATAL("HubCore", "Write failed: %s", strerror(errno));
            g_ctx.running = 0; 
            return;
        }

        size_t written = (sent < 0) ? 0 : sent;
        size_t remaining = packet_size - written;
        
        if (remaining > TX_BUFFER_SIZE) {
             LOG_FATAL("HubCore", "TX Buffer Overflow (Packet too big)");
             g_ctx.running = 0;
             return;
        }

        memcpy(mod->tx_buf, packet + written, remaining);
        mod->tx_len = remaining;
        mod->tx_pos = 0;
        
        update_epoll_flags(module_index, EPOLLIN | EPOLLOUT);
    } 
    else {
        if (mod->tx_len + packet_size > TX_BUFFER_SIZE) {
             LOG_FATAL("HubCore", "TX Buffer Overflow (Queue full)");
             g_ctx.running = 0;
             return;
        }
        
        if (mod->tx_pos > 0) {
            memmove(mod->tx_buf, mod->tx_buf + mod->tx_pos, mod->tx_len);
            mod->tx_pos = 0;
        }
        
        memcpy(mod->tx_buf + mod->tx_len, packet, packet_size);
        mod->tx_len += packet_size;
        update_epoll_flags(module_index, EPOLLIN | EPOLLOUT);
    }
}

// ==============================================================================================
// SECTION: Infrastructure & Main
// ==============================================================================================

static void handle_module_read(RunningModule* mod) {
    uint8_t raw_buf[MAX_PACKET_SIZE + 1];
    ssize_t r = read(mod->socket_fd, raw_buf, MAX_PACKET_SIZE);

    if (r < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("HubCore", "Read error: %s", strerror(errno));
            mod->active = 0;
            close(mod->socket_fd);
            g_ctx.active_module_count--;
        }
        return;
    }

    if (r == 0) return; 

    if (r < HEADER_SIZE) {
        LOG_ERROR("HubCore", "Undersized packet");
        return;
    }
    
    uint8_t type;
    uint16_t len;
    ipc_parse_header(raw_buf, &type, &len);

    if (r != HEADER_SIZE + len) {
        LOG_ERROR("HubCore", "Size Mismatch");
        return;
    }

    if (len > 0) raw_buf[HEADER_SIZE + len] = '\0';
    logic_on_message(mod - g_modules, type, raw_buf + HEADER_SIZE, len);
}

static void shutdown_cleanup(void) {
    LOG_INFO("HubCore", "Shutdown...");
    for (int i = 0; i < MODULE_COUNT; i++) if (g_modules[i].active) kill(g_modules[i].pid, SIGKILL);
    if (g_ctx.signal_fd != -1) close(g_ctx.signal_fd);
    if (g_ctx.timer_fd != -1) close(g_ctx.timer_fd);
    if (g_ctx.epoll_fd != -1) close(g_ctx.epoll_fd);
    symbol_resolver_cleanup();
}

static int init_daemon(void) {
    if (symbol_resolver_init() != 0) return -1;
    if ((g_ctx.epoll_fd = epoll_create1(0)) == -1) return -1;
    
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGCHLD); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    
    if ((g_ctx.signal_fd = signalfd(-1, &mask, SFD_NONBLOCK)) == -1) return -1;
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(g_ctx.epoll_fd, EPOLL_CTL_ADD, g_ctx.signal_fd, &ev);

    if ((g_ctx.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) == -1) return -1;
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

    // 1. Start all modules
    for (int i = 0; i < MODULE_COUNT; i++) {
        spawn_module(i);
    }
    
    // 2. Initial Log to Network
    log_to_network("Daemon started running");

    // 3. Trigger Data Modules
    // Use proper module lookups just in case indexing changes, though simple loop is O(N)
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
                struct signalfd_siginfo fdsi;
                if (read(g_ctx.signal_fd, &fdsi, sizeof(fdsi)) == sizeof(fdsi)) {
                    if (fdsi.ssi_signo == SIGCHLD) reap_zombies();
                    else g_ctx.running = 0;
                }
            } else if (ptr == &SENTINEL_TIMER) {
                LOG_FATAL("HubCore", "Global Timeout"); g_ctx.running = 0;
            } else {
                RunningModule* mod = (RunningModule*)ptr;
                if (!mod->active) continue;
                
                if (events[n].events & EPOLLIN) handle_module_read(mod);
                if (events[n].events & EPOLLOUT) handle_module_write(mod);
            }
        }
    }

    shutdown_cleanup();
    return 0;
}