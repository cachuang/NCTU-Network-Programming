#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>
#include <vector>
#include "readline.h"

#define MAXLINE 15000
#define MAX_CMD_NUM 15000
#define MAX_CMD_LEN 300
#define MAX_CLIENT_NUM 30
#define MAX_PUBLIC_PIPE 100
#define SIGUSR_SEM "SIGUSR semaphore"
#define PIPE_SEM "public pipe semaphore"
#define PIPEFIFO "../npFIFO"

using namespace std;

extern char **environ;
int sigusrfd, client_shm_id, sigusr_shm_id, pipe_shm_id;
char *sigusr_message;
struct clientData *clients;
struct publicPipeData *publicPipe;
sem_t *sigusr_sem, *pipe_sem;

struct clientData
{
    int id, pid, port;
    char ip[MAXLINE], name[MAXLINE];
    bool exist;
};

struct publicPipeData
{
    char name[MAXLINE];
    bool exist;
};

void sigint_handler(int signal)
{
    shmdt(clients);
    shmdt(sigusr_message);
    shmdt(publicPipe);
    shmctl(client_shm_id, IPC_RMID, NULL);
    shmctl(sigusr_shm_id, IPC_RMID, NULL);
    shmctl(pipe_shm_id, IPC_RMID, NULL);
    
    sem_close(sigusr_sem);
    sem_close(pipe_sem);
    sem_unlink(SIGUSR_SEM);
    sem_unlink(PIPE_SEM);
    
    exit(1);
}

void sigusr_handler(int signal)
{
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
    // protect share memory sigusr_message
    sem_wait(sigusr_sem);
    
    strcpy(sigusr_message, message);
    
    for(int i = 1; i <= MAX_CLIENT_NUM; i++)
        if(clients[i].exist)
            kill(clients[i].pid, SIGUSR1);
            
    sem_post(sigusr_sem);
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
        perror("socket error");
        exit(errno);
    }

    if( bind(listenfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("bind error");
        exit(errno);
    }

    if( listen(listenfd, 20) < 0 ) {
        perror("listen error");
        exit(errno);
    }
    
    // create share memory and semaphore
    if( (client_shm_id = shmget(IPC_PRIVATE, (MAX_CLIENT_NUM + 1) * sizeof(struct clientData), IPC_CREAT | IPC_EXCL | 0600)) < 0 ) {
        perror("[shmget error]");
        shmctl(client_shm_id, IPC_RMID, NULL);
        exit(errno);    
    }
    if( (sigusr_shm_id = shmget(IPC_PRIVATE, MAXLINE * sizeof(char), IPC_CREAT | IPC_EXCL | 0600)) < 0 ) {
        perror("[shmget error]");
        shmctl(sigusr_shm_id, IPC_RMID, NULL);
        exit(errno);    
    }
    if( (pipe_shm_id = shmget(IPC_PRIVATE, MAX_PUBLIC_PIPE * sizeof(struct publicPipeData), IPC_CREAT | IPC_EXCL | 0600)) < 0 ) {
        perror("[shmget error]");
        shmctl(pipe_shm_id, IPC_RMID, NULL);
        exit(errno);    
    }  
    if( (sigusr_sem = sem_open (SIGUSR_SEM, O_CREAT | O_EXCL, 0600, 1)) == SEM_FAILED ) {
        perror("[sem_open error]");
        sem_unlink(SIGUSR_SEM);
        exit(errno);
    }
    if( (pipe_sem = sem_open (PIPE_SEM, O_CREAT | O_EXCL, 0600, 1)) == SEM_FAILED ) {
        perror("[sem_open error]");
        sem_unlink(PIPE_SEM);
        exit(errno);
    }
    
    clients = (struct clientData *) shmat(client_shm_id, NULL, 0);
    publicPipe = (struct publicPipeData *) shmat(pipe_shm_id, NULL, 0);
    
    for(int i = 0; i <= MAX_CLIENT_NUM; i++)
        clients[i].exist = false;
        
    for(int i = 0; i < MAX_PUBLIC_PIPE; i++)
        publicPipe[i].exist = false;
 
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);

    while(1)
    {
        socklen_t len;
        struct sockaddr_in clientaddr;
        char ip[INET_ADDRSTRLEN];
        int id, port;
        
        len = sizeof(clientaddr);
        connfd = accept(listenfd, (struct sockaddr *) &clientaddr, &len);
        if(errno == EINTR) continue;
        
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
            char enter[MAXLINE];
            vector < pair<int, int > > pipeCounter;
                        
            close(listenfd);
            
            sigusrfd = connfd;
            clients[id].pid = getpid();
            
            signal(SIGCHLD, SIG_DFL);
            signal(SIGUSR1, sigusr_handler);
            
            // attach share memory
            clients = (struct clientData *) shmat(client_shm_id, NULL, 0);
            sigusr_message = (char *) shmat(sigusr_shm_id, NULL, 0);
            publicPipe = (struct publicPipeData *) shmat(pipe_shm_id, NULL, 0);
            
            // change directory to ~/ras
            strcat(home, getenv("HOME"));
            strcat(home, "/ras");
            chdir(home); 
            
            // clear environment variables and set PATH
            clearenv();
            setenv("PATH", "bin:.", 1);            
 
            write(connfd, welcome, strlen(welcome));
            snprintf(enter, sizeof(enter), "*** User '(no name)' entered from %s/%d. ***\n", clients[id].ip, clients[id].port);    
            broadcast(clients, enter);       
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
                    
                // command "exit"
                if(!strcmp(arg[0], "exit"))
                    break;
                    
                // command "setenv"    
                else if(!strcmp(arg[0], "setenv"))    
                    setenv(arg[1], arg[2], 1);
                
                // command "printenv"
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
                        char output[MAXLINE] = "";
                        char temp[MAXLINE];
                        for (int i = 0; environ[i] != NULL; i++) {
                            snprintf(temp, sizeof(temp), "%s\n", environ[i]);
                            strcat(output, temp);
                        } 
                        write(connfd, output, strlen(output));
                    }    
                }
                
                // command "who"
                else if(!strcmp(arg[0], "who"))    
                {
                    char message[MAXLINE] = "";
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
                    for(int i = 1; i <= MAX_CLIENT_NUM; i++)
                        if(clients[i].exist && arg[1] != NULL && !strcmp(clients[i].name, arg[1]))                {
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
                        strcpy(clients[id].name, arg[1]);
                        broadcast(clients, message);
                    }
                }
                
                // command "tell <id> <message>"
                else if(!strcmp(arg[0], "tell")) 
                {
                    int targetID = atoi(arg[1]);
                    
                    if(targetID <= 0)
                    {
                        char error[] = "*** Error: Invalid ID. ***\n";
                        write(connfd, error, strlen(error));
                    }    
                    else if(!clients[targetID].exist)
                    {
                        char error[MAXLINE];
                        snprintf(error, sizeof(error), "*** Error: user #%d does not exist yet. ***\n", id);
                        write(connfd, error, strlen(error));
                    }
                    else 
                    {
                        char message[MAXLINE];
                        char temp[MAXLINE] = "";
                        
                        pch = strtok(text, " ");   
                        pch = strtok(NULL, " ");   
                        if((pch = strtok(NULL, "\r\n")))    
                            strcpy(temp, pch);
                        snprintf(message, sizeof(message), "*** %s told you ***: %s\n", clients[id].name, temp);
                        sem_wait(sigusr_sem);
                        strcpy(sigusr_message, message);
                        kill(clients[targetID].pid, SIGUSR1);
                        sem_post(sigusr_sem);
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
                    // last command
                    if(arg[i] == NULL)    
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
                    
                    // redirect stdout to file
                    else if(!strcmp(arg[i], ">"))    
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
                    
                    // pipe stdout to next command 
                    else if(!strcmp(arg[i], "|"))    
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
                    
                    // pipe stdout to next N line command 
                    else if(arg[i][0] == '|')    
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
                    
                    // pipe stderr to next N line command 
                    else if(arg[i][0] == '!')    
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
                    
                    // pipe to public pipe
                    else if(arg[i][0] == '>')    
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
                            
                        if(arg[i+1] != NULL && arg[i+1][0] == '<')     
                            if((pch = strtok(arg[i+1], "<")))
                                if( (number = atoi(pch)) > 0 )
                                {
                                    char message[MAXLINE];
                                    
                                    sem_wait(pipe_sem);
                                    if(publicPipe[number].exist)
                                    {
                                        publicPipe[number].exist = false;
                                        sem_post(pipe_sem);
              
                                        readfd = open(publicPipe[number].name, O_RDONLY);
                                        
                                        pch = strtok(text, "\r\n");
                                        snprintf(message, sizeof(message), "*** %s (#%d) just received via '%s' ***\n", clients[id].name, clients[id].id, pch);
                                        broadcast(clients, message);
                                    }
                                    else 
                                    {
                                        sem_post(pipe_sem);
                                        char error[MAXLINE];
                                        snprintf(error, sizeof(error), "*** Error: public pipe #%d does not exist yet. ***\n", number);
                                        write(connfd, error, strlen(error));    
                                    }
                                }    

                        if( execute(command, readfd, stdout, stderr) < 0 )
                        {
                            error_cmd_handler(connfd, pipeCounter, cmd, arg[0], readfd, fd);
                            break;
                        }

                        if(readfd != 0)    
                            close(readfd);
                        
                        if((pch = strtok(arg[i], ">")))
                            if( (number = atoi(pch)) > 0 )
                            {
                                char message[MAXLINE];
                                
                                sem_wait(pipe_sem);
                                if(!publicPipe[number].exist)
                                {
                                    publicPipe[number].exist = true;
                                    sem_post(pipe_sem);
                                    
                                    char fname[MAXLINE];
                                    snprintf(fname, sizeof(fname), "%s%d", PIPEFIFO, number);                          
                                    strcpy(publicPipe[number].name, fname);
                                    
                                    // open FIFO in writeonly will block until another process
                                    // open FIFO for read, so fork a child process to wait  
                                    // use double fork to prevent zombie process                        
                                    if( fork() == 0 )
                                    {
                                        if ( fork() == 0 )
                                        {
                                            mkfifo(fname, 0600);
                                            int fifofd = open(fname, O_WRONLY);
                                            
                                            read_write(stderr, fifofd);
                                            read_write(stdout, fifofd);
                                            close(fifofd);
                                            unlink(fname);
                                            
                                            exit(0);
                                        }
                                        else 
                                            exit(0);
                                    }
                                    else 
                                    {
                                        wait(NULL);
                                        pch = strtok(text, "\r\n");
                                        snprintf(message, sizeof(message), "*** %s (#%d) just piped '%s' ***\n", clients[id].name, clients[id].id, pch);
                                        broadcast(clients, message);
                                    }
                                }
                                else
                                {
                                    sem_post(pipe_sem);
                                    char error[MAXLINE];
                                    snprintf(error, sizeof(error), "*** Error: public pipe #%d already exists. ***\n", number);
                                    write(connfd, error, strlen(error));
                                }
                            }
                             
                        break;          
                    }
                    
                    // get input from public pipe
                    else if(arg[i][0] == '<')    
                    {                        
                        char message[MAXLINE];
                        int number = 0;
                        
                        pch = strtok(text, "\r\n");
                        snprintf(message, sizeof(message), "*** %s (#%d) just received via '%s' ***\n", clients[id].name, clients[id].id, pch);

                        if((pch = strtok(arg[i], "<")))
                            if( (number = atoi(pch)) > 0 )
                            {  
                                sem_wait(pipe_sem);
                                if(publicPipe[number].exist)
                                {
                                    publicPipe[number].exist = false;
                                    sem_post(pipe_sem);
          
                                    readfd = open(publicPipe[number].name, O_RDONLY);

                                    if(arg[i+1] == NULL)
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
                                        
                                        broadcast(clients, message);
                                        
                                        break;
                                    }
                                    else
                                        broadcast(clients, message);
                                }
                                else 
                                {
                                    sem_post(pipe_sem);
                                    char error[MAXLINE];
                                    snprintf(error, sizeof(error), "*** Error: public pipe #%d does not exist yet. ***\n", number);
                                    write(connfd, error, strlen(error));
                                }
                            }
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
                // client disconnect
                char leave[MAXLINE];
                snprintf(leave, sizeof(leave), "*** User '%s' left. ***\n", clients[id].name);
                broadcast(clients, leave); 
                clients[id].exist = false;
                close(connfd);
                shmdt(clients);
                shmdt(sigusr_message);
                shmdt(publicPipe);
                sem_close(sigusr_sem);
                sem_close(pipe_sem);

                exit(0);
        }
            close(connfd);
    }

    return 0;
}

