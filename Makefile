
.PHONY: out-directory install clean

OBJ = src/MemoryAllocation.o src/Configuration.o src/Heap.o src/Tracker.o src/liblouse.o

all: build

build: out-directory $(OBJ)
	$(CC) -rdynamic -Wall -Wextra -g -O3 -std=c++11 -shared -fPIC $(OBJ) -o out/liblouse.so -lstdc++ -lunwind -ldl -lpthread

%.o: %.cc 
	$(CC) -Wall -Wextra -g -O3 -std=c++11 -fPIC -c -o $@ $<

out-directory: 
	@mkdir -p out

install: build
	cp `pwd`/bin/louse /usr/bin
	cp `pwd`/out/liblouse.so /usr/lib

clean:
	rm -rf $(OBJ) src/*.gch out/*

