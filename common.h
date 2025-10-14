#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <sys/types.h>

/* Limites */
#define MAX_APPS 6
#define MAX_NAME 16

/* Sinais usados:
   - SIGUSR1 -> IRQ0 (time-slice a cada 1s)
   - SIGUSR2 -> IRQ1 (fim de I/O 3s após cada pedido)
   - SIGALRM -> “acorda kernel” para drenar pipe app->kernel
*/

/* Tipos de mensagens app->kernel */
enum { MSG_SYSCALL_RW = 1, MSG_APP_STATUS = 2, MSG_IO_START = 3 };

/* app -> kernel */
typedef struct {
    int   msg_type;   // MSG_SYSCALL_RW ou MSG_APP_STATUS
    pid_t pid;        // PID do app remetente
    int   arg;        // SYSCALL: 0=READ,1=WRITE | STATUS: PC atual
} appmsg_t;

/* kernel -> inter_controller */
typedef struct {
    int msg_type; // sempre MSG_IO_START
} icmsg_t;

typedef enum { ST_READY=0, ST_RUNNING=1, ST_BLOCKED=2, ST_FINISHED=3 } pstate_t;

/* PCB do Kernel (estado em “memória” do processo) */
typedef struct {
    pid_t pid;
    char  name[MAX_NAME];
    pstate_t st;
    int   last_pc;       // último PC informado pelo app (contexto salvo)
    int   last_syscall;  // 0=READ, 1=WRITE, -1=nenhum (parâmetro da última syscall)
} pcb_t;

#endif
