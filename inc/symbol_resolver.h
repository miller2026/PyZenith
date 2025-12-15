/**
 * @file symbol_resolver.h
 * @brief System Abstraction Layer (Interface).
 * * Provides function pointers to Android system libraries (libc, liblog, libselinux).
 * This allows the binary to run without build-time linking to unstable Android ABIs.
 */

#ifndef SYMBOL_RESOLVER_H
#define SYMBOL_RESOLVER_H

#include <sys/types.h>
#include <unistd.h>
#include <stdarg.h>

// ==============================================================================================
// FUNCTION POINTER TYPES
// ==============================================================================================

// --- Android Logging (liblog) ---
typedef int (*func_android_log_print)(int prio, const char* tag, const char* fmt, ...);

// --- SELinux (libselinux) ---
typedef int (*func_setcon)(const char *context);
typedef int (*func_getcon)(char **context);
typedef void (*func_freecon)(char *context);

// --- Process & Credential Management (libc) ---
typedef int (*func_setresuid)(uid_t ruid, uid_t euid, uid_t suid);
typedef int (*func_setresgid)(gid_t rgid, gid_t egid, gid_t sgid);
typedef int (*func_prctl)(int option, ...);

// --- System Properties (libc) ---
typedef int (*func_system_property_get)(const char* name, char* value);

// ==============================================================================================
// API STRUCTURE
// ==============================================================================================

/**
 * @brief The Global System API Table.
 * Populated during initialization via dlopen/dlsym.
 */
typedef struct {
    // Library Handles (Opaque)
    void* handle_libc;
    void* handle_liblog;
    void* handle_libselinux;

    // API Functions
    func_android_log_print      log_print;
    func_setcon                 selinux_setcon;
    func_getcon                 selinux_getcon;
    func_freecon                selinux_freecon;
    func_setresuid              sys_setresuid;
    func_setresgid              sys_setresgid;
    func_prctl                  sys_prctl;
    func_system_property_get    sys_prop_get;

} SystemAPI;

// Global read-only pointer to the API table.
extern const SystemAPI* g_api;

// ==============================================================================================
// LIFECYCLE
// ==============================================================================================

/**
 * @brief Initializes the System Abstraction Layer.
 * Loads libraries and resolves symbols.
 * @return 0 on success, exit(1) on failure.
 */
int symbol_resolver_init(void);

/**
 * @brief Cleans up SYMBOL_RESOLVER resources.
 */
void symbol_resolver_cleanup(void);

// ==============================================================================================
// LOGGING MACROS
// ==============================================================================================

#define ANDROID_LOG_INFO  4
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7

// Safe logging macros that check for API existence before calling
#define LOG_INFO(tag, ...)  if (g_api && g_api->log_print) g_api->log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) if (g_api && g_api->log_print) g_api->log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
#define LOG_FATAL(tag, ...) if (g_api && g_api->log_print) g_api->log_print(ANDROID_LOG_FATAL, tag, __VA_ARGS__)

#endif // SYMBOL_RESOLVER_H