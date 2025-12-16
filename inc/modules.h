/**
 * @file modules.h
 * @brief Registry and definitions for isolated task modules.
 */

#ifndef PROJECT_HUB_MODULES_H
#define PROJECT_HUB_MODULES_H

// --- Module IDs ---
#define MOD_ID_IMEI     0
#define MOD_ID_PHONE    1
#define MOD_ID_MAC      2
#define MOD_ID_LOGGER   3
#define MOD_ID_SENDER   4

/**
 * @brief Standard function signature for all modules.
 * @param socket_fd Unix Domain Socket (Write-only for results).
 * @param input_arg Optional input string from Orchestrator.
 */
typedef void (*module_entry_fn)(int socket_fd, const char* input_arg);

/**
 * @brief Retrieve the entry point for a specific module ID.
 */
module_entry_fn get_module_entry(int module_id);

// --- Specific Implementations ---
void mod_imei_entry(int socket_fd, const char* input_arg);
void mod_phone_entry(int socket_fd, const char* input_arg);
void mod_mac_entry(int socket_fd, const char* input_arg);
void mod_logger_entry(int socket_fd, const char* input_arg);
void mod_sender_entry(int socket_fd, const char* input_arg);

#endif // PROJECT_HUB_MODULES_H


