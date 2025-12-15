/**
 * @file modules.h
 * @brief Module Definitions and Registry
 */

#ifndef MODULES_H
#define MODULES_H

#include <stdint.h>
#include <sys/types.h>
#include "symbol_resolver.h"

// ==============================================================================================
// SECTION: Module Identification
// ==============================================================================================

typedef enum {
    MOD_IMEI = 0,
    MOD_PHONE,
    MOD_NETWORK,
    MODULE_COUNT
} ModuleID;

// ==============================================================================================
// SECTION: Types
// ==============================================================================================

typedef enum {
    MODULE_TYPE_ONESHOT,    ///< Runs a task and exits immediately.
    MODULE_TYPE_SERVICE     ///< Long-running daemon listening for requests.
} ModuleType;

typedef struct {
    ModuleID id;
    const char* name;
    ModuleType type;
    
    // --- Isolation Attributes ---
    uid_t target_uid;
    gid_t target_gid;
    const char* target_selinux_context;

    // --- Execution ---
    void (*entrypoint)(int socket_fd);
} ModuleDef;

// ==============================================================================================
// SECTION: Registry
// ==============================================================================================

extern const ModuleDef MODULE_REGISTRY[MODULE_COUNT];

// ==============================================================================================
// SECTION: Prototype Declarations
// ==============================================================================================

void mod_imei_entry(int socket_fd);
void mod_phone_entry(int socket_fd);
void mod_network_entry(int socket_fd);

#endif // MODULES_H