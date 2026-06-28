# ===========================================================================
# Makefile  -  Distributed Software Update Framework (ENCS4330 Project #3)
# ---------------------------------------------------------------------------
# Targets:
#     make            build server, client and the OpenGL monitor
#     make server     build just the server
#     make client     build just the client
#     make monitor    build just the GL monitor
#     make clean      remove build artifacts
#
# The -g flag is included so you can debug with gdb, as suggested in the spec.
# ===========================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -g -O2 -pthread
LDLIBS_NET := -pthread
LDLIBS_GL  := -lglut -lGLU -lGL -lm

SRC := src
BIN := bin

# Object files shared by server and client.
COMMON_OBJ := $(BIN)/netutil.o $(BIN)/logger.o $(BIN)/config.o $(BIN)/sha256.o

.PHONY: all server server_adv client monitor clean dirs

all: dirs server server_adv client monitor

dirs:
	@mkdir -p $(BIN) logs server_repo client_storage

# ---- shared objects -------------------------------------------------------
$(BIN)/netutil.o: $(SRC)/netutil.c $(SRC)/common.h
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN)/logger.o: $(SRC)/logger.c $(SRC)/logger.h
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN)/config.o: $(SRC)/config.c $(SRC)/config.h
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN)/sha256.o: $(SRC)/sha256.c $(SRC)/sha256.h
	$(CC) $(CFLAGS) -c $< -o $@

# ---- server ---------------------------------------------------------------
server: dirs $(BIN)/server
$(BIN)/server: $(SRC)/server.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $(SRC)/server.c $(COMMON_OBJ) -o $@ $(LDLIBS_NET)

# ---- advanced server (fork + threads + SysV shm/sem + real-time) ----------
server_adv: dirs $(BIN)/server_adv
$(BIN)/server_adv: $(SRC)/server_adv.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $(SRC)/server_adv.c $(COMMON_OBJ) -o $@ $(LDLIBS_NET)

# ---- client ---------------------------------------------------------------
client: dirs $(BIN)/client
$(BIN)/client: $(SRC)/client.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $(SRC)/client.c $(COMMON_OBJ) -o $@ $(LDLIBS_NET)

# ---- OpenGL monitor -------------------------------------------------------
monitor: dirs $(BIN)/monitor_gl
$(BIN)/monitor_gl: $(SRC)/monitor_gl.c $(BIN)/config.o
	$(CC) $(CFLAGS) $(SRC)/monitor_gl.c $(BIN)/config.o -o $@ $(LDLIBS_GL)

clean:
	rm -rf $(BIN) logs/*.log client_storage/*.bin
	@echo "cleaned."
