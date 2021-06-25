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
#include <string.h>

#include "miner.h"
#include "monitor.h"

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819

#define MAX_BLOCKS 200

typedef struct _workerInfo {
    int numWorkers; /*argv[1]*/
    int numThread; /*Identifier comprehended betweeen 0 and argv[2] - 1*/
    Block *currentBlock;
    NetData *net;
} WorkerInfo;

static volatile int sigusr2_received = 0;
static volatile int sigusr1_received = 0;
static volatile int sigint_received = 0;

/************************PRIVATE*HEADERS****************************/
NetData* open_netShmemory(short * isFirst);
NetData* map_netShmemory(int fd);
sigset_t proc_handlers();
Block* open_sharedBlockMemory();
Block* map_blockShmemory(int fd);
void* worker(void* args);
void sigusr_handler(int signal);
int signUp(NetData * net);
Block * updateBlock(Block *shBlock);
/******************************************************************/


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
                close(fd);
                if(!net){
                    fprintf(stderr, "Failed to map the net\n");
                    return NULL;
                }
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
        close(fd);
        if(!net){
            fprintf(stderr, "Failed to map the net\n");
            return NULL;
        }
        if(sem_init(&net->netShMemory_mutex, 1, 1)){
            perror("net SemInit");
            munmap(net, sizeof(NetData));
            return NULL;
        }
        if(sem_init(&net->blockShMemory_mutex, 1, 1) == -1){
            perror("block semInit w_mutex");
            munmap(net, sizeof(Block));
            return NULL;
        }
        if(sem_init(&net->solution_mutex, 1, 1) == -1){
            perror("block semInit solution");
            munmap(net, sizeof(NetData));
            return NULL;
        }
        sem_wait(&net->netShMemory_mutex);
        net->roundInProgress = 0;
        net->total_miners = 0;
        net->numWaiting_list = 0;
        net->monitor_pid = NO_MONITOR;
        sem_post(&net->netShMemory_mutex);
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
sigset_t proc_handlers(){

    struct sigaction action;
    sigset_t mask, oldmask;

    action.sa_handler = sigusr_handler;
    sigemptyset(&(action.sa_mask));
    action.sa_flags = 0;

    if(sigaction(SIGUSR2, &action, NULL) < 0){
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if(sigaction(SIGUSR1, &action, NULL) < 0){
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    /*sigint no necesita ser bloqueada porque no la esperaremos en ningun momento con sigsuspend*/
    if(sigaction(SIGINT, &action, NULL) < 0){
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    sigemptyset(&mask);
    if(sigaddset(&mask, SIGUSR2) < 0){
        perror("Sigaddset sigusr2");
        exit(EXIT_FAILURE);
    }
    if(sigaddset(&mask, SIGUSR1) < 0){
        perror("Sigaddset sigusr1");
        exit(EXIT_FAILURE);
    }
    sigprocmask (SIG_BLOCK, &mask, &oldmask);
    
    return oldmask;
}

Block* open_sharedBlockMemory(){

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
                shBlock = updateBlock(NULL);
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
            fprintf(stderr, "Failed to map the shared block\n");
            return NULL;
        }
        close(fd);
        for(i = 0; i < MAX_MINERS; i++){ /*If  the blockchain is the first blockchain of the chain*/
            shBlock->wallets[i] = 0; 
        }
        shBlock->id = 1;

        return shBlock;
    }
}

void reverse(char s[])
 {
     int i, j;
     char c;
    /*Da la vuelta a la cadena pasada por referencia*/
     for (i = 0, j = strlen(s)-1; i<j; i++, j--) {
         c = s[i];
         s[i] = s[j];
         s[j] = c;
     }
}  

void intToAscii(int n, char s[]){
     int i=0;
     do { 
         s[i++] = n%10 + '0'; 
     }while((n /= 10) > 0);     
     s[i] = '\0';
     reverse(s);
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

        
        for(i = iniInterval; i < iniInterval+intervalSize && pow->currentBlock->solution_found == 0 && !sigusr2_received; i++){
            if(pow->currentBlock->target == simple_hash(i)){
                sem_wait(&pow->net->solution_mutex);
                if(pow->currentBlock->solution_found == 0){
                    pow->currentBlock->solution_found = 1;
                    pow->currentBlock->solution = i;
                    
                    sem_wait(&pow->net->netShMemory_mutex);
                    pow->net->last_winner = getpid();
                    sem_post(&pow->net->netShMemory_mutex);

                    for(i = 0; i < pow->net->total_miners; i++){
                        if(getpid() != pow->net->miners_pid[i]){
                            if(kill(pow->net->miners_pid[i], SIGUSR2)){
                                if(errno != ESRCH){
                                    /*no retorna porque el error no es crítico*/
                                    perror("kill_sigusr2_byWorker");
                                }
                            }
                        }
                        else{
                            if(kill(pow->net->miners_pid[i], SIGUSR1)){
                                if(errno != ESRCH){
                                    /*no retorna porque el error no es crítico*/
                                    perror("kill_sigusr1_byWorker");
                                }
                            }
                        } 
                    }
                }
                sem_post(&pow->net->solution_mutex);
                pthread_exit(NULL);
            }
        }
        pthread_exit(NULL);
}

void blocksigint(){
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

void unblocksigint(){
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

void sigusr_handler(int signal){
    if(signal == SIGUSR2) sigusr2_received = 1;
    if(signal == SIGUSR1) sigusr1_received = 1;
    if(signal == SIGINT) sigint_received = 1;
}

int signUp(NetData * net){
    sigset_t emptyMask;

    /*Crea un set vacio para bloquear todas las señales*/
    sigemptyset(&emptyMask);
    /*Si ya se han lanzado el maximo de mineros retorna*/
    if(net->total_miners == MAX_MINERS){
        printf("Max miners reached\n");
        return ERR;
    }

    /*En caso de que no sea el primer minero, puede ser que haya una ronda en curso*/
    if(net->total_miners > 0 && net->roundInProgress){

        sem_wait(&net->netShMemory_mutex);
        /*se apunta a la lista de espera*/
        net->waiting_list[net->numWaiting_list] = getpid();
        net->numWaiting_list++;
        sem_post(&net->netShMemory_mutex);
        /*Espera a que llegue una señal del ganador de la ronda para registrarse*/
        while(net->roundInProgress) sigsuspend(&emptyMask);
        kill(net->last_winner, SIGUSR1);
    } 
    /*Se apunta a la lista de mineros */
    sem_wait(&net->netShMemory_mutex);
    net->miners_pid[net->total_miners] = getpid();
    (net->total_miners)++;
    sem_post(&net->netShMemory_mutex);

    return OK;
}

void vote(NetData * net, Block * shBlock){

    int i;
    int pos_found = 0;

    /*Encuentra la posicion del minero que votara*/
    for(i = 0; i < net->total_miners && !pos_found; i++){
        if(net->miners_pid[i] == getpid()) pos_found = 1; 
    }
    i--;

    /*Vota en consecuencia del retorno de simple_hash*/
    if(simple_hash(shBlock->solution) == shBlock->target){
        sem_wait(&net->netShMemory_mutex);
        net->voting_pool[i] = VALID_VOTE;
        sem_post(&net->netShMemory_mutex);
    }
    else {
        sem_wait(&net->netShMemory_mutex);
        net->voting_pool[i] = INVALID_VOTE;
        sem_post(&net->netShMemory_mutex);
    }
}

/*guarda en array las posiciones en el array de los mineros activos y retorna el tamaño de dicho array*/
int notifyActiveMiners(NetData * net, int *array){
    int i, activeMiners = 0, j = 0;
    int * aux, activeMinersPos[MAX_MINERS];
    if(array) aux = array;
    else aux = activeMinersPos;

    for(i = 0; i < net->total_miners; i++){
        if(net->miners_pid[i] != getpid()){
            /*No controla el retorno de kill porque no es un fallo critico para el funcionamiento del programa*/
            if(!kill(net->miners_pid[i], SIGUSR1)){
                aux[j] = i;
                j++;
                activeMiners++;
            }
        }
        else{
            aux[j] = i;
            j++;
            activeMiners++;
        }
    }
    return activeMiners;
}

Block * addBlockToBlockchain(Block *shBlock, NetData *net){

    int fd;
    int i, j;
    struct stat statbuf;
    
    if((fd = shm_open(SHM_NAME_BLOCK, O_RDWR, 0)) == -1){
       perror("addBlock ShmOpen");
       return NULL;
    }
    else{
        if(fstat(fd, &statbuf)){
            perror("fstat");
            close(fd);
        }
        /*Borra el mapeado hecho previamente*/
        munmap(shBlock, statbuf.st_size);
        /*Trunca el tamaño del fichero al anterior mas un bloque nuevo*/
        if(ftruncate(fd, sizeof(Block) + statbuf.st_size) == -1){
                perror("ftruncate");
                close(fd);
                return NULL;
        }
        if ((shBlock = (Block*)mmap(NULL, sizeof(Block) + statbuf.st_size , PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
                close(fd);
                perror("mmap");
                return NULL;
        }
        i = statbuf.st_size/sizeof(Block); /*Consigue la posición del nuevo bloque*/
        close(fd);

        /*Setea los campos del nuevo bloque*/
        sem_wait(&net->blockShMemory_mutex);
        shBlock[i].id = shBlock[i-1].id+1;
        shBlock[i].target = shBlock[i-1].solution;
        shBlock[i].prev = &shBlock[i-1];
        shBlock[i].next = NULL;
        shBlock[i-1].next = &shBlock[i];
        shBlock[i].solution_found = 0;
        for(j = 0; j < net->total_miners; j++) shBlock[i].wallets[j] = shBlock[i-1].wallets[j];
        sem_post(&net->blockShMemory_mutex);

        /*Devuelve el puntero al nuevo bloque creado*/
        return &shBlock[i];
    }

}

/*Cumple el mismo proposito que la funcion anterior pero para mineros que perdieron la ronda*/
Block * updateBlock(Block *shBlock){ 
    
    int fd, i;
    struct stat statbuf;

    if((fd = shm_open(SHM_NAME_BLOCK, O_RDWR, 0)) == -1){
       perror("updateBlock ShmOpen");
       return NULL;
    }
    else{
        if(fstat(fd, &statbuf)){
            perror("fstat");
            close(fd);
        }
        if (shBlock) munmap(shBlock, statbuf.st_size);

        if ((shBlock = (Block*)mmap(NULL, statbuf.st_size , PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
            perror("mmap");
            close(fd);
            exit(EXIT_FAILURE);
        }
        i = statbuf.st_size/sizeof(Block);
        close(fd);
        
        /*En este caso es i-1 porque tiene en cuenta que se ha debido de ejecutar la funcion addblocktoblockchain previamente*/
        return &shBlock[i-1];
    }
}

void addCoin(NetData *net, Block * shBlock){
    int i;

    for(i = 0; i < net->total_miners; i++){
        if(getpid() == net->miners_pid[i]){
            sem_wait(&net->blockShMemory_mutex);
            shBlock->wallets[i]++;
            sem_post(&net->blockShMemory_mutex);
            return;
        }  
    }

}

void addBlockToSelfBlockchain(Block * blockchain, Block *shBlock, int pos){

    blockchain[pos] = *shBlock;
}

void printBlockchainAtExit(Block *blockchain, NetData *net, int roundsDone){
    int j, i;
    FILE *f;
    char str[30];
    char path[30];

    /*Construye el path al directorio en el que guardara el fichero*/
    intToAscii(getpid(), str);
    strcpy(path, "./blockchains/");
    strcat(path, str);

    f = fopen(path, "w");
    if(!f){
        printf("Failed to open file\n");
        return;
    } 

    fprintf(f, "\nPRINTING BLOCKCHAIN\n");
    for(i = 0; i < roundsDone; i++) {
        fprintf(f, "Block number: %d; Target: %ld;    Solution: %ld\n", blockchain[i].id, blockchain[i].target, blockchain[i].solution);
        for(j = 0; j < net->total_miners; j++) {
            fprintf(f, "%d: %d;         ", j+1, blockchain[i].wallets[j]);
        }
        fprintf(f, "\n\n\n");
    }

    fclose(f);
}

int main(int argc, char *argv[]) {
    int i;
    int aux = 0;
    int activeMinersPos[MAX_MINERS];
    int roundCounter = 0, maxRounds, activeMiners, numVotes;
    pthread_t workers[MAX_WORKERS];
    short numWorkers, isFirst = 0;
    WorkerInfo info[MAX_WORKERS];
    NetData *net; 
    Block *shBlock;
    sigset_t emptyMask;
    mqd_t minersQueue;
    mqd_t monitorQueue;
    Block blockchain[MAX_BLOCKS];
    struct mq_attr attributes = {
        .mq_flags = 0,
        .mq_maxmsg = MAX_WORKERS,
        .mq_curmsgs = 0,
        .mq_msgsize = sizeof(int)
    };
    struct mq_attr monitorAttributes = {
        .mq_flags = 0,
        .mq_maxmsg = MAX_BLOCKS_MONITOR,
        .mq_curmsgs = 0,
        .mq_msgsize = sizeof(Block)
    };
    int posBlockchain = 0;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <NUM_WORKERS> <MAX_ROUNDS>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    numWorkers = atoi(argv[1]);
    if(numWorkers > MAX_WORKERS || numWorkers < 1){
        fprintf(stderr, "The number of workers should be comprehended between 1 and %d, Finishing program...\n", MAX_WORKERS);
        exit(EXIT_FAILURE);
    }
    maxRounds = atoi(argv[2]);
    if(maxRounds <= 0 || maxRounds > MAX_BLOCKS){
        fprintf(stderr, "The number of rounds should be between one and %d, Finishing program...\n", MAX_BLOCKS);
        exit(EXIT_FAILURE);
    }

    /*Procs signal handlers*/
    emptyMask = proc_handlers();

    /*opens a queue to comunicate with other miners*/
    minersQueue = mq_open(MQ_MINERS, O_CREAT | O_RDWR , S_IRUSR | S_IWUSR, &attributes);
    if(minersQueue == (mqd_t)-1) {
        perror("minersQueue");
        exit(EXIT_FAILURE);
    }

    monitorQueue = mq_open(MQ_MONITOR, O_CREAT | O_WRONLY , S_IRUSR | S_IWUSR, &monitorAttributes);
    if(monitorQueue == (mqd_t)-1) {
        perror("monitorQueue");
        mq_close(minersQueue);
        exit(EXIT_FAILURE);
    }

    /*opens and maps netShememory*/
    net = open_netShmemory(&isFirst);
    if(!net){
        fprintf(stderr, "Failed to open the shared net\n");
        mq_close(minersQueue);
        mq_close(monitorQueue);
        exit(EXIT_FAILURE);
    }
    printf("Registering miner %d...\n", getpid());
    sleep(1);
    if(signUp(net) == ERR){
        fprintf(stderr, "Failed to register miner %d\n", getpid());
        munmap(net, sizeof(NetData));
        mq_close(minersQueue);
        mq_close(monitorQueue);
        exit(EXIT_FAILURE);
    }
    printf("Miner registered.\n");
    printf("Printing miners pid list...\n");
    sleep(1);
    for(i = 0; i < net->total_miners; i++){
        printf("%d\n", net->miners_pid[i]);
    }
    sleep(1);

    /*opens and maps the shared block*/
    shBlock = open_sharedBlockMemory();
    if(!shBlock){
        fprintf(stderr, "Failed to open the shared block\n");
        munmap(net, sizeof(NetData));
        mq_close(minersQueue);
        mq_close(monitorQueue);
        exit(EXIT_FAILURE);
    }
    
    
    if(isFirst){
        printf("The miner %d is the first in the net. Creating first block with random target...\n", getpid());
        sleep(1);
        srand(time(NULL));

        sem_wait(&net->blockShMemory_mutex);
        shBlock->id = 0;
        shBlock->prev = NULL;
        shBlock->target = rand()%PRIME;
        printf("Random target: %ld\n", shBlock->target);
        sem_post(&net->blockShMemory_mutex);  
    }
    

    printf("The miner %d is starting its rounds... \n", getpid());
    sleep(1);
    while(roundCounter < maxRounds && sigint_received == 0){

        blocksigint();
        if(!net->roundInProgress){
            sem_wait(&net->netShMemory_mutex);
            net->roundInProgress = 1;
            sem_post(&net->netShMemory_mutex);
        }
        
        printf("Target for the round %d: %ld\n",  roundCounter+1, shBlock->target);
        /*carga la info de cada trabajador*/
        /*lanza el número de trabajadores especificados*/
        sigusr2_received = 0;
        sigusr1_received = 0;


        printf("Sending workers to mine the solution...\n");
        sleep(1);
        for (i = 0; i < numWorkers; i++){
            info[i].numWorkers = numWorkers;
            info[i].numThread = i;
            info[i].currentBlock = shBlock;
            info[i].net = net;
            printf("Sending worker Nº %d...\n", i+1);
            if(pthread_create(&workers[i], NULL, worker, &info[i]) != 0){
                fprintf(stderr, "Failed creating a worker...\n");
                munmap(net, sizeof(NetData));
                munmap(shBlock, sizeof(Block));
                mq_close(minersQueue);
                mq_close(monitorQueue);
                exit(EXIT_FAILURE);
            }
        }
        printf("All workers were sent. Main thread will now wait for the results...\n");
        while(!sigusr2_received && !sigusr1_received) sigsuspend(&emptyMask);

        if(sigusr2_received) printf("\n\nROUND LOST\n\n");
        else  printf("\n\nROUND WON\n\n");

        for(i = 0; i < numWorkers; i++){
            pthread_join(workers[i], NULL);
        }
        sleep(1);
        printf("End of the round...\n");
        sleep(1);
        roundCounter++;   
        


        if(net->last_winner == getpid()){

        /*Notifica SIGUSR1 a los mineros activos para indicar que ya fueron seteados los campos y pueden votar*/
        /****************************************************************************************************/
            activeMiners = notifyActiveMiners(net, activeMinersPos);
        /****************************************************************************************************/


        /*Se queda a la espera de que los otros mineros notifiquen que ya han votado*/
        /****************************************************************************************************/
            sleep(1);
            numVotes = 0;
            printf("Waiting for other miners to vote...\n");
            while (numVotes != activeMiners - 1){

                mq_receive(minersQueue, (char *)&aux, sizeof(int), NULL);
                printf("Vote received\n");
                numVotes++;
                printf("Votes %d/%d\n", numVotes, activeMiners - 1);
            }
        /****************************************************************************************************/
        
        
        /*Hace el recuento de votos*/
        /****************************************************************************************************/
            sleep(1);
            numVotes = 0;
            printf("Counting votes...\n");
            for(i = 0; i < activeMiners ; i++){
                if(getpid() != net->miners_pid[activeMinersPos[i]]){
                    if(net->voting_pool[activeMinersPos[i]] == VALID_VOTE)
                        numVotes++;
                }
            }
            printf("Votes: %d\n", numVotes);
            sleep(1);
        /****************************************************************************************************/
            

        /*Decide si el bloque es valido o no y actua en consecuencia*/   
        /****************************************************************************************************/    
            sleep(1);
            printf("Deciding if the block is valid...\n");
            if(numVotes >= (activeMiners - 1)/2){
                sem_wait(&net->blockShMemory_mutex);
                shBlock->is_valid = 1;
                sem_post(&net->blockShMemory_mutex);
                addCoin(net, shBlock);
                addBlockToSelfBlockchain(blockchain, shBlock, posBlockchain);
                posBlockchain++;
                if(net->monitor_pid != NO_MONITOR){
                    printf("Sending info to the monitor\n");
                    if(mq_send(monitorQueue, (char*)shBlock, sizeof(Block), 2) == -1){
                        perror("monitorSend");
                    }
                } 
                shBlock = addBlockToBlockchain(shBlock, net);
                if(!shBlock){
                    munmap(net, sizeof(NetData));
                    munmap(shBlock, sizeof(Block));
                    mq_close(minersQueue);
                    mq_close(monitorQueue);
                    exit(EXIT_FAILURE);
                }
            }
            else{
                sem_wait(&net->blockShMemory_mutex);
                shBlock->is_valid = 0;
                sem_post(&net->blockShMemory_mutex);
            }
            sleep(1);
        /****************************************************************************************************/
        
        
        /*Notifica a los otros mineros activos que ya se sabe si el bloque es valido*/
        /****************************************************************************************************/
            notifyActiveMiners(net, NULL);
        /****************************************************************************************************/
        

        /*Avisa a los que estaban en lista de espera que ya acabo la ronda,*/
        /****************************************************************************************************/
            sleep(1);
            printf("Notifying miners in waiting list...\n");
            sem_wait(&net->netShMemory_mutex);
            net->last_winner = getpid();
            net->roundInProgress = 0;
            for(i = 0; i < net->numWaiting_list; i++){
                if(kill(net->waiting_list[i], SIGUSR1)) perror("kill_toWaitingList");
                net->waiting_list[i] = -1;
            }
            for(i = 0; i < net->numWaiting_list; i++){
                sigusr1_received = 0; 
                while(!sigusr1_received){
                    sigsuspend(&emptyMask);
                } 
            }
            net->numWaiting_list = 0;
            sem_post(&net->netShMemory_mutex);
        /****************************************************************************************************/


        /*Notifica a los usuarios de que la ronda acabo y de que toca empezar la siguiente*/
        /****************************************************************************************************/    
            notifyActiveMiners(net, NULL);
        /****************************************************************************************************/
        }

        else{
        /*Entra en espera no activa hasta que el ganador notifique que se ha escrito la solucion*/
        /****************************************************************************************************/
            sleep(1);
            printf("Waiting for the winner to signal the rest of miners can vote\n");
            while(!sigusr1_received) sigsuspend(&emptyMask); /*espera a que el ganador le mande sigusr1 para que se contabilice su voto*/
            sigusr1_received = 0;
        /****************************************************************************************************/
            

        /*Vota si la solución propuesta es válida o no*/   
        /****************************************************************************************************/
            sleep(1);
            printf("Voting...\n");
            vote(net, shBlock);

            printf("Notifying the winner the vote has been casted...\n");
            if(mq_send(minersQueue, (char *)&aux, sizeof(int), 0) == -1){
                perror("mq_send minersQueue");
            }
            sleep(1);
        /****************************************************************************************************/


        /*Entra en espera no activa hasta que se sepa si el bloque es valido o no*/
        /****************************************************************************************************/
            sleep(1);
            printf("Waiting for the winner to decide if the solution is valid or not...\n");
            while(!sigusr1_received) sigsuspend(&emptyMask); /*esperan a que el ganador comunica si el bloque es valido*/
            sigusr1_received = 0;
            if(shBlock->is_valid){
                if(net->monitor_pid != NO_MONITOR){
                    printf("Sending info to the monitor\n");
                    if(mq_send(monitorQueue, (char*)shBlock, sizeof(Block), 1) == -1){
                        perror("monitorSend");
                    }
                }
                addBlockToSelfBlockchain(blockchain, shBlock, posBlockchain);
                posBlockchain++;
                shBlock = updateBlock(shBlock);    
                printf("The block is valid!\n");
            }
        /****************************************************************************************************/


        /*Entra en espera no activa hasta que se sepa si la ronda ha terminado*/
        /****************************************************************************************************/
            sleep(1);
            printf("Waiting for the winner to notify the round is over\n");
            while(!sigusr1_received) sigsuspend(&emptyMask); /*esperan a que el ganador comunica si el bloque es valido*/
        /****************************************************************************************************/     
        }
        unblocksigint();
    }

    printBlockchainAtExit(blockchain, net, posBlockchain);
    if(sigint_received) printf("Sigint received, finishing program...\n");
    else printf("Max rounds reached, finishing program...\n");

    munmap(net, sizeof(NetData));
    munmap(shBlock, sizeof(Block));
    mq_close(minersQueue);
    mq_close(monitorQueue);
    exit(EXIT_SUCCESS); 
}


