#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>
#include <vector>
#include "readline.h"

#define MAXLINE 15000
#define MAX_CMD_NUM 15000
#define MAX_CMD_LEN 300

using namespace std;

void handler(int signal)
{
    while(waitpid(-1, NULL, WNOHANG))
        if(errno == ECHILD)  break;

    return;
}

int execute(char *command[], int readfd, int& stdout, int& stderr)
{
    int pipes[2], pipes2[2], status;
    
    pipe(pipes);
    pipe(pipes2);
    
    //printf("Open fd: %d %d\n", pipes[0], pipes[1]);

    if( fork() == 0 )
    {
        dup2(readfd, 0);
        dup2(pipes[1], 1); 
        dup2(pipes2[1], 2);
        close(readfd);
        close(pipes[0]);
        close(pipes[1]);
        close(pipes2[0]);
        close(pipes2[1]);
        
        if(execvp(command[0], command) < 0)    // exec fail
            exit(-1); 
    }
    else 
        wait(&status);
        
    close(pipes[1]);
    close(pipes2[1]);
           
    //printf("exec exit code: %d\n", WEXITSTATUS(status));
        
    if(status == 0)                           // exec succeed
    {
        stdout = pipes[0];
        stderr = pipes2[0];
        return 0; 
    }                 
    else if(WEXITSTATUS(status) == 255)       // exec fail
    {
        close(pipes[0]);
        close(pipes2[0]);
        return -1;
    } 
    else                                      // exec error
    { 
        stdout = pipes[0];
        stderr = pipes2[0];
        return 1;
    }               
}

int checkCounter(vector< pair<int, int> >& pipeCounter)
{
    char output[MAXLINE];
    int merge[2];
    bool newPipe = false;
    
    for(int i = 0; i < pipeCounter.size(); i++)
    {
        if(pipeCounter[i].first == 0)
        {
            if(!newPipe) {
                pipe(merge);
                newPipe = true;
            }
            int n = read(pipeCounter[i].second, output, sizeof(output));
            output[n] = 0;                    
            write(merge[1], output, strlen(output));
            close(pipeCounter[i].second);
            pipeCounter.erase(pipeCounter.begin() + i--);
        }
    }
        
    if(newPipe) {
        close(merge[1]); 
        return merge[0];
    }
    else 
        return -1;     
}

int main(int argc, char *argv[], char *envp[])
{
    int listenfd, connfd;
    struct sockaddr_in server_addr;
    
    if(argc != 2) {
        printf("Usage: ./server <port>\n");
        exit(0);
    }
    
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(argv[1]));
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket Error");
        exit(errno);
    }

    if( bind(listenfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("Bind Error");
        exit(errno);
    }

    if( listen(listenfd, 20) < 0 ) {
        perror("Listen Error");
        exit(errno);
    }

    signal(SIGCHLD, handler);

    while(1)
    {
        connfd = accept(listenfd, (struct sockaddr *) NULL, NULL);
        if(errno == EINTR)  continue;

        if( fork() == 0 )
        {
            int n;
            char welcome[] = 
            "****************************************\n"
            "** Welcome to the information server. **\n"
            "****************************************\n";
            char prompt[] = "% ";
            char buffer[MAXLINE];
            char home[MAXLINE] = "\0";
            vector < pair<int, int > > pipeCounter;
                        
            close(listenfd);
            
            signal(SIGCHLD, SIG_DFL);
            
            // change directory to ~/ras
            strcat(home, getenv("HOME"));
            strcat(home, "/ras");
            chdir(home); 
            
            // set PATH
            setenv("PATH", "bin:.", 1);
            
            write(connfd, welcome, strlen(welcome));
            write(connfd, prompt, strlen(prompt));

            while((n = readline(connfd, buffer, MAXLINE)) > 0)
            {
                char *pch, *arg[MAX_CMD_NUM], *command[MAX_CMD_NUM];
                int fd, argN = 0, commandN = 0, readfd = 0, stdout = 1, stderr = 2;
                
                // get first command
                arg[0] = new char[MAX_CMD_LEN];          
                if((pch = strtok(buffer, " \r\n")))
                    strcpy(arg[0], pch);
                    
                // get arguments
                while(pch != NULL) 
                {
                    arg[++argN] = new char[MAX_CMD_LEN];
                    if((pch = strtok(NULL, " \r\n")))
                        strcpy(arg[argN], pch);
                    else
                        arg[argN++] = NULL;
                }
                    
                // receive exit
                if(!strcmp(arg[0], "exit"))
                    break;
                    
                // receive setenv    
                else if(!strcmp(arg[0], "setenv"))    
                    setenv(arg[1], arg[2], 1);
                
                // receive printenv
                else if(!strcmp(arg[0], "printenv"))    
                {
                    if(argN == 3)         // print specific environment variable
                    {
                        char output[MAXLINE];
                        snprintf(output, sizeof(output), "%s=%s\n", arg[1], getenv(arg[1]));
                        write(connfd, output, strlen(output));
                    }
                    else if(argN == 2)    // print all environment variable
                    {
                        char output[MAXLINE] = "\0";
                        char temp[MAXLINE];
                        for (int i = 0; envp[i] != NULL; i++) {
                            snprintf(temp, sizeof(temp), "%s\n", envp[i]);
                            strcat(output, temp);
                        } 
                        write(connfd, output, strlen(output));
                    }    
                }
                
                else 
                for(int i = 0; i < argN; i++)
                {
                    if(arg[i] == NULL)    // last command
                    {
                        char output[MAXLINE];

                        command[commandN] = NULL;
                        commandN = 0;
                        
                        // check numbered-pipe
                        // if there's no zero then return -1, else return new readfd
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;
                        
                        // read from readfd, stdout and stderr will be set when return
                        // return 0 if succeed, return 1 if error, return -1 if fail   
                        if( execute(command, readfd, stdout, stderr) < 0 ) 
                        {
                            char error[MAXLINE];                        
                            snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                            write(connfd, error, strlen(error));  
                            
                            for(int i = 0; i < pipeCounter.size(); i++)
                                pipeCounter[i].first += 1;
                            
                            // ls |1
                            // ctt
                            // cat      
                            if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                pipeCounter.push_back(make_pair(1, readfd));
                            else if(readfd != 0) 
                                close(readfd);
                                
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];

                            break;
                        }
                        
                        for(int k = 0; command[k] != NULL; k++)
                            delete [] command[k];
                        
                        if(readfd != 0)    
                            close(readfd);
                        
                        readfd = stdout;

                        n = read(stderr, output, sizeof(output));
                        output[n] = 0;                    
                        write(connfd, output, strlen(output));
                        n = read(stdout, output, sizeof(output));
                        output[n] = 0;                    
                        write(connfd, output, strlen(output));
                        
                        close(stderr);
                        close(stdout);
                    }
                    else if(!strcmp(arg[i], ">"))    // redirect stdout to file
                    {
                        char output[MAXLINE];
 
                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;
                        
                        if( execute(command, readfd, stdout, stderr) < 0 ) 
                        {
                            char error[MAXLINE];
                            snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                            write(connfd, error, strlen(error)); 
                            
                            for(int i = 0; i < pipeCounter.size(); i++)
                                pipeCounter[i].first += 1;
                            
                            if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                pipeCounter.push_back(make_pair(1, readfd));
                            else if(readfd != 0) 
                                close(readfd);
                                
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];     
                                                           
                            break;   
                        }
                        
                        for(int k = 0; command[k] != NULL; k++)
                            delete [] command[k];
                        
                        if(readfd != 0)    
                            close(readfd);
                        
                        FILE *fp = fopen(arg[i+1], "w");
                        
                        if(fp) {           
                            n = read(stdout, output, sizeof(output));
                            output[n] = 0;                    
                            write(fileno(fp), output, strlen(output));
                            fclose(fp);
                        }
                        else {
                            char error[MAXLINE];
                            snprintf(error, sizeof(error), "Invalid file name: [%s].\n", arg[i+1]);
                            write(connfd, error, strlen(error));
                        }
                        
                        n = read(stderr, output, sizeof(output));
                        output[n] = 0;                    
                        write(connfd, output, strlen(output));

                        close(stdout);
                        close(stderr);
                        
                        break;
                    }
                    else if(!strcmp(arg[i], "|"))    // pipe stdout to next command 
                    { 
                        char output[MAXLINE];
                        
                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;
                           
                        if( execute(command, readfd, stdout, stderr) < 0 ) 
                        {
                            char error[MAXLINE];
                            snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                            write(connfd, error, strlen(error)); 
                            
                            for(int i = 0; i < pipeCounter.size(); i++)
                                pipeCounter[i].first += 1;
                                
                            if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                pipeCounter.push_back(make_pair(1, readfd));
                            else if(readfd != 0) 
                                close(readfd);
                                
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];
                            
                            break;
                        }
                        
                        for(int k = 0; command[k] != NULL; k++)
                            delete [] command[k];
                        
                        if(readfd != 0)    
                            close(readfd);
                        
                        readfd = stdout; 
                        
                        n = read(stderr, output, sizeof(output));
                        output[n] = 0;                    
                        write(connfd, output, strlen(output));
                        
                        close(stderr);
                    }
                    else if(arg[i][0] == '|')    // pipe stdout to next N line command 
                    {
                        char output[MAXLINE];
                        int number = 0;
                        
                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;

                        if( execute(command, readfd, stdout, stderr) < 0 ) 
                        {
                            char error[MAXLINE];
                            snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                            write(connfd, error, strlen(error)); 
                            
                            for(int i = 0; i < pipeCounter.size(); i++)
                                pipeCounter[i].first += 1;
                                
                            if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                pipeCounter.push_back(make_pair(1, readfd));
                            else if(readfd != 0) 
                                close(readfd);
                                
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];
                                
                            break;
                        }
                        
                        for(int k = 0; command[k] != NULL; k++)
                            delete [] command[k];
                        
                        if(readfd != 0)    
                            close(readfd);
                        
                        if((pch = strtok(arg[i], "|")))
                            if( (number = atoi(pch)) > 0 )
                                pipeCounter.push_back(make_pair(number * 2, stdout));
                        
                        if(arg[i+1] != NULL && arg[i+1][0] == '!') {
                            if((pch = strtok(arg[i+1], "!")))
                                if( (number = atoi(pch)) > 0 )
                                    pipeCounter.push_back(make_pair(number * 2, stderr)); 
                        }
                        else {
                            n = read(stderr, output, sizeof(output));
                            output[n] = 0;                    
                            write(connfd, output, strlen(output));      
                            close(stderr);
                        }
                             
                        break;          
                    }
                    else if(arg[i][0] == '!')    // pipe stderr to next N line command 
                    {
                        char output[MAXLINE];
                        int number = 0;
                        
                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if( (fd = checkCounter(pipeCounter)) != -1)
                            readfd = fd;

                        if( execute(command, readfd, stdout, stderr) < 0 ) 
                        {
                            char error[MAXLINE];
                            snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                            write(connfd, error, strlen(error));  
                            
                            for(int i = 0; i < pipeCounter.size(); i++)
                                pipeCounter[i].first += 1;
                                
                            if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                pipeCounter.push_back(make_pair(1, readfd));
                            else if(readfd != 0) 
                                close(readfd);
                            
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];       
                                                         
                            break;
                        }
                        
                        for(int k = 0; command[k] != NULL; k++)
                            delete [] command[k];    
                                                
                        if(readfd != 0)    
                            close(readfd);
                        
                        if((pch = strtok(arg[i], "!")))
                            if( (number = atoi(pch)) > 0 )
                                pipeCounter.push_back(make_pair(number * 2, stderr));
                        
                        if(arg[i+1] != NULL && arg[i+1][0] == '|') {
                            if((pch = strtok(arg[i+1], "|"))) 
                                if( (number = atoi(pch)) > 0 )
                                    pipeCounter.push_back(make_pair(number * 2, stdout));
                        }
                        else {    
                            n = read(stdout, output, sizeof(output));
                            output[n] = 0;                    
                            write(connfd, output, strlen(output)); 
                            close(stdout);
                        }
                             
                        break;          
                    }
                    else 
                    {
                        command[commandN] = new char[MAX_CMD_LEN];
                        strcpy(command[commandN++], arg[i]);
                    }
                }
                
                for(int i = 0; i < pipeCounter.size(); i++)
                    pipeCounter[i].first -= 1;
                    
                for(int i = 0; i < argN; i++)
                    delete [] arg[i];
                    
                write(connfd, prompt, strlen(prompt)); 
            }
                // client close connection
                if(n == 0)
                    printf("client has closed the connection.\n");
                close(connfd);
                exit(0);
        }
            close(connfd);
    }

    return 0;
}

