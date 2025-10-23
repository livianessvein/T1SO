#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

// ===== Cores p/ logs (opcionais) =====
#define C_RST "\x1b[0m"
#define C_IRQ "\x1b[36m"
#define C_SCH "\x1b[33m"
#define C_IO  "\x1b[35m"
#define C_APP "\x1b[32m"
#define C_ERR "\x1b[31m"

// ===== Config geral =====
#define PC_MAX      15
#define QUANTUM     1   // 1 tick (dormimos 1s por tick)
#define IO_LAT      3   // latência do dispositivo em ticks
#define MAX_APPS    64

typedef enum { NEW, READY, RUNNING, BLOCKED, FINISHED } state_t;

typedef struct Proc {
    int     pid;
    char    name[8];
    int     pc;
    state_t state;
    char    rw;          // 'R', 'W' ou '-'
    int     uses_io;     // 0 = cpu-only, 1 = usa IO
} Proc;

typedef struct Node {
    Proc *p;
    struct Node *next;
} Node;

static Node *ready_q = NULL;
static Node *io_wait_q = NULL;

static Proc  procs[MAX_APPS];
static int   nprocs = 0;
static Proc *current = NULL;

// Dispositivo
static int    dev_busy = 0;
static Proc  *dev_owner = NULL;
static int    next_irq1_at = -1;

// tempo simulado (em ticks)
static int now_tick = 0;

// ===== Utils de fila =====
static void q_push(Node **q, Proc *p) {
    Node *n = (Node*)malloc(sizeof(Node));
    n->p = p; n->next = NULL;
    if (!*q) { *q = n; return; }
    Node *t = *q;
    while (t->next) t = t->next;
    t->next = n;
}
static Proc* q_pop(Node **q) {
    if (!*q) return NULL;
    Node *n = *q;
    *q = n->next;
    Proc *p = n->p;
    free(n);
    return p;
}
static int q_len(Node *q) {
    int c=0;
    for(Node *t=q;t;t=t->next) c++;
    return c;
}

// ===== Logging =====
static void ts() { printf("[%3ds] ", now_tick); }

static void log_boot(int apps) {
    ts(); printf("BOOT      " C_SCH "~~" C_RST " KernelSim iniciando (%d apps)\n", apps);
}
static void log_spawn(Proc *p) {
    ts(); printf("SPAWN     " C_APP "++ %s" C_RST "  (pid=%d) adicionado à fila de prontos\n", p->name, p->pid);
}
static void log_dispatch(Proc *p, int restore) {
    if (!p) {
        ts(); printf("DISPATCH  (fila vazia) — aguardando próximo evento\n");
    } else {
        ts(); printf("DISPATCH  " C_SCH "-> %s" C_RST "  (pid=%d) [restore PC=%d, RW=%c]\n",
                     p->name, p->pid, restore, p->rw ? p->rw : '-');
    }
}
static void log_pc(Proc *p, int newpc) {
    ts(); printf("PC        " C_APP ":: %s  -> %d" C_RST "\n", p->name, newpc);
}
static void log_irq0(int unico) {
    ts(); printf(C_IRQ "IRQ0      " C_RST "** time-slice encerrado");
    if (unico) printf(" — único pronto continua");
    printf("\n");
}
static void log_preempt(Proc *p) {
    ts(); printf("PREEMPT   " C_SCH "<- %s" C_RST "  (volta à fila de prontos)\n", p->name);
}
static void log_syscall(Proc *p, const char *op) {
    ts(); printf("SYSCALL   " C_IO "!! %s" C_RST "  pede I/O (%s)\n", p->name, op);
}
static void log_block(Proc *p) {
    ts(); printf("BLOCK     .. %s  bloqueado por I/O [ctx: PC=%d, RW=%c]\n", p->name, p->pc, p->rw);
}
static void log_iostart(Proc *p) {
    ts(); printf("IO-START  " C_IO ">> %s" C_RST "  (pid=%d) — D1 ocupado\n", p->name, p->pid);
}
static void log_irq1() {
    ts(); printf(C_IRQ "IRQ1      " C_RST "** D1 sinaliza término de I/O\n");
}
static void log_iodone(Proc *p) {
    ts(); printf("IO-DONE   " C_IO "<< %s" C_RST "  liberado; volta à fila de prontos\n", p->name);
}
static void log_finished(Proc *p) {
    ts(); printf("FINISHED  " C_ERR "xx %s" C_RST "  (pid=%d)\n", p->name, p->pid);
}
static void log_all_done() {
    ts(); printf("TERMINOU: todos os apps finalizaram; encerrando Kernel\n");
    ts(); printf("SHUTDOWN  " C_SCH "~~" C_RST " Kernel encerrado\n");
}

// ===== Perfil das apps =====
//  - APP_PROFILE=cpu   -> todos CPU-only (A1..An sem I/O)
//  - APP_PROFILE=io    -> todos usam I/O
//  - default (split)   -> A1..A3 CPU-only; A4..A6 usam I/O
static int env_is(const char *name, const char *val) {
    const char *v = getenv(name);
    return v && strcmp(v, val)==0;
}
static int app_should_use_io(int idx) {
    if (env_is("APP_PROFILE","cpu")) return 0;
    if (env_is("APP_PROFILE","io"))  return 1;
    return (idx >= 3); // split default
}

// Pontos determinísticos de I/O p/ quem usa I/O (parecido com seus logs)
static int will_do_io_now(Proc *p) {
    if (!p->uses_io) return 0;
    if (p->pc==3 || p->pc==5 || p->pc==7 || p->pc==9 || p->pc==10 || p->pc==12) return 1;
    return 0;
}
static char io_kind_for(Proc *p) {
    int k = (p->pid + p->pc) % 2;
    return k ? 'R' : 'W';
}

// ===== Dispositivo =====
static void start_io(Proc *p) {
    dev_busy   = 1;
    dev_owner  = p;
    next_irq1_at = now_tick + IO_LAT;
    log_iostart(p);
}
static void on_irq1() {
    if (!dev_busy || !dev_owner) return;
    Proc *done = dev_owner;
    dev_busy = 0;
    dev_owner = NULL;
    next_irq1_at = -1;

    log_iodone(done);
    done->state = READY;
    q_push(&ready_q, done);

    // Pega próximo bloqueado (se houver)
    Proc *next = q_pop(&io_wait_q);
    if (next) start_io(next);
}

// ===== Agendamento =====
static void try_dispatch() {
    if (current) return;
    current = q_pop(&ready_q);
    if (current) {
        current->state = RUNNING;
        log_dispatch(current, current->pc);
    } else {
        log_dispatch(NULL, 0);
    }
}

static int all_finished() {
    for (int i=0;i<nprocs;i++) if (procs[i].state != FINISHED) return 0;
    return 1;
}

int main(int argc, char **argv) {
    int req = 3;
    if (argc >= 2) {
        req = atoi(argv[1]);
        if (req < 1) req = 1;
        if (req > MAX_APPS) req = MAX_APPS;
    }

    nprocs = req;
    log_boot(nprocs);

    // cria processos
    int next_pid = 50000;
    for (int i=0;i<nprocs;i++) {
        Proc *p = &procs[i];
        p->pid = next_pid++;
        snprintf(p->name, sizeof(p->name), "A%d", i+1);
        p->pc = 0;
        p->state = NEW;
        p->rw = '-';
        p->uses_io = app_should_use_io(i);
        p->state = READY;
        q_push(&ready_q, p);
        log_spawn(p);
    }

    try_dispatch();

    // loop principal
    while (1) {
        // Executa 1 tick se houver corrente
        if (current && current->state == RUNNING) {
            current->pc += 1;
            log_pc(current, current->pc);

            if (current->pc >= PC_MAX) {
                current->state = FINISHED;
                log_finished(current);
                current = NULL;
            } else if (will_do_io_now(current)) {
                current->rw = io_kind_for(current);
                const char *op = (current->rw=='R') ? "READ" : "WRITE";
                log_syscall(current, op);
                log_block(current);
                current->state = BLOCKED;
                Proc *blk = current;
                current = NULL;
                if (!dev_busy) start_io(blk);
                else q_push(&io_wait_q, blk);
            }
        }

        // IRQ1 (término de I/O)
        if (next_irq1_at == now_tick) {
            log_irq1();
            on_irq1();
        }

        // Fim de quantum
        int rlen = q_len(ready_q);
        if (current) {
            if (rlen > 0) {
                log_irq0(0);
                log_preempt(current);
                current->state = READY;
                q_push(&ready_q, current);
                current = NULL;
            } else {
                log_irq0(1);
            }
        }

        if (all_finished()) {
            log_all_done();
            break;
        }

        try_dispatch();

        sleep(QUANTUM);
        now_tick += 1;
    }

    return 0;
}
