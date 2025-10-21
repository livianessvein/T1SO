CC      = clang
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lpthread

# Binários “separados”, cada um com seu main()
BINS = kernel_sim inter_controller app

all: $(BINS) kernel   # 'kernel' é só um alias/cópia de kernel_sim

kernel_sim: kernel_sim.c common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

inter_controller: inter_controller.c common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

app: app.c common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

kernel: kernel_sim
	cp -f kernel_sim kernel

clean:
	rm -f $(BINS) kernel *.o saida_*.txt

.PHONY: all clean test_cpu test_io test_all

# ---- Testes do professor ----
test_cpu: kernel app
	@echo "== Teste CPU (A1..A3 sem I/O) =="
	APP_PROFILE=cpu ./kernel 3 | tee saida_cpu.txt

test_io: kernel app
	@echo "== Teste I/O (A4..A6 com I/O) =="
	APP_PROFILE=io APP_NAME_OFFSET=3 ./kernel 3 | tee saida_io.txt

test_all: kernel app
	@echo "== Teste completo (6 apps mistos) =="
	APP_PROFILE=split ./kernel 6 | tee saida_6apps.txt