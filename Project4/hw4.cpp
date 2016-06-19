#include <iostream>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <arpa/inet.h>

#include "Client.h"

#define MAXLINE 15000
#define MAX_SERVER_NUM 5

#define CONNECTING 0
#define READING 1
#define WRITING 2
#define SEND_SOCKS_REQUEST 3
#define READ_SOCKS_REQUEST 4

using namespace std;

Client client[MAX_SERVER_NUM + 1];
vector <Client *> allclient;

char* hostname_to_ip(char *host)
{
	struct hostent *hptr;

	if( (hptr = gethostbyname(host)) == NULL ) {	
        printf("gethostbyname error<br>");
        exit(errno);
	}

	return inet_ntoa(*((struct in_addr*) hptr->h_addr));
}

void printHtml()
{
	cout << "<html>" << endl;
	cout << "<head>" << endl;
	cout << "<meta http-equiv='Content-Type' content='text/html; charset=big5' />" << endl;
	cout << "<title>Network Programming Homework 3</title>" << endl;
	cout << "</head>" << endl;
	cout << "<body bgcolor=#336699>" << endl;
	cout << "<font face='Courier New' size=2 color=#FFFF99>" << endl;

	cout << "<table width='800' border='1'>" << endl;
	cout << "<tr>" << endl;
	for(int i = 0; i < allclient.size(); i++)
		printf("<td>%s/%d %s/%d %s</td>\n", allclient[i]->socks_server_ip, allclient[i]->socks_server_port, allclient[i]->server_ip, allclient[i]->server_port, allclient[i]->filename);
	cout << "</tr>" << endl;
	cout << "<tr>" << endl;
	for(int i = 0; i < allclient.size(); i++)
		printf("<td valign='top' id='m%d'></td>\n", allclient[i]->id);
	cout << "</tr>" << endl;
	cout << "</table>" << endl;

  	fflush(stdout);
}

int main()
{
	int maxfd = 0, nready, connection = 0;
	char *data, *pch;
	fd_set allset, rset, wset;

	FD_ZERO(&allset);

	cout << "Content-type: text/html\n\n";

	data = getenv("QUERY_STRING");

	// parse QUERY_STRING
	for(int i = 1; i <= MAX_SERVER_NUM; i++) {

		i == 1 ? pch = strtok(data, "&") : pch = strtok(NULL, "&");
		if(pch == NULL) break;

		// support hostname and ip conversion
		if(sscanf(pch, "%*[^=]=%50s", client[i].server_ip) == 1)
			strcpy(client[i].server_ip, hostname_to_ip(client[i].server_ip));

		if((pch = strtok(NULL, "&")))
			sscanf(pch, "%*[^=]=%d", &client[i].server_port);

		if((pch = strtok(NULL, "&")))
			sscanf(pch, "%*[^=]=%100s", client[i].filename);

		if((pch = strtok(NULL, "&"))) {
			if(sscanf(pch, "%*[^=]=%50s", client[i].socks_server_ip) == 1)
				strcpy(client[i].socks_server_ip, hostname_to_ip(client[i].socks_server_ip));
		}

		if((pch = strtok(NULL, "&")))
			sscanf(pch, "%*[^=]=%d", &client[i].socks_server_port);	
	}

	// check whether input is valid
	for(int i = 1, id = 1; i <= MAX_SERVER_NUM; i++)
	{
		if(client[i].server_ip[0] != '\0' && client[i].filename[0] != '\0' && client[i].server_port > 0 && client[i].socks_server_ip[0] != '\0' && client[i].socks_server_port > 0) {
			client[i].id = id++;
			allclient.push_back(&client[i]);
		}
	}

	printHtml();

	// connect to server
	for(int i = 0; i < allclient.size(); i++) 
	{	
		if( allclient[i]->setConnection() < 0) {
			allclient[i]->printText(allclient[i]->errorMsg);
			allclient.erase(allclient.begin() + i--);
		}
		else {
			allclient[i]->status = CONNECTING;
			if(allclient[i]->sockfd > maxfd)
				maxfd = allclient[i]->sockfd;
			FD_SET(allclient[i]->sockfd, &allset);
			connection++;
		}
	}	

	while(connection > 0)
	{
	    rset = wset = allset;
	    nready = select(maxfd + 1, &rset, &wset, NULL, NULL);

	    for(int i = 0; i < allclient.size(); i++)
	    {
		    if(allclient[i]->status == CONNECTING && (FD_ISSET(allclient[i]->sockfd, &wset) || FD_ISSET(allclient[i]->sockfd, &rset)))
		    {
		    	int ret, error;
		    	socklen_t len;

		    	if( ((ret = getsockopt(allclient[i]->sockfd, SOL_SOCKET, SO_ERROR, &error, &len)) < 0) || (error != 0) ) {
					// non-blocking connect failed
					char errorMsg[MAXLINE];

					if(ret < 0)
						strcpy(errorMsg, "getsockopt error");
					else
						strcpy(errorMsg, strerror(error));

					allclient[i]->printText("non-blocking connect failed<br>");
					allclient[i]->printText(errorMsg);

					close(allclient[i]->sockfd);
		    		fclose(allclient[i]->file);
		    		FD_CLR(allclient[i]->sockfd, &allset);
		    		allclient.erase(allclient.begin() + i--);
					connection--;
					// because erase i--, need continue to prevent process others sockfd;
					continue;
				}
				else
					allclient[i]->status = SEND_SOCKS_REQUEST;
		    }

		    if(FD_ISSET(allclient[i]->sockfd, &wset))
		    {
		    	if(allclient[i]->status == SEND_SOCKS_REQUEST)
		    	{
			    	unsigned char buffer[9];
			    	buffer[0] = 4;
				    buffer[1] = 1;
				    buffer[2] = (unsigned int) allclient[i]->server_port / 256;
				    buffer[3] = (unsigned int) allclient[i]->server_port % 256;
				    sscanf(allclient[i]->server_ip, "%u.%u.%u.%u", &buffer[4], &buffer[5], &buffer[6], &buffer[7]);

				    // send socks request
				    write(allclient[i]->sockfd, buffer, 8);

				    allclient[i]->status = READ_SOCKS_REQUEST;
		    	}
		    	else if(allclient[i]->status == WRITING)
		    	{
		    		char line[MAXLINE]; 
			    	int nread, nwrite;  

			    	if(fgets(line, sizeof(line), allclient[i]->file)) {  

			    		int pos;
			    		char buffer[MAXLINE]; 

			    		nread = strlen(line);

						string text = string(line);

			    		while( (pos = text.find("<")) != string::npos)
							text.replace(pos, 1, "&lt;");
						while( (pos = text.find(">")) != string::npos)
							text.replace(pos, 1, "&gt;");
						while( (pos = text.find("\r\n")) != string::npos)
							text.replace(pos, 2, "<br>");
						while( (pos = text.find("\n\r")) != string::npos)
							text.replace(pos, 2, "<br>");
						while( (pos = text.find("\n")) != string::npos)
							text.replace(pos, 1, "<br>");
						while( (pos = text.find("\r")) != string::npos)
							text.replace(pos, 1, "<br>");

			    		snprintf(buffer, sizeof(buffer), "<b>%s</b>", text.c_str());
			    		allclient[i]->printText(buffer);

			    		nwrite = write(allclient[i]->sockfd, line, strlen(line));

			    		if(nwrite < nread)
			    			fseek(allclient[i]->file, -(nread - nwrite), SEEK_CUR);
			    	}

			    	if(nwrite >= nread)
			    		allclient[i]->status = READING;
		    	}
		    }

		    if(FD_ISSET(allclient[i]->sockfd, &rset))
		    {
		    	if(allclient[i]->status == READ_SOCKS_REQUEST)
		    	{
			    	int n;
			    	unsigned char buffer[MAXLINE];

			    	if( (n = read(allclient[i]->sockfd, buffer, 8)) > 0 )
			    	{
			    		if(buffer[1] == 90)
			    			allclient[i]->status = READING;
			    		else 
			    		{
							allclient[i]->printText("Socks request reject<br>");
			    			close(allclient[i]->sockfd);
				    		fclose(allclient[i]->file);
				    		FD_CLR(allclient[i]->sockfd, &allset);
				    		allclient.erase(allclient.begin() + i--);
				    		connection--;
				    		continue;
			    		}
			    	}
			    	else
			    	{
			    		allclient[i]->printText("Socks server connection refused");
		    			close(allclient[i]->sockfd);
			    		fclose(allclient[i]->file);
			    		FD_CLR(allclient[i]->sockfd, &allset);
			    		allclient.erase(allclient.begin() + i--);
						connection--;
						continue;
			    	}
		    	}
		    	else if(allclient[i]->status == READING)
		    	{
			    	int n;
			    	char buffer[MAXLINE];

			    	if( (n = read(allclient[i]->sockfd, buffer, MAXLINE)) > 0 )
			    	{
			    		buffer[n] = 0;

			    		int pos;
			    		string text = string(buffer);

			    		while( (pos = text.find("<")) != string::npos)
							text.replace(pos, 1, "&lt;");
						while( (pos = text.find(">")) != string::npos)
							text.replace(pos, 1, "&gt;");
						while( (pos = text.find("\r\n")) != string::npos)
							text.replace(pos, 2, "<br>");
						while( (pos = text.find("\n\r")) != string::npos)
							text.replace(pos, 2, "<br>");
						while( (pos = text.find("\n")) != string::npos)
							text.replace(pos, 1, "<br>");
						while( (pos = text.find("\r")) != string::npos)
							text.replace(pos, 1, "<br>");

			    		allclient[i]->printText(text.c_str());

			    		if(text.find("%") != string::npos)
			    			allclient[i]->status = WRITING;
			    	}
			    	// connection close
			    	else
			    	{
			    		close(allclient[i]->sockfd);
			    		fclose(allclient[i]->file);
			    		FD_CLR(allclient[i]->sockfd, &allset);
			    		allclient.erase(allclient.begin() + i--);
						connection--;
						continue;
			    	}
		    	}
		    }

		    if(--nready <= 0)
		    	break;
	    }
	}

	cout << "</font>" << endl;
	cout << "</body>" << endl;
	cout << "</html>" << endl;

	cout << "CGI Program exit" << endl;

	return 0;
}
