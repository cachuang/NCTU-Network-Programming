#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>
#include <vector>
#include "readline.h"

#define MAXLINE 15000
#define MAX_CMD_NUM 15000
#define MAX_CMD_LEN 300
#define MAX_SHM_SIZE 1000
#define MAX_CLIENT_NUM 30

using namespace std;

extern char **environ;
char *sigusr_message;
int sigusrfd;
int client_shm_id, sigusr_shm_id;

struct clientData
{
    int id, pid, port;
    char ip[MAXLINE], name[MAXLINE];
    bool exist;
};

void sigint_handler(int signal)
{
    shmctl(client_shm_id, IPC_RMID, NULL);
    shmctl(sigusr_shm_id, IPC_RMID, NULL);
    
    exit(1);
}

void sigusr_handler(int signal)
{
    if(signal == SIGUSR1)
        write(sigusrfd, sigusr_message, strlen(sigusr_message));
}

void sigchld_handler(int signal)
{
    while(waitpid(-1, NULL, WNOHANG))
        if(errno == ECHILD)  break;

    return;
}

void broadcast(struct clientData *clients, char *message)
{
    strcpy(sigusr_message, message);
    
    for(int i = 1; i <= MAX_CLIENT_NUM; i++)
        if(clients[i].exist)
            kill(clients[i].pid, SIGUSR1);
}

// read from readfd, stdout and stderr will be set when return
// return 0 if succeed, return 1 if error, return -1 if fail  
int execute(char *command[], int readfd, int& stdout, int& stderr)
{
    int pipes[2], pipes2[2], status;
    
    pipe(pipes);
    pipe(pipes2);

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
    
    for(int i = 0; command[i] != NULL; i++)
        delete [] command[i];
    
    // exec succeed
    if(status == 0) {
        stdout = pipes[0];
        stderr = pipes2[0];
        return 0; 
    }       
    // exec fail          
    else if(WEXITSTATUS(status) == 255) {
        close(pipes[0]);
        close(pipes2[0]);
        return -1;
    } 
    // exec error
    else { 
        stdout = pipes[0];
        stderr = pipes2[0];
        return 1;
    }               
}

// check numbered-pipe                                       
// if there's no any counter's count equals to zero then return -1, else return new readfd 
int checkCounter(vector< pair<int, int> >& pipeCounter)
{
    char output[MAXLINE];
    int merge[2];
    bool newPipe = false;
    
    for(int i = 0; i < pipeCounter.size(); i++)
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
        
    if(newPipe) {
        close(merge[1]); 
        return merge[0];
    }
    else 
        return -1;     
}

void error_cmd_handler(int sockfd, vector < pair<int, int > > &pipeCounter, char *cmd, char *arg, int readfd, int mergefd)
{
    char error[MAXLINE];                        
    snprintf(error, sizeof(error), "Unknown command: [%s].\n", cmd);
    write(sockfd, error, strlen(error));  
    
    if(!strcmp(arg, cmd))
        for(int i = 0; i < pipeCounter.size(); i++)
            pipeCounter[i].first += 1;
    
    // ls |1
    // ctt
    // cat      
    if(!strcmp(arg, cmd) && mergefd != -1) 
        pipeCounter.push_back(make_pair(1, readfd));
    else if(readfd != 0) 
        close(readfd);
}

// read from src and write to dst
void read_write(int src, int dst)
{
    char buffer[MAXLINE];
    
    int n = read(src, buffer, sizeof(buffer));
    buffer[n] = 0;
    write(dst, buffer, strlen(buffer));
    
    close(src);
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
    
    // create share memory
    client_shm_id = shmget(IPC_PRIVATE, (MAX_CLIENT_NUM + 1) * sizeof(struct clientData), IPC_CREAT | 0600);
    sigusr_shm_id = shmget(IPC_PRIVATE, MAXLINE * sizeof(char), IPC_CREAT | 0600);
    
    struct clientData* clients = (struct clientData *) shmat(client_shm_id, NULL, 0);
    
    for(int i = 0; i <= MAX_CLIENT_NUM; i++)
        clients[i].exist = false;
 
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);

    while(1)
    {
        socklen_t len;
        struct sockaddr_in clientaddr;
        char ip[INET_ADDRSTRLEN];
        int port, id;
        
        len = sizeof(clientaddr);
        connfd = accept(listenfd, (struct sockaddr *) &clientaddr, &len);
        if(errno == EINTR)  continue;
        
        inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof(ip));
        port = ntohs(clientaddr.sin_port);
        
        
        for(id = 1; id <= MAX_CLIENT_NUM; id++) {
            if(!clients[id].exist) {
                clients[id].exist = true;
                clients[id].id = id;
                clients[id].port = port;
                strcpy(clients[id].name, "(no name)");
                strcpy(clients[id].ip, ip);
                break;
            }
        }

        if( fork() == 0 )
        {
            int n;
            char welcome[] = 
            "****************************************\n"
            "** Welcome to the information server. **\n"
            "****************************************\n";
            char prompt[] = "% ";
            char buffer[MAXLINE];
            char home[MAXLINE] = "";
            vector < pair<int, int > > pipeCounter;
                        
            close(listenfd);
            
            sigusrfd = connfd;
            clients[id].pid = getpid();
            
            signal(SIGCHLD, SIG_DFL);
            signal(SIGUSR1, sigusr_handler);
            
            // attach share memory
            struct clientData* clients = (struct clientData*) shmat(client_shm_id, NULL, 0);
            sigusr_message = (char *) shmat(sigusr_shm_id, NULL, 0);
            
            // change directory to ~/ras
            strcat(home, getenv("HOME"));
            strcat(home, "/ras");
            chdir(home); 
            
            //clearenv();
            // set PATH
            setenv("PATH", "bin:.", 1);
            
            write(connfd, welcome, strlen(welcome));
            write(connfd, prompt, strlen(prompt));

            while((n = readline(connfd, buffer, MAXLINE)) > 0)
            {
                char *pch, *arg[MAX_CMD_NUM], *command[MAX_CMD_NUM], text[MAXLINE];
                int fd, argN = 0, commandN = 0, readfd = 0, stdout = 1, stderr = 2;
                
                strcpy(text, buffer);
                
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
                        for (int i = 0; environ[i] != NULL; i++) {
                            snprintf(temp, sizeof(temp), "%s\n", environ[i]);
                            strcat(output, temp);
                        } 
                        write(connfd, output, strlen(output));
                    }    
                }
                else if(!strcmp(arg[0], "who"))    
                {
                    char message[MAXLINE] = "\0";
                    char temp[MAXLINE];
                    
                    strcat(message, "<ID>\t<nickname>\t<IP/port>\t<indicate me>\n");
                    
                    for(int i = 1; i <= MAX_CLIENT_NUM; i++)
                    {
                        if(clients[i].exist)
                        {
                            if(id == i)
                                snprintf(temp, sizeof(temp), "%d\t%s\t%s/%d\t<-me\n", clients[i].id, clients[i].name, clients[i].ip, clients[i].port);
                            else 
                                snprintf(temp, sizeof(temp), "%d\t%s\t%s/%d\t\n", clients[i].id, clients[i].name, clients[i].ip, clients[i].port);    
                            strcat(message, temp);
                        }
                    }
                    write(connfd, message, strlen(message));
                }
                
                // command "name"
                else if(!strcmp(arg[0], "name"))
                {
                    bool unique = true;
                    
                    // check if name already exist
                    for(int j = 1; j <= MAX_CLIENT_NUM; j++)
                        if(clients[j].exist && arg[1] != NULL && !strcmp(clients[j].name, arg[1]))                {
                            unique = false;
                            break;
                        }
                    
                    // name already exist
                    if(!unique)
                    {
                        char error[MAXLINE];
                        snprintf(error, sizeof(error), "*** User '%s' already exists. ***\n", arg[1]);
                        write(connfd, error, strlen(error));
                    }
                    // name is not empty
                    else if(arg[1] != NULL)    
                    {
                        char message[MAXLINE];
                        snprintf(message, sizeof(message), "*** User from %s/%d is named '%s'. ***\n", clients[id].ip, clients[id].port, arg[1]);
                        // TODO: need semaphore
                        broadcast(clients, message);
                        strcpy(clients[id].name, arg[1]);
                    }
                }
                
                // command "tell <id> <message>"
                else if(!strcmp(arg[0], "tell")) 
                {
                    char message[MAXLINE];
                    char temp[MAXLINE] = "";
                    char error[MAXLINE];
                    int targetID = atoi(arg[1]);
                    
                    if(targetID <= 0)
                    {
                        char error[] = "*** Error: Invalid ID. ***\n";
                        write(connfd, error, strlen(error));
                    }    
                    else if(!clients[targetID].exist)
                    {
                        snprintf(error, sizeof(error), "*** Error: user #%d does not exist yet. ***\n", id);
                        write(connfd, error, strlen(error));
                    }
                    else 
                    {
                        pch = strtok(text, " ");   
                        pch = strtok(NULL, " ");   
                        if((pch = strtok(NULL, "\r\n")))    
                            strcpy(temp, pch);
                        snprintf(message, sizeof(message), "*** %s told you ***: %s\n", clients[id].name, temp);
                        strcpy(sigusr_message, message);
                        kill(clients[targetID].pid, SIGUSR1);
                    }
                }
                
                // command "yell <message>"
                else if(!strcmp(arg[0], "yell")) 
                {
                    char message[MAXLINE];
                    char temp[MAXLINE] = "";
                    
                    pch = strtok(text, " ");  
                    if((pch = strtok(NULL, "\r\n")))   
                        strcpy(temp, pch);
                    snprintf(message, sizeof(message), "*** %s yelled ***: %s\n", clients[id].name, temp);
                    broadcast(clients, message);
                }
                
                else 
                for(int i = 0; i < argN; i++)
                {
                    if(arg[i] == NULL)    // last command
                    {
                        char output[MAXLINE];
                        char cmd[MAXLINE] = "";

                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if(command[0] != NULL)
                            strcpy(cmd, command[0]);
                        
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;
 
                        if( execute(command, readfd, stdout, stderr) < 0 )
                        {
                            error_cmd_handler(connfd, pipeCounter, cmd, arg[0], readfd, fd);
                            break;
                        }                        
                        
                        if(readfd != 0)    
                            close(readfd);
                        
                        read_write(stderr, connfd);
                        read_write(stdout, connfd);
                    }
                    else if(!strcmp(arg[i], ">"))    // redirect stdout to file
                    {
                        char output[MAXLINE];
                        char cmd[MAXLINE] = "";

                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if(command[0] != NULL)
                            strcpy(cmd, command[0]);
                        
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;
                        
                        if( execute(command, readfd, stdout, stderr) < 0 )
                        {
                            error_cmd_handler(connfd, pipeCounter, cmd, arg[0], readfd, fd);
                            break;
                        }                        

                        if(readfd != 0)    
                            close(readfd);
                        
                        FILE *fp = fopen(arg[i+1], "w");
                        
                        if(fp) {           
                            read_write(stdout, fileno(fp));
                            fclose(fp);
                        }
                        else {
                            char error[MAXLINE];
                            snprintf(error, sizeof(error), "Invalid file name: [%s].\n", arg[i+1]);
                            write(connfd, error, strlen(error));
                        }
                        
                        read_write(stderr, connfd);
                        
                        break;
                    }
                    else if(!strcmp(arg[i], "|"))    // pipe stdout to next command 
                    { 
                        char output[MAXLINE];
                        char cmd[MAXLINE] = "";

                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if(command[0] != NULL)
                            strcpy(cmd, command[0]);
                        
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;
                           
                        if( execute(command, readfd, stdout, stderr) < 0 )
                        {
                            error_cmd_handler(connfd, pipeCounter, cmd, arg[0], readfd, fd);
                            break;
                        }                        

                        if(readfd != 0)    
                            close(readfd);
                        
                        readfd = stdout; 
                        
                        read_write(stderr, connfd);
                    }
                    else if(arg[i][0] == '|')    // pipe stdout to next N line command 
                    {
                        char output[MAXLINE];
                        char cmd[MAXLINE] = "";
                        int number = 0;

                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if(command[0] != NULL)
                            strcpy(cmd, command[0]);
                        
                        if( (fd = checkCounter(pipeCounter)) != -1 )
                            readfd = fd;

                        if( execute(command, readfd, stdout, stderr) < 0 )
                        {
                            error_cmd_handler(connfd, pipeCounter, cmd, arg[0], readfd, fd);
                            break;
                        }

                        if(readfd != 0)    
                            close(readfd);
                        
                        if((pch = strtok(arg[i], "|")))
                            if( (number = atoi(pch)) > 0 )
                                pipeCounter.push_back(make_pair(number, stdout));
                        
                        if(arg[i+1] != NULL && arg[i+1][0] == '!') {
                            if((pch = strtok(arg[i+1], "!")))
                                if( (number = atoi(pch)) > 0 )
                                    pipeCounter.push_back(make_pair(number, stderr)); 
                        }
                        else 
                            read_write(stderr, connfd);     
                             
                        break;          
                    }
                    else if(arg[i][0] == '!')    // pipe stderr to next N line command 
                    {
                        char output[MAXLINE];
                        char cmd[MAXLINE] = "";
                        int number = 0;

                        command[commandN] = NULL;
                        commandN = 0;
                        
                        if(command[0] != NULL)
                            strcpy(cmd, command[0]);
                        
                        if( (fd = checkCounter(pipeCounter)) != -1)
                            readfd = fd;

                        if( execute(command, readfd, stdout, stderr) < 0 )
                        {
                            error_cmd_handler(connfd, pipeCounter, cmd, arg[0], readfd, fd);
                            break;
                        }                        

                        if(readfd != 0)    
                            close(readfd);
                        
                        if((pch = strtok(arg[i], "!")))
                            if( (number = atoi(pch)) > 0 )
                                pipeCounter.push_back(make_pair(number, stderr));
                        
                        if(arg[i+1] != NULL && arg[i+1][0] == '|') {
                            if((pch = strtok(arg[i+1], "|"))) 
                                if( (number = atoi(pch)) > 0 )
                                    pipeCounter.push_back(make_pair(number, stdout));
                        }
                        else 
                            read_write(stdout, connfd);
                             
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

