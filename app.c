// ================================================
// app.c — Simula um processo de usuário (aplicativo)
// ================================================
// Este código representa um “app” controlado pelo kernel_sim.
// Ele envia periodicamente seu PC (program counter) ao kernel,
// realiza syscalls de leitura/escrita em pontos definidos e dorme 1s por iteração.

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>

// Define descritor de escrita para o pipe app->kernel
// e outras variáveis de identificação do processo
static int fd_kernel = -1;           // Descritor de arquivo para escrever no kernel
static char me_name[MAX_NAME];       // Nome do processo/app
static int idx = 0;                  // Índice identificador do processo (1..4)
static pid_t kernel_pid = -1;        // PID do processo kernel para envio de sinais

// Envia uma syscall de leitura ou escrita ao kernel e se auto-suspende (SIGSTOP)
// até que o kernel o retome após tratar a requisição.
static void do_syscall_rw(int rw_flag){
    appmsg_t m = { .msg_type = MSG_SYSCALL_RW, .pid = getpid(), .arg = rw_flag };
    write(fd_kernel, &m, sizeof(m));
    // Kernel é quem efetivamente para, mas faremos STOP voluntário para reduzir corrida:
    raise(SIGSTOP);
}

// Reporta ao kernel o PC atual (estado de execução)
// e envia SIGALRM para garantir leitura imediata do pipe.
static void send_status(int pc){
    appmsg_t m = { .msg_type = MSG_APP_STATUS, .pid = getpid(), .arg = pc };
    write(fd_kernel, &m, sizeof(m));
    // acorda o kernel para drenar pipe (antes usávamos SIGRTMIN+1)
    kill(kernel_pid, SIGALRM);
}

int main(int argc, char** argv){
    // Suspende o processo logo ao iniciar — o kernel fará o primeiro SIGCONT (DISPATCH)
    // garantindo sincronização entre kernel e app.
    raise(SIGSTOP);

    // Converte argumentos e inicializa variáveis básicas
    if(argc<5){
        fprintf(stderr,"Uso: %s <fd_kernel_write> <nome> <idx> <kernel_pid>\n", argv[0]);
        return 1;
    }
    fd_kernel = atoi(argv[1]);
    strncpy(me_name, argv[2], sizeof(me_name)-1);
    me_name[sizeof(me_name)-1] = '\0';
    idx = atoi(argv[3]);
    kernel_pid = (pid_t)atoi(argv[4]);

    // Define pontos específicos de I/O de acordo com o índice do processo
    int io_points[5]={0};
    int io_n=0;
    if(idx==1){ int tmp[]={3,7,12}; memcpy(io_points,tmp,sizeof(int)*3); io_n=3; }
    else if(idx==2){ int tmp[]={4,9}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }
    else if(idx==3){ int tmp[]={5,10}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }
    else { int tmp[]={6,11}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }

    const int MAX = 15;

    // Loop principal: incrementa o PC, envia STATUS, verifica se há I/O e dorme 1s
    for (int pc = 1; pc <= MAX; ++pc) {
        send_status(pc);                // 1) reporta imediatamente

        // 2) se este PC tem I/O, pede e se bloqueia; quando voltar, segue
        for (int k = 0; k < io_n; ++k) {
            if (pc == io_points[k]) {
                do_syscall_rw((pc % 2) ? 0 : 1); // ímpar=READ, par=WRITE
                break;
            }
        }

        sleep(1);                       // 3) consome 1s de CPU
    }
    return 0;
}
