#include "symbol_resolver.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static sys_api_t internal_sys;
const sys_api_t* sys = &internal_sys;

static void* load(const char* lib) {
    void* h = dlopen(lib, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "[FATAL] Missing %s: %s\n", lib, dlerror());
        exit(1);
    }
    return h;
}

static void* sym(void* h, const char* name) {
    void* p = dlsym(h, name);
    if (!p) {
        fprintf(stderr, "[FATAL] Missing symbol %s\n", name);
        exit(1);
    }
    return p;
}

int sal_init(void) {
    void* libc = load("libc.so");
    void* log  = load("liblog.so");
    void* sel  = load("libselinux.so");

    internal_sys.log       = (f_log_print) sym(log, "__android_log_print");
    internal_sys.setcon    = (f_setcon)    sym(sel, "setcon");
    internal_sys.setresuid = (f_setresuid) sym(libc, "setresuid");
    internal_sys.setresgid = (f_setresgid) sym(libc, "setresgid");
    internal_sys.prctl     = (f_prctl)     sym(libc, "prctl");
    internal_sys.prop_get  = (f_prop_get)  sym(libc, "__system_property_get");

    return 0;
}

void sal_cleanup(void) {
    // We intentionally don't dlclose() system libs to avoid teardown races
}