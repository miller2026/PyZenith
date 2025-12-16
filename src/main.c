/*
 * Project Hub: Android System Daemon
 * * Usage: project_hub [-v] [-t timeout]
 */

#include "common/sal/symbol_resolver.h"
#include "common/ipc/ipc.h"
#include "modules/modules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <sys/prctl.h>
#include <getopt.h>

#define MAX_EVENTS 10
#define DEFAULT_TIMEOUT 900

// --- App Context ---
static struct {
    int epoll_fd;
    int sig_fd;
    int timer_fd;
    int active_mods;
    int running;
    int verbose;
    int timeout;
} app = {
    .epoll_fd = -1,
    .sig_fd = -1,
    .timer_fd = -1,
    .timeout = DEFAULT_TIMEOUT
};

static struct {
    int   active;
    pid_t pid;
    int   fd;
    const mod_def_t* def;
} workers[MOD_COUNT];

static struct {
    char imei[128];
    char phone[128];
    bool has_imei;
    bool has_phone;
    bool sent;
} state;

static int timer_trigger = 0;

/* --- Logic --- */

static void bus_send(int id, int type, const char* data) {
    if (!workers[id].active) return;
    
    ipc_packet_t pkt = {0};
    pkt.head.type = type;
    pkt.head.sender = -1; // Hub
    ipc_set_str(&pkt, data);

    if (ipc_send(workers[id].fd, &pkt) != 0 && errno != EAGAIN) {
        LOG_F("Hub", "Send failed to %s: %s", workers[id].def->name, strerror(errno));
        app.running = 0;
    }
}

static void try_finalize(void) {
    if (state.has_imei && state.has_phone && !state.sent) {
        if (app.verbose) LOG_I("Hub", "Data collected. Uploading...");
        
        char buf[512];
        snprintf(buf, sizeof(buf), "IMEI:%s PHONE:%s", state.imei, state.phone);
        bus_send(MOD_NET, MSG_REQ, buf);
        state.sent = 1;
    }
}

static void on_msg(int id, const ipc_packet_t* pkt) {
    const char* name = workers[id].def->name;

    if (pkt->head.status != 0) {
        LOG_E("Hub", "%s error: %s", name, pkt->data);
        
        // Mark missing data as N/A to allow completion
        if (id == MOD_IMEI) {
            strcpy(state.imei, "N/A");
            state.has_imei = 1;
        }
        else if (id == MOD_PHONE) {
            strcpy(state.phone, "N/A");
            state.has_phone = 1;
        }
        else if (id == MOD_NET && state.sent) {
            LOG_F("Hub", "Network failed final upload");
            app.running = 0;
        }
        try_finalize();
        return;
    }

    if (pkt->head.type == MSG_RESP) {
        if (id == MOD_IMEI) {
            strcpy(state.imei, (char*)pkt->data);
            state.has_imei = 1;
        }
        else if (id == MOD_PHONE) {
            strcpy(state.phone, (char*)pkt->data);
            state.has_phone = 1;
        }
        else if (id == MOD_NET) {
            LOG_I("Hub", "Transaction Complete: %s", pkt->data);
            app.running = 0; 
        }
        
        if (app.verbose && id != MOD_NET) {
            LOG_I("Hub", "Recv from %s", name);
        }
        try_finalize();
    }
}

static void on_exit(int id, int status) {
    if (id == MOD_NET) {
        LOG_F("Hub", "Network module died. Aborting.");
        app.running = 0;
        return;
    }
    
    // If a collector died before reporting, fill N/A
    if (id == MOD_IMEI && !state.has_imei) {
        strcpy(state.imei, "N/A"); 
        state.has_imei = 1;
    }
    else if (id == MOD_PHONE && !state.has_phone) {
        strcpy(state.phone, "N/A");
        state.has_phone = 1;
    }
    try_finalize();
}

/* --- Process Management --- */

static void run_worker(int fd, const mod_def_t* def) {
    // Die if parent dies (Anti-Zombie)
    if (sys->prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) _exit(1);
    if (getppid() == 1) _exit(1);

    memset(&state, 0, sizeof(state));

    // Optimisation: Don't iterate 32k FDs
    int max = sysconf(_SC_OPEN_MAX);
    if (max > 1024) max = 1024;
    for (int i = 3; i < max; i++) {
        if (i != fd) close(i);
    }

    // Drop Privs
    if (sys->setcon(def->se_ctx) != 0) _exit(1);
    if (sys->setresgid(def->gid, def->gid, def->gid) != 0) _exit(1);
    if (sys->setresuid(def->uid, def->uid, def->uid) != 0) _exit(1);

    def->run(fd);
    _exit(0);
}

static void reap(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MOD_COUNT; i++) {
            if (workers[i].active && workers[i].pid == pid) {
                workers[i].active = 0;
                close(workers[i].fd);
                app.active_mods--;
                on_exit(i, status);
            }
        }
    }
}

static void spawn(int i) {
    const mod_def_t* def = &MODULES[i];
    int sv[2];
    
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) return;
    
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        run_worker(sv[1], def);
    } else if (pid > 0) {
        close(sv[1]);
        fcntl(sv[0], F_SETFL, O_NONBLOCK);
        
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = &workers[i] };
        epoll_ctl(app.epoll_fd, EPOLL_CTL_ADD, sv[0], &ev);

        workers[i] = (typeof(workers[0])){ 1, pid, sv[0], def };
        app.active_mods++;
        
        if (app.verbose) LOG_I("Hub", "Spawned %s (pid:%d)", def->name, pid);
    }
}

/* --- Entry --- */

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-v] [-t timeout_sec]\n", prog);
    exit(1);
}

int main(int argc, char** argv) {

    if (sal_init() != 0) return 1;

    app.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    
    // Signals
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    app.sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    
    struct epoll_event ev_sig = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(app.epoll_fd, EPOLL_CTL_ADD, app.sig_fd, &ev_sig);

    // Timer
    app.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec ts = { {0,0}, {app.timeout, 0} };
    timerfd_settime(app.timer_fd, 0, &ts, NULL);
    
    struct epoll_event ev_tmr = { .events = EPOLLIN, .data.ptr = &timer_trigger };
    epoll_ctl(app.epoll_fd, EPOLL_CTL_ADD, app.timer_fd, &ev_tmr);

    app.running = 1;
    for (int i = 0; i < MOD_COUNT; i++) spawn(i);
    
    // Kickoff
    bus_send(MOD_IMEI, MSG_REQ, "START");
    bus_send(MOD_PHONE, MSG_REQ, "START");

    struct epoll_event events[MAX_EVENTS];
    while (app.running && app.active_mods > 0) {
        int n = epoll_wait(app.epoll_fd, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < n; i++) {
            void* p = events[i].data.ptr;
            
            if (!p) { // Signal
                struct signalfd_siginfo si;
                if (read(app.sig_fd, &si, sizeof(si)) > 0) {
                    if (si.ssi_signo == SIGCHLD) reap();
                    else app.running = 0;
                }
            }
            else if (p == &timer_trigger) {
                LOG_F("Hub", "Global Timeout");
                app.running = 0;
            }
            else { // Worker
                int id = (typeof(workers)*)p - workers;
                ipc_packet_t pkt;
                if (ipc_recv(workers[id].fd, &pkt) == 0) {
                    on_msg(id, &pkt);
                } else if (errno != EAGAIN) {
                    workers[id].active = 0;
                    close(workers[id].fd);
                    app.active_mods--;
                }
            }
        }
    }

    // Teardown
    for (int i = 0; i < MOD_COUNT; i++) {
        if (workers[i].active) kill(workers[i].pid, SIGKILL);
    }
    sal_cleanup();
    return 0;
}