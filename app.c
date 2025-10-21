#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>

static int fd_kernel = -1;
static char me_name[MAX_NAME];
static int idx = 0;
static pid_t kernel_pid = -1;

static void do_syscall_rw(int rw_flag){
    appmsg_t m = { .msg_type = MSG_SYSCALL_RW, .pid = getpid(), .arg = rw_flag };
    write(fd_kernel, &m, sizeof(m));
    // Kernel é quem efetivamente para, mas faremos STOP voluntário para reduzir corrida:
    raise(SIGSTOP);
}

static void send_status(int pc){
    appmsg_t m = { .msg_type = MSG_APP_STATUS, .pid = getpid(), .arg = pc };
    write(fd_kernel, &m, sizeof(m));
    // acorda o kernel para drenar pipe (antes usávamos SIGRTMIN+1)
    kill(kernel_pid, SIGALRM);
}

int main(int argc, char** argv){
    if(argc<5){
        fprintf(stderr,"Uso: %s <fd_kernel_write> <nome> <idx> <kernel_pid>\n", argv[0]);
        return 1;
    }
    fd_kernel = atoi(argv[1]);
    strncpy(me_name, argv[2], sizeof(me_name)-1);
    idx = atoi(argv[3]);
    kernel_pid = (pid_t)atoi(argv[4]);

    // Perfil de teste: "cpu" (sem I/O), "io" (todos com I/O) ou "split" (A1..A3 sem I/O; A4..A6 com I/O)
    enum { PROFILE_SPLIT, PROFILE_CPU, PROFILE_IO } profile_mode = PROFILE_SPLIT;
    const char *env_profile = getenv("APP_PROFILE");
    if(env_profile){
        if(strcmp(env_profile, "cpu")==0) profile_mode = PROFILE_CPU;
        else if(strcmp(env_profile, "io")==0) profile_mode = PROFILE_IO;
        else if(strcmp(env_profile, "split")==0) profile_mode = PROFILE_SPLIT;
    }

    int io_points[5]={0};
    int io_n=0;

    if(profile_mode == PROFILE_CPU){
        // Nenhum processo faz I/O neste perfil
        io_n = 0;
    } else if(profile_mode == PROFILE_IO){
        // Todos os processos fazem I/O segundo seus índices
        if(idx==1){ int tmp[]={3,7,12}; memcpy(io_points,tmp,sizeof(int)*3); io_n=3; }
        else if(idx==2){ int tmp[]={4,9}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }
        else if(idx==3){ int tmp[]={5,10}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }
        else { int tmp[]={6,11}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }
    } else { // PROFILE_SPLIT (padrão): A1..A3 sem I/O; A4..A6 com I/O
        if(idx <= 3){
            io_n = 0; // A1, A2, A3 CPU-bound
        } else {
            // A4, A5, A6 com pontos de I/O por índice, para escalonar e manter o D1 sempre serial.
            // Mantém a mesma lógica de quantum (um PC por rodada) – só dispara SYSCALL nos PCs abaixo.
            if(idx==4){ int tmp[]={3,7,12}; memcpy(io_points,tmp,sizeof(int)*3); io_n=3; }
            else if(idx==5){ int tmp[]={6,11}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }
            else /* idx==6 */{ int tmp[]={5,10}; memcpy(io_points,tmp,sizeof(int)*2); io_n=2; }
        }
    }

    const int MAX = 15;

    for(int pc=1; pc<=MAX; ++pc){
        send_status(pc);
        sleep(1);

        for(int k=0;k<io_n;k++){
            if(pc == io_points[k]){
                do_syscall_rw((pc%2)==0 ? 1 : 0); // 0=READ,1=WRITE
                break;
            }
        }
    }
    return 0;
}
