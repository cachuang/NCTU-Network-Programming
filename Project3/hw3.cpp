#include <iostream>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "Client.h"

#define MAXLINE 15000
#define MAX_SERVER_NUM 5

#define CONNECTING 0
#define READING 1
#define WRITING 2

using namespace std;

Client client[MAX_SERVER_NUM + 1];
vector <Client *> allclient;

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
		printf("<td>%s/%d %s</td>\n", allclient[i]->server_ip, allclient[i]->server_port, allclient[i]->filename);
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
		sscanf(pch, "%*[^=]=%16s", client[i].server_ip);
		if((pch = strtok(NULL, "&")));
			sscanf(pch, "%*[^=]=%d", &client[i].server_port);
		if((pch = strtok(NULL, "&")));
			sscanf(pch, "%*[^=]=%100s", client[i].filename);
	}

	// check whether input is valid
	for(int i = 1, id = 1; i <= MAX_SERVER_NUM; i++)
	{
		if(client[i].server_ip[0] != '\0' && client[i].filename[0] != '\0' && client[i].server_port > 0) {
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
		    	int error;
		    	socklen_t len;

		    	if(getsockopt(allclient[i]->sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
					// non-blocking connect failed
					char error[] = "non-blocking failed<br>";
					allclient[i]->printText(error);
					allclient.erase(allclient.begin() + i--);
					connection--;
				}
				else
					allclient[i]->status = READING;
		    }

		    if(allclient[i]->status == WRITING && FD_ISSET(allclient[i]->sockfd, &wset))
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
		    if(allclient[i]->status == READING && FD_ISSET(allclient[i]->sockfd, &rset))
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
