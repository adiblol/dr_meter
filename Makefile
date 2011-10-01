
CPP  = g++
CC   = gcc
OBJ  = main.o
LINKOBJ  = $(OBJ)
LIBS =  -L"/usr/lib"
INCS =  -I"/usr/include"
CXXINCS =  $(INCS)
BIN  = dr_meter
CXXFLAGS = $(CXXINCS) -O3 -ggdb
CFLAGS = $(INCS) -O3 -ggdb
SOURCE = main.cpp
OBJECTS = $(SOURCES:.cpp=.o)


.PHONY: all clean build run

build: $(BIN)

clean:
	rm -f $(OBJ) $(BIN)

$(BIN): $(OBJ)
	$(CPP) $(LINKOBJ) -o $(BIN) $(LIBS)

main.o: main.cpp $(SOURCE)
	$(CPP) -c main.cpp -o main.o $(CXXFLAGS)

run: $(BIN)
	./$(BIN)

