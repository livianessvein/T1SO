#define _GNU_SOURCE
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>

/* ====== Visual ====== */
#define C_RST "\x1b[0m"
#define C_IRQ "\x1b[36m"
#define C_SCH "\x1b[33m"
#define C_IO  "\x1b[35m"
#define C_APP "\x1b[32m"
#define C_ERR "\x1b[31m"

/* ====== Estado global ====== */
static pcb_t procs[MAX_APPS];
static int nprocs = 0;

/* pipes */
static int fd_app_r = -1, fd_app_w = -1; // app->kernel (kernel lê r; apps escrevem w)
static int fd_ic_r  = -1, fd_ic_w  = -1; // kernel->IC   (IC lê r; kernel escreve w)
static pid_t ic_pid = -1;

/* flags de sinal */
static volatile sig_atomic_t got_irq0 = 0; // timeslice
static volatile sig_atomic_t got_irq1 = 0; // I/O terminado
static volatile sig_atomic_t got_sysc = 0; // notificação para drenar pipe

/* ====== Fila de prontos ====== */
static pid_t rq[MAX_APPS];
static int rq_head = 0, rq_tail = 0, rq_count = 0;

static pid_t current = -1;

/* Fila de bloqueados por I/O e quem está em serviço */
static pid_t io_q[MAX_APPS];
static int io_head = 0, io_tail = 0, io_count = 0;
static int io_busy = 0;
static pid_t io_serving = -1;

/* Contagem de finalizados para critério de parada */
static int finished_count = 0;

/* Anti-stall (quando só há 1 pronto e ele “não anda”) */
static int stall_ticks = 0;        // quantos IRQ0 seguidos sem progresso do current
static int last_progress_pc = -1;  // último PC observado do current

/* Tempo base para logs */
static time_t t0;

/* ==== PROTÓTIPOS (evita "implicit declaration" no Clang) ==== */
static void rq_push(pid_t p);
static int  rq_pop(pid_t *p);
static void io_push(pid_t p);
static int  io_pop(pid_t *p);

/* ====== Helpers ====== */
static void log_ts_prefix(void)
{
    time_t now = time(NULL);
    printf("[%3lds] ", (long)(now - t0));
    fflush(stdout);
}
static const char *name_of(pid_t pid)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].pid == pid)
            return procs[i].name;
    return "?";
}
static pcb_t *bypid(pid_t pid)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].pid == pid)
            return &procs[i];
    return NULL;
}
static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* === Helpers de vida e limpeza de filas === */
static int is_alive(pid_t pid) {
    return (kill(pid, 0) == 0);
}

/* fila de prontos */
static void rq_push(pid_t p)
{
    pcb_t *pp = bypid(p);
    if (pp && pp->st == ST_FINISHED) return; // não enfileira finalizado
    if (rq_count >= MAX_APPS) return;
    rq[rq_tail] = p;
    rq_tail = (rq_tail + 1) % MAX_APPS;
    rq_count++;
}
static int rq_pop(pid_t *p)
{
    if (rq_count == 0) return 0;
    *p = rq[rq_head];
    rq_head = (rq_head + 1) % MAX_APPS;
    rq_count--;
    return 1;
}

/* fila de I/O */
static void io_push(pid_t p)
{
    if (io_count >= MAX_APPS) return;
    io_q[io_tail] = p;
    io_tail = (io_tail + 1) % MAX_APPS;
    io_count++;
}
static int io_pop(pid_t *p)
{
    if (io_count == 0) return 0;
    *p = io_q[io_head];
    io_head = (io_head + 1) % MAX_APPS;
    io_count--;
    return 1;
}

/* limpeza de filas (usa protótipos acima) */
static void rq_remove_pid(pid_t pid) {
    int n = rq_count;
    for (int i = 0; i < n; i++) {
        pid_t p;
        if (!rq_pop(&p)) break;
        if (p != pid) rq_push(p);
    }
}
static void io_remove_pid(pid_t pid) {
    int n = io_count;
    for (int i = 0; i < n; i++) {
        pid_t p;
        if (!io_pop(&p)) break;
        if (p != pid) io_push(p);
    }
    if (io_serving == pid) {
        io_serving = -1;
        io_busy = 0;
    }
}

/* ====== Sinais ====== */
static void on_irq0(int s){ (void)s; got_irq0 = 1; }
static void on_irq1(int s){ (void)s; got_irq1 = 1; }
static void on_sysc(int s){ (void)s; got_sysc = 1; }

/* ====== Escalonamento ====== */
static void dispatch_next()
{
    if (current != -1) return;

    pid_t nx;
    while (rq_pop(&nx)) {
        pcb_t *p = bypid(nx);
        if (!p || p->st == ST_FINISHED || !is_alive(nx)) {
            continue; // pula PIDs mortos/finalizados
        }

        current = nx;
        p->st = ST_RUNNING;
        last_progress_pc = p->last_pc;
        stall_ticks = 0;

        log_ts_prefix();
        printf(C_SCH "DISPATCH  -> %-3s (pid=%d) [restore PC=%d, RW=%s]" C_RST "\n",
               name_of(nx), (int)nx,
               p->last_pc,
               (p->last_syscall != -1) ? (p->last_syscall ? "W" : "R") : "-");

        kill(nx, SIGCONT);
        return;
    }

    /* fila vazia */
    static time_t last_print = 0;
    time_t now = time(NULL);
    if (now != last_print) {
        last_print = now;
        log_ts_prefix();
        printf(C_SCH "DISPATCH  (fila vazia) — aguardando próximo evento" C_RST "\n");
    }
}

static void preempt_current()
{
    if (current == -1) return;
    if (is_alive(current)) kill(current, SIGSTOP);
    pcb_t *p = bypid(current);
    if (p && p->st == ST_RUNNING) {
        p->st = ST_READY;
        rq_push(current);
    }
    current = -1;
    stall_ticks = 0; // vai recomeçar em outro processo
    log_ts_prefix();
    printf(C_SCH "PREEMPT   <- %-3s (volta à fila de prontos)" C_RST "\n", p ? p->name : "?");
}

/* Inicia serviço de I/O se o dispositivo está livre */
static void start_io_if_idle()
{
    if (io_busy) return;
    pid_t p;
    if (!io_pop(&p)) return;

    io_busy = 1;
    io_serving = p;

    /* avisa o InterController: começa cronômetro de 3s */
    icmsg_t m = {.msg_type = MSG_IO_START};
    (void)write(fd_ic_w, &m, sizeof(m));

    log_ts_prefix();
    printf(C_IO "IO-START  >> %-3s (pid=%d) — D1 ocupado" C_RST "\n", name_of(p), (int)p);
}

/* ====== Comunicação com apps ====== */
static void handle_app_pipe()
{
    for (;;) {
        appmsg_t m;
        ssize_t r = read(fd_app_r, &m, sizeof(m));
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("read app pipe");
            break;
        }
        if (r == 0) break;
        if (r != (ssize_t)sizeof(m)) continue;

        pcb_t *p = bypid(m.pid);
        if (!p) continue;

        if (m.msg_type == MSG_SYSCALL_RW) {
            /* salva no contexto o parâmetro da syscall (R/W) */
            p->last_syscall = (m.arg ? 1 : 0);

            log_ts_prefix();
            printf(C_IO "SYSCALL   !! %-3s pede I/O (%s)" C_RST "\n",
                   name_of(m.pid), m.arg ? "WRITE" : "READ");

            if (p->st == ST_RUNNING) {
                kill(p->pid, SIGSTOP);
                p->st = ST_BLOCKED;
                if (current == p->pid) current = -1;
                log_ts_prefix();
                printf(C_IO "BLOCK     .. %-3s bloqueado por I/O [ctx: PC=%d, RW=%s]" C_RST "\n",
                       name_of(p->pid), p->last_pc, p->last_syscall ? "W" : "R");
            } else {
                p->st = ST_BLOCKED;
            }
            io_push(p->pid);
            start_io_if_idle();
        }
        else if (m.msg_type == MSG_APP_STATUS) {
            p->last_pc = m.arg;   // mantém PC atualizado no contexto
            if (current == p->pid) {
                /* progresso do processo corrente: zera stall */
                last_progress_pc = p->last_pc;
                stall_ticks = 0;
            }
            log_ts_prefix();
            printf(C_APP "PC        :: %-3s -> %d" C_RST "\n", p->name, p->last_pc);
        }
    }
}

/* ====== Reaper ====== */
static void on_child_exit()
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pcb_t *p = bypid(pid);
        if (p && p->st != ST_FINISHED) {
            p->st = ST_FINISHED;
            finished_count++;

            if (current == pid) current = -1;

            /* remova de todas as filas para não despachar de novo */
            rq_remove_pid(pid);
            io_remove_pid(pid);

            log_ts_prefix();
            printf(C_APP "FINISHED  xx %-3s (pid=%d)" C_RST "\n", p->name, (int)pid);
        }
    }
}

/* ====== Critério de parada ====== */
static int all_done(void)
{
    return (finished_count == nprocs) && (rq_count == 0) && (current == -1)
           && (io_busy == 0) && (io_count == 0);
}

/* ====== Loop principal ====== */
static void schedule_loop()
{
    for (;;) {
        handle_app_pipe();

        if (got_irq1) {
            got_irq1 = 0;
            log_ts_prefix();
            printf(C_IRQ "IRQ1      ** D1 sinaliza término de I/O" C_RST "\n");

            io_busy = 0;
            if (io_serving != -1) {
                pcb_t *p = bypid(io_serving);
                if (p && p->st == ST_BLOCKED) {
                    p->st = ST_READY;
                    rq_push(p->pid);
                    log_ts_prefix();
                    printf(C_IO "IO-DONE   << %-3s liberado; volta à fila de prontos" C_RST "\n", p->name);
                }
                io_serving = -1;
            }
            start_io_if_idle();
            dispatch_next();
        }

        if (got_irq0) {
            got_irq0 = 0;

            if (current != -1 && rq_count == 0) {
                /* Único pronto: não preempta — MAS reforça CONT e vigia stall */
                log_ts_prefix();
                printf(C_IRQ "IRQ0      ** time-slice encerrado — único pronto continua" C_RST "\n");

                /* 1) Reforço: se ficou parado em SIGSTOP por corrida, acorda */
                kill(current, SIGCONT);

                /* 2) Watchdog: se não há progresso de PC, conta stall */
                pcb_t *cp = bypid(current);
                if (cp) {
                    if (cp->last_pc == last_progress_pc) {
                        stall_ticks++;
                    } else {
                        last_progress_pc = cp->last_pc;
                        stall_ticks = 0;
                    }
                }

                /* 3) Se 5 ticks sem progresso, “nudge”: STOP -> fila -> DISPATCH */
                if (stall_ticks >= 5 && cp) {
                    log_ts_prefix();
                    printf(C_ERR "NUDGE     !! sem progresso (%d ticks) — reativando %s" C_RST "\n",
                           stall_ticks, name_of(current));
                    if (is_alive(current)) kill(current, SIGSTOP);
                    cp->st = ST_READY;
                    rq_push(current);
                    current = -1;
                    stall_ticks = 0;
                    dispatch_next();
                }
            } else {
                /* Há 2+ prontos: preempta normalmente */
                log_ts_prefix();
                printf(C_IRQ "IRQ0      ** time-slice encerrado" C_RST "\n");
                preempt_current();
                dispatch_next();
            }
        }

        if (got_sysc) {
            got_sysc = 0;
            if (current == -1) dispatch_next();
        }

        on_child_exit();

        if (all_done()) {
            log_ts_prefix();
            printf(C_SCH "ALL DONE  ✅ todos os apps finalizaram; encerrando Kernel e IC" C_RST "\n");
            if (ic_pid > 0) kill(ic_pid, SIGTERM);
            if (ic_pid > 0) waitpid(ic_pid, NULL, 0);
            break;
        }

        if (current == -1) dispatch_next();

        usleep(10000);
    }
}

/* ====== Main ====== */
static void usage(const char *argv0)
{
    fprintf(stderr, "Uso: %s <num_apps (3..6)>\n", argv0);
    exit(1);
}

int main(int argc, char **argv)
{
    t0 = time(NULL);
    setvbuf(stdout, NULL, _IOLBF, 0); // flush por linha (macOS)

    if (argc < 2) usage(argv[0]);

    nprocs = atoi(argv[1]);
    if (nprocs < 3 || nprocs > 6) {
        fprintf(stderr, C_ERR "Erro: número de apps precisa estar entre 3 e 6 (conforme enunciado)." C_RST "\n");
        usage(argv[0]);
    }

    /* pipes app->kernel */
    int p_app[2];
    if (pipe(p_app) < 0) { perror("pipe app"); return 1; }
    fd_app_r = p_app[0];
    fd_app_w = p_app[1];
    set_nonblock(fd_app_r);

    /* pipes kernel->IC */
    int p_ic[2];
    if (pipe(p_ic) < 0) { perror("pipe ic"); return 1; }
    fd_ic_r = p_ic[0];
    fd_ic_w = p_ic[1];

    /* Fork InterController */
    ic_pid = fork();
    if (ic_pid == 0)
    {
        close(fd_app_r);
        close(fd_app_w);
        close(fd_ic_w); /* IC só lê */
        char fd_read_str[32], kpid[32];
        snprintf(fd_read_str, sizeof(fd_read_str), "%d", fd_ic_r);
        snprintf(kpid, sizeof(kpid), "%d", getppid());
        execl("./inter_controller", "./inter_controller", fd_read_str, kpid, (char *)NULL);
        perror("exec inter_controller");
        _exit(1);
    }
    close(fd_ic_r); // kernel não lê do IC

    /* Handlers */
    struct sigaction sa = {0};
    sa.sa_handler = on_irq0;  sigaction(SIGUSR1, &sa, NULL); // IRQ0
    sa.sa_handler = on_irq1;  sigaction(SIGUSR2, &sa, NULL); // IRQ1
    sa.sa_handler = on_sysc;  sigaction(SIGALRM, &sa, NULL); // “acorda kernel”
    signal(SIGCHLD, (void (*)(int))on_child_exit);

    /* Fork apps A1..An */
    log_ts_prefix();
    printf(C_SCH "BOOT      ~~ KernelSim iniciando (%d apps)" C_RST "\n", nprocs);
    for (int i = 0; i < nprocs; i++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            close(fd_app_r); /* app não lê */
            close(fd_ic_r);
            close(fd_ic_w);
            char fdw[32], name[32], idx[8], kpid[32];
            snprintf(fdw, sizeof(fdw), "%d", fd_app_w);
            snprintf(idx, sizeof(idx), "%d", i + 1);
            snprintf(name, sizeof(name), "A%d", i + 1);
            snprintf(kpid, sizeof(kpid), "%d", getppid());
            execl("./app", "./app", fdw, name, idx, kpid, (char *)NULL);
            perror("exec app");
            _exit(1);
        }
        else if (pid > 0)
        {
            snprintf(procs[i].name, sizeof(procs[i].name), "A%d", i + 1);
            procs[i].pid = pid;
            procs[i].st = ST_READY;
            procs[i].last_pc = 0;
            procs[i].last_syscall = -1;   /* parâmetro de syscall salvo no contexto */
            rq_push(pid);

            log_ts_prefix();
            printf(C_APP "SPAWN     ++ %-3s (pid=%d) adicionado à fila de prontos" C_RST "\n",
                   procs[i].name, (int)pid);
        }
        else
        {
            perror("fork app");
            return 1;
        }
        // (sem sleep para reduzir janelas de corrida no boot)
    }

    /* Pausa todos; kernel decide quem roda */
    for (int i = 0; i < nprocs; i++)
        kill(procs[i].pid, SIGSTOP);
    current = -1;

    dispatch_next();
    schedule_loop();

    log_ts_prefix();
    printf(C_SCH "SHUTDOWN  ~~ Kernel encerrado\n" C_RST);
    return 0;
}
