CC=cc
CFLAGS = -g -Wall
LDFLAGS = -pthread
EXE_1 = rpc-server
EXE_2 = rpc-client
RPC_SYSTEM=rpc.o

.PHONY: format all

all: $(RPC_SYSTEM) $(EXE_1) $(EXE_2)

$(RPC_SYSTEM): rpc.c rpc.h
	$(CC) -c -o $@ $< $(CFLAGS)

RPC_SYSTEM_A=rpc.a
$(RPC_SYSTEM_A): rpc.o
	ar rcs $(RPC_SYSTEM_A) $(RPC_SYSTEM)

$(EXE_1): server.a
	$(CC) $(CFLAGS) -o $(EXE_1) $< $(RPC_SYSTEM) $(LDFLAGS)

$(EXE_2): client.a
	$(CC) $(CFLAGS) -o $(EXE_2) $< $(RPC_SYSTEM) $(LDFLAGS)

%.o: %.c %.h
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f *.o $(EXE_1) $(EXE_2)


format:
	clang-format -style=file -i *.c *.h
