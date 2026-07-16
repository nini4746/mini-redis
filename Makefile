CC = cc
CFLAGS = -std=c11 -Wall -Wextra -O2
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)
BIN = mini-redis

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

test: $(BIN)
	bash tests/run.sh

clean:
	rm -f $(BIN)

.PHONY: clean test
