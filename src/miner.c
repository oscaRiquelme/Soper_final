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

#include "miner.h"

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819

typedef struct _workerInfo {
    int numWorkers; /*argv[1]*/
    int numThread; /*Identifier comprehended betweeen 0 and argv[2] - 1*/
    int solution; /*Variable in which de solution will be returned if found*/
    Block *currentBlock;
    NetData *net;
} WorkerInfo;

static volatile int solution_found = 0;
static volatile int sigusr2_received = 0;

/************************PRIVATE*HEADERS****************************/
NetData* open_netShmemory(short * isFirst);
NetData* map_netShmemory(int fd);
sigset_t proc_handlers();
Block* open_blockChainMemory();
Block* map_blockShmemory(int fd);
void* worker(void* args);
void sigusr2_handler(int signal);
int signUp(NetData * net);


long int simple_hash(long int number) {
    long int result = (number * BIG_X + BIG_Y) % PRIME;
    return result;
}

void print_blocks(Block *plast_block, int num_wallets) {
    Block *block = NULL;
    int i, j;

    for(i = 0, block = plast_block; block != NULL; block = block->prev, i++) {
        printf("Block number: %d; Target: %ld;    Solution: %ld\n", block->id, block->target, block->solution);
        for(j = 0; j < num_wallets; j++) {
            printf("%d: %d;         ", j, block->wallets[j]);
        }
        printf("\n\n\n");
    }
    printf("A total of %d blocks were printed\n", i);
}

NetData* open_netShmemory(short * isFirst){
    
    int fd;
    NetData *net;
    if((fd = shm_open(SHM_NAME_NET, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1){
        if(errno == EEXIST){
            fd = shm_open(SHM_NAME_NET, O_RDWR, 0);
            if(fd == -1){
                perror("open_shmemory(existing)");
                return NULL;
            }
            else{
                net = map_netShmemory(fd);
                if(!net){
                    close(fd);
                    fprintf(stderr, "Fracase mapeando la red manito\n");
                    return NULL;
                }
                close(fd);
                return net;
            }
        }
        else{
            perror("open_shmemory(non_existing)");
            return NULL;
        }
    }
    else{
        if(ftruncate(fd, sizeof(NetData)) == -1){
            perror("ftruncate");
            close(fd);
            return NULL;
        }
        net = map_netShmemory(fd);
        if(!net){
            fprintf(stderr, "Fracase mapeando la red manito\n");
            close(fd);
            return NULL;
        }
        close(fd);
        if(sem_init(&net->netShMemory_mutex, 1, 1)){
            perror("net SemInit");
            munmap(net, sizeof(NetData));
            return NULL;
        }
        if(isFirst) *isFirst = 1;
        return net;
    }
}

NetData* map_netShmemory(int fd){

    NetData* data = NULL;

    if ((data = (NetData*)mmap(NULL, sizeof(NetData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return data;
}

/*TODO: exit failures no me gusta, quizas una variable por referencia para cde*/
sigset_t proc_handlers(){

    struct sigaction action;
    sigset_t mask, oldmask;

    action.sa_handler = sigusr2_handler;
    sigemptyset(&(action.sa_mask));
    action.sa_flags = 0;

    if(sigaction(SIGUSR2, &action, NULL) < 0){
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    sigemptyset(&mask);
    if(sigaddset(&mask, SIGUSR2) < 0){
        perror("Sigaddset sigusr2");
        exit(EXIT_FAILURE);
    }
    sigprocmask (SIG_BLOCK, &mask, &oldmask);
    
    return mask;
}

Block* open_blockChainMemory(){

    Block * shBlock;

    int fd;
    int i;
    if((fd = shm_open(SHM_NAME_BLOCK, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1){
        if(errno == EEXIST){
            fd = shm_open(SHM_NAME_BLOCK, O_RDWR, 0);
            if(fd == -1){
                perror("open_shmemory(existing)");
                return NULL;
            }
            else{
                shBlock = map_blockShmemory(fd);
                if(!shBlock){
                    fprintf(stderr, "Fracase mapeando el bloque compartido manito\n");
                    return NULL;
                }
                close(fd);
                return shBlock;
            }
        }
        else{
            perror("open_shmemory(non_existing)");
            return NULL;
        }
    }
    else{
        if(ftruncate(fd, sizeof(Block)) == -1){
            perror("ftruncate");
            shm_unlink(SHM_NAME_NET);
            close(fd);
            return NULL;
        }
        shBlock = map_blockShmemory(fd);
        if(!shBlock){
            fprintf(stderr, "Fracase mapeando el bloque compartido manito(existing)\n");
            return NULL;
        }
        close(fd);
        if(sem_init(&shBlock->blockShMemory_mutex, 1, 1) == -1){
            perror("block semInit w_mutex");
            munmap(shBlock, sizeof(Block));
            return NULL;
        }
        if(sem_init(&shBlock->solution_mutex, 1, 1) == -1){
            perror("block semInit solution");
            munmap(shBlock, sizeof(Block));
            return NULL;
        }
        for(i = 0; i < MAX_MINERS; i++){ /*If  the blockchain is the first blockchain of the chain*/
            shBlock->wallets[i] = 0; 
        }
        return shBlock;
    }
}

Block* map_blockShmemory(int fd){

    Block* data = NULL;

    if ((data = (Block*)mmap(NULL, sizeof(Block), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return data;
}

void* worker(void* args){
        
        int i;
        int intervalSize;
        int iniInterval;
        WorkerInfo *pow = (WorkerInfo*)args;
        /*Gets the interval in which the worker will be working*/
        intervalSize = PRIME/pow->numWorkers;
        intervalSize++;
        iniInterval = intervalSize * pow->numThread;

        
        for(i = iniInterval; i < iniInterval+intervalSize && solution_found == 0; i++){
            if(pow->currentBlock->target == simple_hash(i)){
                sem_wait(&pow->currentBlock->solution_mutex);
                if(pow->currentBlock->solution_found == 0){
                    pow->currentBlock->solution_found = 1;
                    solution_found = 1;
                    pow->solution = i;
                    pow->currentBlock->solution = i;

                    for(i = 0; i < pow->net->total_miners; i++){
                        printf("Mandando señal a %d\n", pow->net->miners_pid[i]);
                        if(getpid() != pow->net->miners_pid[i])
                            if(kill(pow->net->miners_pid[i], SIGUSR2)){
                                perror("kill_sigusr2");
                                /*PROBABLEMENTE TENGA QUE LIBERAR MEMORIA Y CERRAR COSAS*/
                        }
                    }
                }
                sem_post(&pow->currentBlock->solution_mutex);
                pthread_exit(pow);
            }
        }
        pthread_exit(NULL);
}

void sigusr2_handler(int signal){
    sigusr2_received = 1;
}

int signUp(NetData * net){
    if(net->total_miners == MAX_MINERS){
        return ERR;
    }

    sem_wait(&net->netShMemory_mutex);
    net->miners_pid[net->total_miners] = getpid();
    (net->total_miners)++;
    sem_post(&net->netShMemory_mutex);

    return OK;
}

int main(int argc, char *argv[]) {
    long int i;
    int solution, roundCounter = 0, maxRounds;
    pthread_t workers[MAX_WORKERS];
    short numWorkers, isFirst;
    WorkerInfo info[MAX_WORKERS];
    NetData *net; 
    Block *shBlock;



    
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <NUM_WORKERS> <MAX_ROUNDS>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    numWorkers = atoi(argv[1]);
    if(numWorkers > 10 || numWorkers < 1){
        fprintf(stderr, "Manito ponme al menos 1 trabajador y no mas de 10\n");
        exit(EXIT_FAILURE);
    }
    maxRounds = atoi(argv[2]);
    if(maxRounds <= 0){
        fprintf(stderr, "Manito como va a hacer cero rondas o menos???\n");
        exit(EXIT_FAILURE);
    }

    /*Procs signal handlers*/
    proc_handlers();

    /*opens and maps netShememory*/
    net = open_netShmemory(&isFirst);
    if(!net){
        fprintf(stderr, "Fracase abriendo o creando la red manito\n");
        exit(EXIT_FAILURE);
    }
    /*opens and maps the blockchain*/
    shBlock = open_blockChainMemory();
    if(!shBlock){
        fprintf(stderr, "Fracase abriendo o creando la red manito\n");
        munmap(net, sizeof(NetData));
        exit(EXIT_FAILURE);
    }
    
    printf("Registrando minero %d en la red...\n", getpid());
    
    if(signUp(net) == ERR){
        fprintf(stderr, "Failed to register miner %d\n", getpid());
        munmap(net, sizeof(NetData));
        munmap(shBlock, sizeof(Block));
    }
    printf("Imprimiendo lista...\n");
    for(i = 0; i < net->total_miners; i++){
        printf("%d\n", net->miners_pid[i]);
    }
    if(isFirst){
        printf("El minero %d es el primero\n", getpid());
        srand(time(NULL));
        sem_wait(&shBlock->blockShMemory_mutex);
        shBlock->id = 0;
        shBlock->prev = NULL;
        shBlock->target = rand()%PRIME;
        sem_post(&shBlock->blockShMemory_mutex);  
    }
    
    printf("El minero %d empieza sus rondas\n", getpid());
    while(roundCounter < maxRounds){
        /*se prepara una ronda*/

        printf("Target: %ld\n", shBlock->target);
        /*carga la info de cada trabajador*/
        /*lanza el número de trabajadores especificados*/
        sigusr2_received = 0;
        solution_found = 0;
        shBlock->solution_found = 0;
        for (i = 0; i < numWorkers; i++){
            info[i].numWorkers = numWorkers;
            info[i].numThread = i;
            info[i].currentBlock = shBlock;
            info[i].net = net;
            info[i].solution = -1;
            printf("Lanzando hilo... %ld\n", i+1);
            if(pthread_create(&workers[i], NULL, worker, &info[i]) != 0){
                fprintf(stderr, "Manito fracase creando hilos...\n");
                exit(EXIT_FAILURE);
            }
        }
        
        printf("Entrando en espera activa...\n");
        while(!sigusr2_received && !solution_found);
        
        for(i = 0; i < numWorkers; i++){
            pthread_join(workers[1], NULL);
            if(info[i].solution != -1 ) solution = info[i].solution;
        }

        roundCounter++;
        if(solution_found){
            fprintf(stdout, "Solucion: %d\n", solution);
        }
    }

    fprintf(stdout, "Max rounds reached manito\n");
    exit(EXIT_SUCCESS); 
        
    
}


