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
#include <string.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <time.h>
#include <mqueue.h>

#include "miner.h"
#include "monitor.h"

#define MONITOR_SEM_NAME "/monitorSem"
#define SECONDS 5

static volatile int sigint_received = 0;
static volatile int sigusr1_received = 0;
static volatile int sigalrm_received = 0;
Block blockchain;
int miners;
char file[30];


void copyBlock(Block * dest, Block * src, int miners){

    Block * aux = dest;

    while(aux->next!=NULL) aux = aux->next;
    printf("4\n");
    int j;
    aux->id = src->id;
    aux->target = src->target;
    aux->solution = src->solution;
    aux->is_valid = src->is_valid;
    for(j = 0; j < miners; j++) aux->wallets[j] = src->wallets[j];
}

void print_blockchain(Block* block, int totalMiners) {
    int j;
    Block * aux;
    FILE *f;
    f = fopen(file, "w");
    if(!f) return;

    fprintf(f, "\nPRINTING BLOCKCHAIN\n");
    for(aux = block->next; aux != NULL; aux = aux->next) {
        fprintf(f, "Block number: %d; Target: %ld;    Solution: %ld\n", aux->id, aux->target, aux->solution);
        for(j = 0; j < totalMiners; j++) {
            fprintf(f, "%d: %d;         ", j+1, aux->wallets[j]);
        }
        fprintf(f, "\n\n\n");
    }

    fclose(f);
}

void sig_handler(int signal){
    if(signal == SIGUSR1) sigusr1_received = 1;

    if(signal == SIGINT) sigint_received = 1;
    if(signal == SIGALRM){
        alarm(SECONDS);
        print_blockchain(&blockchain, miners);
    }
}

void proc_handlers(){

    struct sigaction action;

    action.sa_handler = sig_handler;
    sigemptyset(&(action.sa_mask));
    action.sa_flags = 0;

    if(sigaction(SIGINT, &action, NULL) < 0){
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if(sigaction(SIGUSR1, &action, NULL) < 0){
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if(sigaction(SIGALRM, &action, NULL) < 0){
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

}

void blockSig(int sig){
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

void unblockSig(int sig){
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

NetData* map_netShmemory(int fd){

    NetData* data = NULL;

    if ((data = (NetData*)mmap(NULL, sizeof(NetData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return data;
}

NetData* setMonitorPidInSharedMemory(){
    int fd;
    NetData *net;
    if((fd = shm_open(SHM_NAME_NET, O_RDWR, 0)) == -1){
        perror("shm_open");
        return NULL;
    }
    else{
        net = map_netShmemory(fd);
        close(fd);
        if(!net){
            fprintf(stderr, "Failed to map the net\n");
            return NULL;
        }
        if(net->monitor_pid != NO_MONITOR){
            fprintf(stdout, "Already a monitor running\n");
            return NULL;
        } 
        sem_wait(&net->netShMemory_mutex);
        net->monitor_pid = getpid();
        sem_post(&net->netShMemory_mutex);
        return net;
    }
}


int main(int argc, char* argv[]){

    pid_t pid;
    int pipe_fd[2];
    mqd_t monitorQueue;
    Block block;
    sem_t *sem = NULL;
    ssize_t size_written , total_size_written;
    size_t target_size ;
    ssize_t nbytes ;
    NetData *net;
    Block *bc_pointer;
    int i = 0, j;
    int id_found;
    Block buffer[MAX_BLOCKS_MONITOR]; 

    struct mq_attr attributes = {
        .mq_flags = 0,
        .mq_maxmsg = MAX_BLOCKS_MONITOR,
        .mq_curmsgs = 0,
        .mq_msgsize = sizeof(Block)
    };

    if(argc < 2){
        printf("Wrong arguments passed. Usage: %s <fileName>", argv[0]);
        exit(EXIT_FAILURE);
    }
    printf("Proccing handlers...\n");
    proc_handlers();
    strcpy(file, argv[1]);
    printf("Opening Queue...\n");
    monitorQueue = mq_open(MQ_MONITOR, O_CREAT | O_RDONLY , S_IRUSR | S_IWUSR, &attributes);
    if(monitorQueue == (mqd_t)-1) {
        perror("monitorQueue");
        exit(EXIT_FAILURE);
    }

    printf("Opening semaphore...\n");
    if((sem = sem_open(MONITOR_SEM_NAME, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 0)) == SEM_FAILED){
        perror("semOpen");
        mq_close(monitorQueue);
        exit(EXIT_FAILURE);
    }
    sem_unlink(MONITOR_SEM_NAME);

    printf("Creating pipe...\n");
    if(pipe(pipe_fd)){
        perror("Pipe");
        mq_close(monitorQueue);
        sem_close(sem);
        exit(EXIT_FAILURE);
    }
    printf("Setting pid as the monitor pid in shared memory...\n");
    net = setMonitorPidInSharedMemory();
    if(!net){
        printf("Couldn't set the pid for the monitor in shared memory, exiting...\n");    
        mq_close(monitorQueue);
        sem_close(sem);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        exit(EXIT_FAILURE);
    }
    printf("Monitor pid set successfully: %d\n", net->monitor_pid);
    sleep(1);

    printf("Creating child process...\n");
    pid = fork();
    if(pid < 0){
        perror("fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        mq_close(monitorQueue);
        sem_close(sem);
        munmap(net, sizeof(NetData));
        exit(EXIT_FAILURE);
    }


    else if(pid){ /*Padre*/
        close(pipe_fd[0]);
        printf("%d\n", pid);
        for(j = 0; j < MAX_BLOCKS_MONITOR; j++) buffer[j].id = -1;
        while(!sigint_received){
            blockSig(SIGINT);
            if(mq_receive(monitorQueue, (char*)&block, sizeof(Block), NULL) == -1){
                perror("receive");
            } /*cde*/
            id_found = 0;
            for(j = 0; j < MAX_BLOCKS_MONITOR && id_found == 0; j++){
                if(block.id == buffer[j].id) id_found = 1;
            }

            if(!id_found){
                printf("New block %d received with solution %ld for target %ld\n", block.id, block.solution, block.target);
                total_size_written = 0;
                target_size = sizeof(Block);
                do {
                    size_written = write (pipe_fd[1], &block + total_size_written, target_size - total_size_written ) ;
                    if ( size_written == -1) {
                        perror("write");
                        exit(EXIT_FAILURE);
                    }
                    total_size_written += size_written ;
                } while(total_size_written != target_size);
                sem_post(sem); /*Informa de que escribio en la tuberia*/ 
            }
            else{
                if(buffer[j-1].solution == block.solution) 
                    printf("Verified block %d with solution %ld for target %ld\n", block.id, block.solution, block.target);
                else
                    printf("Error in block %d with solution %ld for target %ld\n", block.id, block.solution, block.target);
            }
            buffer[i] = block;
            i++;
            i = i % MAX_BLOCKS_MONITOR;
            unblockSig(SIGINT); 
        }
        printf("Signalling child to finish...\n");
        kill(pid, SIGUSR1);
        wait(NULL);
        printf("Finishing monitor...\n");
        net->monitor_pid = NO_MONITOR;
        close(pipe_fd[1]);
        mq_close(monitorQueue);
        sem_close(sem);
        exit(EXIT_SUCCESS);

    }
    else{ /*Hijo*/
        blockSig(SIGINT);
        close(pipe_fd[1]);
        mq_close(monitorQueue);
        blockchain.next = NULL;
        blockchain.prev = NULL;
        bc_pointer = &blockchain;
        alarm(SECONDS);

        while(!sigusr1_received){
             /*Espera a tener algo escrito en la tuberia*/

            blockSig(SIGALRM);
            miners = net->total_miners;
            sem_wait(sem);
            nbytes = 0;
            
            do{
                if(!sigusr1_received) nbytes += read(pipe_fd[0], &block + nbytes, sizeof (Block)-nbytes) ;
                if ( nbytes == -1) {
                    perror("read");
                    exit(EXIT_FAILURE);
                }
            }while(nbytes != sizeof(Block) && !sigusr1_received);
            
            bc_pointer->next = (Block*)malloc(sizeof(Block));
            bc_pointer->next->next = NULL;
            copyBlock(&blockchain, &block, net->total_miners);
            bc_pointer = bc_pointer->next;
            unblockSig(SIGALRM);

                
        }
        close(pipe_fd[0]);
        munmap(net, sizeof(NetData));
        sem_close(sem);
        exit(EXIT_SUCCESS);
    }


}


