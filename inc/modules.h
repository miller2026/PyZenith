#ifndef MODULES_H
#define MODULES_H

#include <stdint.h>
#include <sys/types.h>

typedef enum {
    MOD_IMEI = 0,
    MOD_PHONE,
    MOD_NET,
    MOD_COUNT
} mod_id_t;

typedef enum {
    TYPE_ONESHOT,
    TYPE_SERVICE
} mod_type_t;

typedef struct {
    mod_id_t    id;
    const char* name;
    mod_type_t  type;
    uid_t       uid;
    gid_t       gid;
    const char* se_ctx;
    void      (*run)(int fd);
} mod_def_t;

extern const mod_def_t MODULES[MOD_COUNT];

#endif