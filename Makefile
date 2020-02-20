all: main

main: main.cpp function.cpp proxy.cpp proxy.h 
	g++ -g -o main main.cpp function.cpp proxy.cpp

.PHONY:
	clean
clean:
	rm -rf *.o main