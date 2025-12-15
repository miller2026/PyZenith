/**
 * @file symbol_resolver.c
 * @brief System Abstraction Layer Implementation.
 * * Handles the manual loading of Android shared libraries via dlopen/dlsym.
 * Fails hard (exit) if critical system symbols are missing.
 */

#include "symbol_resolver.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

// Global storage for the API
static SystemAPI internal_api;
const SystemAPI* g_api = &internal_api;

// ==============================================================================================
// SECTION: Internal Helpers
// ==============================================================================================

/**
 * @brief Loads a shared library.
 * Exits program on failure as these are critical dependencies.
 */
static void* load_lib_or_die(const char* name) {
    dlerror(); // Clear error
    
    void* handle = dlopen(name, RTLD_NOW);
    if (!handle) {
        // Fallback to stderr. In Android this might be /dev/null, 
        // but it's the best we can do before liblog is loaded.
        fprintf(stderr, "[SYMBOL_RESOLVER_FATAL] Failed to load library '%s': %s\n", name, dlerror());
        exit(EXIT_FAILURE);
    }
    return handle;
}

/**
 * @brief Resolves a symbol from a library handle.
 * Exits program on failure.
 */
static void* load_sym_or_die(void* handle, const char* symbol) {
    dlerror(); 
    
    void* ptr = dlsym(handle, symbol);
    const char* error = dlerror();
    
    if (error != NULL) {
        fprintf(stderr, "[SYMBOL_RESOLVER_FATAL] Failed to resolve symbol '%s': %s\n", symbol, error);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

// ==============================================================================================
// SECTION: Public API
// ==============================================================================================

int symbol_resolver_init(void) {
    // 1. Load Libraries
    // Note: Library names may vary slightly by Android version.
    internal_api.handle_libc       = load_lib_or_die("libc.so");
    internal_api.handle_liblog     = load_lib_or_die("liblog.so");
    internal_api.handle_libselinux = load_lib_or_die("libselinux.so");

    // 2. Resolve Logging
    internal_api.log_print = (func_android_log_print)load_sym_or_die(internal_api.handle_liblog, "__android_log_print");

    // 3. Resolve SELinux
    internal_api.selinux_setcon  = (func_setcon)load_sym_or_die(internal_api.handle_libselinux, "setcon");
    internal_api.selinux_getcon  = (func_getcon)load_sym_or_die(internal_api.handle_libselinux, "getcon");
    internal_api.selinux_freecon = (func_freecon)load_sym_or_die(internal_api.handle_libselinux, "freecon");

    // 4. Resolve Process Management (libc)
    internal_api.sys_setresuid = (func_setresuid)load_sym_or_die(internal_api.handle_libc, "setresuid");
    internal_api.sys_setresgid = (func_setresgid)load_sym_or_die(internal_api.handle_libc, "setresgid");
    internal_api.sys_prctl     = (func_prctl)load_sym_or_die(internal_api.handle_libc, "prctl");

    // 5. Resolve System Properties (libc)
    // Note: __system_property_get is a standard Bionic symbol
    internal_api.sys_prop_get  = (func_system_property_get)load_sym_or_die(internal_api.handle_libc, "__system_property_get");

    LOG_INFO("HubCore", "System Abstraction Layer Initialized successfully.");
    return 0;
}

void symbol_resolver_cleanup(void) {
    // Safety: Do NOT close libc, liblog, or libselinux.
    // Unloading core system libraries while the process is running is undefined behavior 
    
    // Nullify handles to prevent further access attempts via this API struct.
    internal_api.handle_libselinux = NULL;
    internal_api.handle_liblog     = NULL;
    internal_api.handle_libc       = NULL;
    
    LOG_INFO("HubCore", "System Abstraction Layer shutdown complete (Libraries preserved).");
}