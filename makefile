CC = clang
CFLAGS = -Wall -Wextra -O2

all: kernel_sim inter_controller app

kernel_sim: kernel_sim.c common.h
	$(CC) $(CFLAGS) -o kernel_sim kernel_sim.c

inter_controller: inter_controller.c common.h
	$(CC) $(CFLAGS) -o inter_controller inter_controller.c

app: app.c common.h
	$(CC) $(CFLAGS) -o app app.c

clean:
	rm -f kernel_sim inter_controller app