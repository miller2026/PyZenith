/**
 * @file sal.h
 * @brief System Abstraction Layer (SAL) Interface
 * * Provides a standardized interface for accessing Android system libraries
 * (libc, liblog, libselinux) dynamically at runtime. This ensures binary 
 * portability across different Android versions without linking against 
 * specific shared object versions at build time.
 */

#ifndef SAL_H
#define SAL_H

#include <sys/types.h>
#include <unistd.h>
#include <stdarg.h>

// ==============================================================================================
// SECTION: Function Pointer Definitions
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
// __system_property_get is part of Bionic libc
typedef int (*func_system_property_get)(const char* name, char* value);

// ==============================================================================================
// SECTION: API Structure
// ==============================================================================================

/**
 * @brief The Global System API Table.
 * Populated during initialization via dlopen/dlsym.
 */
typedef struct {
    // --- Library Handles ---
    void* handle_libc;
    void* handle_liblog;
    void* handle_libselinux;

    // --- Logging ---
    func_android_log_print log_print;

    // --- SELinux ---
    func_setcon            selinux_setcon;
    func_getcon            selinux_getcon;
    func_freecon           selinux_freecon;
    
    // --- System / Process ---
    func_setresuid         sys_setresuid;
    func_setresgid         sys_setresgid;
    func_prctl             sys_prctl;
    
    // --- Properties ---
    func_system_property_get sys_prop_get;

} SystemAPI;

// Global read-only pointer to the API table.
extern const SystemAPI* g_api;

// ==============================================================================================
// SECTION: Lifecycle Functions
// ==============================================================================================

/**
 * @brief Initializes the System Abstraction Layer.
 * Loads required shared libraries and resolves symbols.
 * @return 0 on success, non-zero on fatal error (logs to stderr).
 */
int sal_init(void);

/**
 * @brief Cleans up SAL resources.
 * Closes dlopen handles.
 */
void sal_cleanup(void);

// ==============================================================================================
// SECTION: Logging Macros
// ==============================================================================================

// Priority Constants for __android_log_print
#define ANDROID_LOG_INFO  4
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7

#define LOG_INFO(tag, ...)  if (g_api && g_api->log_print) g_api->log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) if (g_api && g_api->log_print) g_api->log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
#define LOG_FATAL(tag, ...) if (g_api && g_api->log_print) g_api->log_print(ANDROID_LOG_FATAL, tag, __VA_ARGS__)

#endif // SAL_H
