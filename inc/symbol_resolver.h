#ifndef SYMBOL_RESOLVER_H
#define SYMBOL_RESOLVER_H

#include <sys/types.h>
#include <unistd.h>

/* Function Pointers for Runtime Loading */
typedef int  (*f_log_print)(int prio, const char* tag, const char* fmt, ...);
typedef int  (*f_setcon)(const char *ctx);
typedef int  (*f_setresuid)(uid_t r, uid_t e, uid_t s);
typedef int  (*f_setresgid)(gid_t r, gid_t e, gid_t s);
typedef int  (*f_prctl)(int opt, ...);
typedef int  (*f_prop_get)(const char* key, char* val);

typedef struct {
    f_log_print  log;
    f_setcon     setcon;
    f_setresuid  setresuid;
    f_setresgid  setresgid;
    f_prctl      prctl;
    f_prop_get   prop_get;
} sys_api_t;

extern const sys_api_t* sys;

int  sal_init(void);
void sal_cleanup(void);

#define LOG_I(tag, ...) if (sys && sys->log) sys->log(4, tag, __VA_ARGS__)
#define LOG_E(tag, ...) if (sys && sys->log) sys->log(6, tag, __VA_ARGS__)
#define LOG_F(tag, ...) if (sys && sys->log) sys->log(7, tag, __VA_ARGS__)

#endif