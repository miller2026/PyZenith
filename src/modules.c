#define _GNU_SOURCE

#include "modules.h"
#include "../common/ipc/ipc.h"
#include "../common/sal/symbol_resolver.h"
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h> 

/* --- Helpers --- */

static char* read_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    
    size_t cap = 4096;
    size_t len = 0;
    char* buf = malloc(cap);

    // Limit max file size to prevent OOM
    while (buf && cap < (1024 * 1024)) {
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r <= 0) break;
        len += r;
        if (len + 512 >= cap) buf = realloc(buf, cap *= 2);
    }
    
    if (buf) buf[len] = 0;
    close(fd);
    return buf;
}

// Minimal XML extraction for <string name="key">value</string> or value="..."
static int xml_scan(const char* xml, const char* key, char* out, size_t max) {
    if (!xml || !key || !out || max == 0) return -1;
    
    char needle[128];
    snprintf(needle, sizeof(needle), "name=\"%s\"", key);
    
    const char* p = strstr(xml, needle);
    if (!p) return -1;

    // Check 1: value="..." attribute
    const char* val_attr = strstr(p, "value=\"");
    const char* close_tag = strchr(p, '>');
    
    if (val_attr && close_tag && val_attr < close_tag) {
        val_attr += 7; // Skip value="
        size_t i = 0;
        
        // Manual copy with bounds checking on both source and dest
        // Prevents reading past end of 'xml' string buffer if malformed
        while (val_attr[i] && val_attr[i] != '"' && i < max - 1) {
            out[i] = val_attr[i];
            i++;
        }
        out[i] = 0;
        return 0;
    }

    // Check 2: Inner text >value<
    if (close_tag) {
        const char* end_tag = strchr(close_tag, '<');
        if (end_tag) {
            size_t len = end_tag - (close_tag + 1);
            if (len >= max) len = max - 1;
            strncpy(out, close_tag + 1, len);
            out[len] = 0;
            return 0;
        }
    }
    return -1;
}

/* --- Implementations --- */

static void do_imei(int fd) {
    LOG_I("ModIMEI", "Start");

    ipc_packet_t req, resp = {0};
    if (ipc_recv(fd, &req) < 0) return;

    resp.head.type = MSG_RESP;
    resp.head.sender = MOD_IMEI;
    resp.head.req_id = req.head.req_id;

    char buf[128] = {0};
    // FIXME: Handle property lookup failure more gracefully?
    if (sys->prop_get("ro.id.imei", buf) > 0) {
        ipc_set_str(&resp, buf);
    } else {
        resp.head.status = -1;
        ipc_set_str(&resp, "IMEI not found");
    }

    ipc_send(fd, &resp);
}

static void do_phone(int fd) {
    LOG_I("ModPhone", "Start");

    ipc_packet_t req, resp = {0};
    if (ipc_recv(fd, &req) < 0) return;

    resp.head.type = MSG_RESP;
    resp.head.sender = MOD_PHONE;
    resp.head.req_id = req.head.req_id;

    // Hardcoded path for now.
    // TODO: Move to config file.
    char* xml = read_file("/data/data/com.android.phone/shared_prefs/com.android.phone_preferences.xml");
    char val[256] = {0};

    if (xml && xml_scan(xml, "phone_number_value", val, sizeof(val)) == 0) {
        ipc_set_str(&resp, val);
    } else {
        resp.head.status = -1;
        ipc_set_str(&resp, "Phone not found");
    }
    
    if (xml) free(xml);
    ipc_send(fd, &resp);
}

static void do_net(int fd) {
    LOG_I("ModNet", "Ready");
    srand(time(0));

    while (1) {
        ipc_packet_t req, resp = {0};
        if (ipc_recv(fd, &req) < 0) break;

        resp.head.sender = MOD_NET;
        resp.head.req_id = req.head.req_id;

        if (req.head.type == MSG_LOG) {
            LOG_I("NetLib", "UL Log: %s", req.data);
        }
        else if (req.head.type == MSG_REQ) {
            char buf[128];
            // Simulate network delay
            usleep(50000); 
            snprintf(buf, sizeof(buf), "ACK: %s (ID:%d)", req.data, rand() % 100);
            
            resp.head.type = MSG_RESP;
            ipc_set_str(&resp, buf);
            ipc_send(fd, &resp);
        }
    }
}

const mod_def_t MODULES[MOD_COUNT] = {
    { MOD_IMEI,  "IMEI",  TYPE_ONESHOT, 1001, 1001, "u:r:hub_imei:s0", do_imei },
    { MOD_PHONE, "Phone", TYPE_ONESHOT, 1002, 1002, "u:r:hub_phone:s0", do_phone },
    { MOD_NET,   "Net",   TYPE_SERVICE, 1003, 1003, "u:r:hub_net:s0",   do_net },
};