/**
 * @file symbol_resolver.c
 * @brief Implementation of dynamic loading logic.
 */

#include "symbol_resolver.h"
#include <dlfcn.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

// Android Log Constants
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
#define LOG_TAG "ProjectHub"

static SalContext g_sal_ctx = {0};

int sal_init(void) {
    if (g_sal_ctx.initialized) return 0;

    // 1. liblog (Logging)
    g_sal_ctx.handle_liblog = dlopen("liblog.so", RTLD_LAZY);
    if (g_sal_ctx.handle_liblog) {
        g_sal_ctx.log_print = (pfn_android_log_print)dlsym(g_sal_ctx.handle_liblog, "__android_log_print");
    }

    // 2. libc (System Properties)
    // Note: dlopen on libc handles cases where symbols aren't exposed globally by default
    g_sal_ctx.handle_libc = dlopen("libc.so", RTLD_LAZY);
    if (g_sal_ctx.handle_libc) {
        g_sal_ctx.prop_get = (pfn_system_property_get)dlsym(g_sal_ctx.handle_libc, "__system_property_get");
    }

    // 3. libselinux (Process Contexts)
    g_sal_ctx.handle_libselinux = dlopen("libselinux.so", RTLD_LAZY);
    if (g_sal_ctx.handle_libselinux) {
        g_sal_ctx.set_con = (pfn_setcon)dlsym(g_sal_ctx.handle_libselinux, "setcon");
    }

    g_sal_ctx.initialized = 1;
    return 0;
}

void sal_cleanup(void) {
    if (g_sal_ctx.handle_liblog) dlclose(g_sal_ctx.handle_liblog);
    if (g_sal_ctx.handle_libc) dlclose(g_sal_ctx.handle_libc);
    if (g_sal_ctx.handle_libselinux) dlclose(g_sal_ctx.handle_libselinux);
    g_sal_ctx.initialized = 0;
}

void sal_log_info(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_sal_ctx.log_print) {
        g_sal_ctx.log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", buf);
    } else {
        // Fallback to stdout if liblog missing
        printf("[INFO] %s: %s\n", LOG_TAG, buf);
    }
}

void sal_log_error(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_sal_ctx.log_print) {
        g_sal_ctx.log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s", buf);
    } else {
        fprintf(stderr, "[ERROR] %s: %s\n", LOG_TAG, buf);
    }
}

int sal_get_property(const char* key, char* value) {
    if (g_sal_ctx.prop_get) {
        return g_sal_ctx.prop_get(key, value);
    }
    return 0;
}

int sal_set_selinux_context(const char* context) {
    if (g_sal_ctx.set_con) {
        return g_sal_ctx.set_con(context);
    }
    return -1;
}


