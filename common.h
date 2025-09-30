#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <sys/types.h>

#define MAX_APPS 6
#define MAX_NAME 16

// Sinais usados:
// SIGUSR1 -> IRQ0 (fim de time-slice, 1s)
// SIGUSR2 -> IRQ1 (término de I/O D1, 3s após início)
// SIGALRM -> notificação de "syscall" app->kernel para drenar o pipe

enum { MSG_SYSCALL_RW = 1, MSG_APP_STATUS = 2, MSG_IO_START = 3 };

typedef struct {
    int   msg_type;     // MSG_SYSCALL_RW ou MSG_APP_STATUS
    pid_t pid;          // PID do app remetente
    int   arg;          // para SYSCALL: 0=READ,1=WRITE | para STATUS: PC atual
} appmsg_t;

typedef struct {
    int msg_type; // sempre MSG_IO_START
} icmsg_t;

typedef enum { ST_READY=0, ST_RUNNING=1, ST_BLOCKED=2, ST_FINISHED=3 } pstate_t;

typedef struct {
    pid_t pid;
    char  name[MAX_NAME];
    pstate_t st;
    int   last_pc;    // último PC informado pelo app
    int   wants_io;   // flag momentânea
} pcb_t;

#endif
