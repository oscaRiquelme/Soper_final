#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <time.h>
#include <mqueue.h>

#include "miner.h"


int main(){

    pid_t pid;
    int pipe_fd[2];


    if(pipe(pipe_fd)){
        perror("Pipe");
        exit(EXIT_FAILURE);
    }

    pid = fork();
    if(pid < 0){
        perror("fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        exit(EXIT_FAILURE);
    }

    else if(pid){ /*Padre*/
        close(pipe_fd[0]);


    }
    else{ /*Hijo*/
        close(pipe_fd[1]);
    

    }


}


