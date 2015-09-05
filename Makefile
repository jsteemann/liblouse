
.PHONY: install clean

all: build

build: louse.cc
	@mkdir -p out
	$(CC) -rdynamic -Wall -Wextra -g -O3 -std=c++11 -shared -fPIC louse.cc -o out/liblouse.so -lstdc++ -lunwind -ldl -lpthread

install: build
	cp `pwd`/louse /usr/bin
	cp `pwd`/out/liblouse.so /usr/lib

clean:
	rm -Rf out/*

