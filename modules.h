/**
 * @file modules.h
 * @brief Module Definitions and Registry
 * * Defines the contract for child modules and the static registry 
 * that the Hub uses to spawn them.
 */

#ifndef MODULES_H
#define MODULES_H

#include <stdint.h>
#include <sys/types.h>
#include "sal.h"

// ==============================================================================================
// SECTION: Module Identification
// ==============================================================================================

/**
 * @brief Unique identifiers for all available modules.
 * Used for indexing and logic switching.
 */
typedef enum {
    MOD_ECHO = 0,
    MOD_LONG_TASK,
    MOD_CRASHER,
    MOD_PHONE_READER,
    MOD_PROP_READER, // New Module ID
    MODULE_COUNT // Automatic count of modules
} ModuleID;

// ==============================================================================================
// SECTION: Types
// ==============================================================================================

/**
 * @brief Classification of module behavior.
 */
typedef enum {
    MODULE_TYPE_ONESHOT,    ///< Runs a task and exits immediately.
    MODULE_TYPE_SERVICE     ///< Long-running daemon listening for requests.
} ModuleType;

/**
 * @brief Defines the configuration and entrypoint for a child process module.
 */
typedef struct {
    ModuleID id;
    const char* name;
    ModuleType type;
    
    // --- Isolation Attributes ---
    uid_t target_uid;
    gid_t target_gid;
    const char* target_selinux_context;

    // --- Execution ---
    /**
     * @brief The main entry point for the module logic.
     * @param socket_fd The connected Unix Domain Socket (DGRAM) to the Hub.
     */
    void (*entrypoint)(int socket_fd);
} ModuleDef;

// ==============================================================================================
// SECTION: Registry
// ==============================================================================================

/**
 * @brief Global registry of all available modules.
 */
extern const ModuleDef MODULE_REGISTRY[MODULE_COUNT];

// ==============================================================================================
// SECTION: Prototype Declarations
// ==============================================================================================

void mod_echo_entry(int socket_fd);
void mod_long_task_entry(int socket_fd);
void mod_crasher_entry(int socket_fd);
void mod_phone_reader_entry(int socket_fd);
void mod_prop_reader_entry(int socket_fd);

#endif // MODULES_H
