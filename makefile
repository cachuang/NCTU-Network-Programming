all:multi_process_server.cpp readline.h
	g++ -pthread multi_process_server.cpp -o server
clean:
	rm -f server
