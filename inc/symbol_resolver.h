/**
 * @file symbol_resolver.h
 * @brief System Abstraction Layer (SAL).
 *
 * Provides wrappers for Android system libraries (liblog, libcutils, libselinux)
 * loaded dynamically at runtime via dlopen/dlsym. This removes build-time 
 * dependencies on the Android NDK specifics.
 */

#ifndef PROJECT_HUB_SAL_H
#define PROJECT_HUB_SAL_H

// --- Function Pointer Definitions ---
typedef int (*pfn_android_log_print)(int prio, const char* tag, const char* fmt, ...);
typedef int (*pfn_system_property_get)(const char* key, char* value);
typedef int (*pfn_setcon)(const char* context);

// --- Context ---
typedef struct {
    void* handle_liblog;
    void* handle_libc;
    void* handle_libselinux;

    pfn_android_log_print   log_print;
    pfn_system_property_get prop_get;
    pfn_setcon              set_con;
    
    int initialized;
} SalContext;

// --- Lifecycle ---
/**
 * @brief Initialize the HAL, load libraries and resolve symbols.
 * @return 0 on success.
 */
int sal_init(void);

/**
 * @brief Close library handles.
 */
void sal_cleanup(void);

// --- Wrappers ---
void sal_log_info(const char* fmt, ...);
void sal_log_error(const char* fmt, ...);
int sal_get_property(const char* key, char* value);
int sal_set_selinux_context(const char* context);

#endif // PROJECT_HUB_SAL_H


