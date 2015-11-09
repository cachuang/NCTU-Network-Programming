all:server.cpp readline.h
	g++ server.cpp -o server
clean:
	rm -f server
