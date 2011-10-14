
OBJ  = main.o
LINKOBJ  = $(OBJ)
BIN  = dr_meter
CFLAGS = -O3 -ggdb -std=c99 -Wall -Wextra -Werror
LDLIBS = -lm -lavformat -lavcodec -lavutil
SOURCES = main.c
OBJECTS = $(SOURCES:.c=.o)


.PHONY: all clean build

build: $(BIN)

clean:
	rm -f $(OBJ) $(BIN)

$(BIN): $(OBJ) Makefile
	$(CC) $(LDFLAGS) $< -o $@ $(LDLIBS)

main.o: main.c Makefile

