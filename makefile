all:single_process_server.cpp multi_process_server.cpp readline.h
	g++ single_process_server.cpp -o single_process_server	
	g++ -pthread multi_process_server.cpp -o multi_process_server
clean:
	rm -f single_process_server multi_process_server
