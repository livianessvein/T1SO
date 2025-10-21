// Livian Essvein 2211667
// Giovana Nogueira 2220372    
   
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

// PID do processo-filho responsável pelo timer (gera IRQ0)
static pid_t tmr_pid = -1;

// Handler de término: encerra o timer filho e finaliza o IC.
static void on_term(int s){
    (void)s;
    if(tmr_pid > 0) kill(tmr_pid, SIGTERM);
    _exit(0);
}

// argv[1] = fd_read (kernel->IC)
// argv[2] = kernel_pid
int main(int argc, char** argv){
    // Recebe descritor de leitura (pipe kernel->IC) e PID do kernel
    if(argc<3){
        fprintf(stderr, "Uso: %s <fd_read> <kernel_pid>\n", argv[0]);
        return 1;
    }
    int fd_r = atoi(argv[1]);
    pid_t kpid = (pid_t)atoi(argv[2]);

    signal(SIGTERM, on_term);

    // Cria processo-filho que gera interrupções de tempo (IRQ0) a cada 1s
    tmr_pid = fork();
    if(tmr_pid==0){
        // Timer de IRQ0 (1s)
        for(;;){
            sleep(1);
            kill(kpid, SIGUSR1); // IRQ0
        }
    }

    // Loop principal: aguarda mensagens do kernel no pipe.
    // Ao receber MSG_IO_START, espera 3s e envia IRQ1 (simula fim de I/O).
    // Caso contrário, dorme brevemente para evitar busy-wait.
    for(;;){
        icmsg_t m;
        ssize_t r = read(fd_r, &m, sizeof(m));
        if(r == sizeof(m) && m.msg_type == MSG_IO_START){
            sleep(3);
            kill(kpid, SIGUSR2); // IRQ1
        }else{
            usleep(10000);
        }
    }
    return 0;
}
