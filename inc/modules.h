/**
 * @file modules.h
 * @brief Module Definitions and Registry.
 * * Declares the available modules, their IDs, and their isolation requirements.
 */

#ifndef MODULES_H
#define MODULES_H

#include <stdint.h>
#include <sys/types.h>
#include "symbol_resolver.h"

// ==============================================================================================
// REGISTRY ENUMS
// ==============================================================================================

/**
 * @brief Unique identifiers for each module.
 */
typedef enum {
    MOD_IMEI = 0,
    MOD_PHONE,
    MOD_NETWORK,
    MODULE_COUNT
} ModuleID;

// ==============================================================================================
// STRUCTURES
// ==============================================================================================

typedef enum {
    MODULE_TYPE_ONESHOT,    ///< Runs a task once and exits (e.g., Information Extraction).
    MODULE_TYPE_SERVICE     ///< Long-running daemon listening for requests (e.g., Network).
} ModuleType;

/**
 * @brief Static definition of a module's properties and security context.
 * Used by the Hub to spawn and isolate the process.
 */
typedef struct {
    ModuleID id;
    const char* name;
    ModuleType type;
    
    // --- Isolation Attributes ---
    uid_t target_uid;                   ///< User ID to drop to.
    gid_t target_gid;                   ///< Group ID to drop to.
    const char* target_selinux_context; ///< SELinux domain transition target.

    // --- Execution Entry Point ---
    void (*entrypoint)(int socket_fd);
} ModuleDef;

// ==============================================================================================
// PUBLIC REGISTRY
// ==============================================================================================

/**
 * @brief Global registry of all defined modules.
 * Defined in modules.c
 */
extern const ModuleDef MODULE_REGISTRY[MODULE_COUNT];

// ==============================================================================================
// PROTOTYPES
// ==============================================================================================

void mod_imei_entry(int socket_fd);
void mod_phone_entry(int socket_fd);
void mod_network_entry(int socket_fd);

#endif // MODULES_H