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

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819

typedef struct _workerInfo {
    int numWorkers; /*argv[1]*/
    int numThread; /*Identifier comprehended betweeen 0 and argv[2] - 1*/
    Block *currentBlock;
    NetData *net;
} WorkerInfo;

static volatile int sigusr2_received = 0;
static volatile int sigusr1_received = 0;

/************************PRIVATE*HEADERS****************************/
NetData* open_netShmemory(short * isFirst);
NetData* map_netShmemory(int fd);
sigset_t proc_handlers();
Block* open_blockChainMemory();
Block* map_blockShmemory(int fd);
void* worker(void* args);
void sigusr_handler(int signal);
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
                    fprintf(stderr, "Failed to map the net\n");
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
            fprintf(stderr, "Failed to map the net\n");
            close(fd);
            return NULL;
        }
        close(fd);
        if(sem_init(&net->netShMemory_mutex, 1, 1)){
            perror("net SemInit");
            munmap(net, sizeof(NetData));
            return NULL;
        }
        sem_wait(&net->netShMemory_mutex);
        net->roundInProgress = 0;
        net->total_miners = 0;
        net->numWaiting_list = 0;
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

/*TODO: exit failures no me gusta, quizas una variable por referencia para cde*/
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

    sigemptyset(&mask);
    if(sigaddset(&mask, SIGUSR2) < 0){
        perror("Sigaddset sigusr2");
        exit(EXIT_FAILURE);
    }
    if(sigaddset(&mask, SIGUSR1) < 0){
        perror("Sigaddset sigusr2");
        exit(EXIT_FAILURE);
    }
    sigprocmask (SIG_BLOCK, &mask, &oldmask);
    
    return oldmask;
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
                    fprintf(stderr, "Failed to map the shared block\n");
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
            fprintf(stderr, "Failed to map the shared block\n");
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

        
        for(i = iniInterval; i < iniInterval+intervalSize && pow->currentBlock->solution_found == 0 && !sigusr2_received; i++){
            if(pow->currentBlock->target == simple_hash(i)){
                sem_wait(&pow->currentBlock->solution_mutex);
                if(pow->currentBlock->solution_found == 0){
                    pow->currentBlock->solution_found = 1;
                    pow->currentBlock->solution = i;
                    
                    sem_wait(&pow->net->netShMemory_mutex);
                    pow->net->last_winner = getpid();
                    sem_post(&pow->net->netShMemory_mutex);

                    for(i = 0; i < pow->net->total_miners; i++){
                        if(getpid() != pow->net->miners_pid[i]){
                            if(kill(pow->net->miners_pid[i], SIGUSR2)){
                                perror("kill_sigusr2");
                                /*PROBABLEMENTE TENGA QUE LIBERAR MEMORIA Y CERRAR COSAS*/
                            }
                        }
                        else{
                            if(kill(pow->net->miners_pid[i], SIGUSR1)){
                                perror("kill_sigusr1");
                                /*PROBABLEMENTE TENGA QUE LIBERAR MEMORIA Y CERRAR COSAS*/
                            }
                        } 
                    }
                }
                sem_post(&pow->currentBlock->solution_mutex);
                pthread_exit(pow);
            }
        }
        pthread_exit(NULL);
}

void sigusr_handler(int signal){
    if(signal == SIGUSR2) sigusr2_received = 1;
    if(signal == SIGUSR1) sigusr1_received = 1;
}

int signUp(NetData * net){
    sigset_t emptyMask;

    sigemptyset(&emptyMask);
    if(net->total_miners == MAX_MINERS){
        return ERR;
    }

    if(net->total_miners > 0){

        sem_wait(&net->netShMemory_mutex);
        net->waiting_list[net->numWaiting_list] = getpid();
        net->numWaiting_list++;
        sem_post(&net->netShMemory_mutex);
        while(net->roundInProgress) sigsuspend(&emptyMask);
        kill(net->last_winner, SIGUSR1);
    } 
    sem_wait(&net->netShMemory_mutex);
    net->miners_pid[net->total_miners] = getpid();
    (net->total_miners)++;
    sem_post(&net->netShMemory_mutex);

    return OK;
}

void vote(NetData * net, Block * shBlock){

    int i;
    int pos_found = 0;

    for(i = 0; i < net->total_miners && !pos_found; i++){
        if(net->miners_pid[i] == getpid()) pos_found = 1; 
    }

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

int notifyActiveMiners(NetData * net, int *array){
    int i, activeMiners = 0, j = 0;
    int * aux, activeMinersPos[MAX_MINERS];
    if(array) aux = array;
    else aux = activeMinersPos;

    for(i = 0; i < net->total_miners; i++){
        if(net->miners_pid[i] != getpid()){
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
        //else if(errno != ESRCH) return -1;
    }
    return activeMiners;
}

int main(int argc, char *argv[]) {
    int i;
    int aux;
    int activeMinersPos[MAX_MINERS];
    int roundCounter = 0, maxRounds, activeMiners, numVotes;
    pthread_t workers[MAX_WORKERS];
    short numWorkers, isFirst = 0;
    WorkerInfo info[MAX_WORKERS];
    NetData *net; 
    Block *shBlock;
    sigset_t emptyMask;
    sem_t globalVarMutex;
    mqd_t minersQueue;
    struct mq_attr attributes = {
        .mq_flags = 0,
        .mq_maxmsg = MAX_WORKERS,
        .mq_curmsgs = 0,
        .mq_msgsize = sizeof(int)
    };
   
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
    if(maxRounds <= 0){
        fprintf(stderr, "The number of rounds should be one or more, Finishing program...\n");
        exit(EXIT_FAILURE);
    }

    /*Procs signal handlers*/
    emptyMask = proc_handlers();
    /*TODO: esto vale para algo?? */
    if(sem_init(&globalVarMutex, 0, 1)){
        perror("globalVarMutex sem_init");
        exit(EXIT_FAILURE);
    }

    minersQueue = mq_open(MQ_MINERS, O_CREAT | O_RDWR , S_IRUSR | S_IWUSR, &attributes);
    if(minersQueue == (mqd_t)-1) {
        perror("minersQueue");
        exit(EXIT_FAILURE);
    }

    /*opens and maps netShememory*/
    net = open_netShmemory(&isFirst);
    if(!net){
        fprintf(stderr, "Failed to open the shared net\n");
        exit(EXIT_FAILURE);
    }
    /*opens and maps the blockchain*/
    shBlock = open_blockChainMemory();
    if(!shBlock){
        fprintf(stderr, "Failed to open the shared block\n");
        munmap(net, sizeof(NetData));
        exit(EXIT_FAILURE);
    }
    
    printf("Registering miner %d...\n", getpid());
    sleep(2);
    
    if(signUp(net) == ERR){
        fprintf(stderr, "Failed to register miner %d\n", getpid());
        munmap(net, sizeof(NetData));
        munmap(shBlock, sizeof(Block));
    }
    printf("Miner registered.\n");
    printf("Printing miners pid list...\n");
    for(i = 0; i < net->total_miners; i++){
        printf("%d\n", net->miners_pid[i]);
    }
    sleep(2);
    if(isFirst){
        printf("The miner %d is the first in the net. Creating first block with random target...\n", getpid());
        sleep(2);
        srand(time(NULL));

        sem_wait(&shBlock->blockShMemory_mutex);
        shBlock->id = 0;
        shBlock->prev = NULL;
        shBlock->target = rand()%PRIME;
        printf("Random target: %ld\n", shBlock->target);
        sem_post(&shBlock->blockShMemory_mutex);  
    }
    

    printf("The miner %d is starting its rounds... \n", getpid());
    while(roundCounter < maxRounds){
        /*se prepara una ronda*/
        if(!net->roundInProgress){
            sem_wait(&net->netShMemory_mutex);
            net->roundInProgress = 1;
            sem_post(&net->netShMemory_mutex);
        }
        
        printf("Target for the round %d: %ld\n",  roundCounter+1, shBlock->target);
        sleep(2);
        /*carga la info de cada trabajador*/
        /*lanza el número de trabajadores especificados*/
        sigusr2_received = 0;
        sigusr1_received = 0;
        sem_wait(&shBlock->blockShMemory_mutex);
        shBlock->solution_found = 0;
        sem_post(&shBlock->blockShMemory_mutex);

        printf("Sending workers to mine the solution...\n");
        sleep(2);
        for (i = 0; i < numWorkers; i++){
            info[i].numWorkers = numWorkers;
            info[i].numThread = i;
            info[i].currentBlock = shBlock;
            info[i].net = net;
            printf("Sending worker Nº %d...\n", i+1);
            if(pthread_create(&workers[i], NULL, worker, &info[i]) != 0){
                fprintf(stderr, "Manito fracase creando hilos...\n");
                exit(EXIT_FAILURE);
            }
        }
        sleep(2);
        printf("All workers were sent. Main thread will now wait for the results...\n");
        while(!sigusr2_received && !sigusr1_received) sigsuspend(&emptyMask);

        if(sigusr2_received) printf("\n\nROUND LOST\n\n");
        else  printf("\n\nROUND WON\n\n");

        for(i = 0; i < numWorkers; i++){
            pthread_join(workers[1], NULL);
        }
        printf("End of the round...\n");
        sleep(2);
        roundCounter++;   
        


        if(net->last_winner == getpid()){

        /*Notifica SIGUSR1 a los mineros activos para indicar que ya fueron seteados los campos y pueden votar*/
        /****************************************************************************************************/
            activeMiners = notifyActiveMiners(net, activeMinersPos);
            sleep(2);
        /****************************************************************************************************/


        /*Se queda a la espera de que los otros mineros notifiquen que ya han votado*/
        /****************************************************************************************************/
            numVotes = 0;
            printf("Waiting for other miners to vote...\n");
            while (numVotes != activeMiners - 1){
                /*TODO: hacer algo con lo de aux xd*/
                mq_receive(minersQueue, (char *)&aux, sizeof(int), NULL);
                printf("Vote received\n");
                sleep(1);
                numVotes++;
                printf("Votes %d/%d\n", numVotes, activeMiners - 1);
            }
        /****************************************************************************************************/
        
        
        /*Hace el recuento de votos*/
        /****************************************************************************************************/
            numVotes = 0;
            printf("Counting votes...\n");
            for(i = 0; i < activeMiners ; i++){
                if(getpid() != net->miners_pid[activeMinersPos[i]]){
                    if(net->voting_pool[activeMinersPos[i]] == VALID_VOTE)
                        numVotes++;
                }
            }
            printf("Votes: %d\n", numVotes);
            sleep(2);
        /****************************************************************************************************/
            

        /*Decide si el bloque es valido o no*/   
        /****************************************************************************************************/    
            if(numVotes >= (activeMiners - 1)/2 + 1){
                sem_wait(&shBlock->blockShMemory_mutex);
                shBlock->is_valid = 1;
                sem_post(&shBlock->blockShMemory_mutex);
                printf("Block is valid\n");
            }
            else{
                sem_wait(&shBlock->blockShMemory_mutex);
                shBlock->is_valid = 0;
                sem_post(&shBlock->blockShMemory_mutex);
                printf("Block is not valid\n");
            }
            sleep(2);
        /****************************************************************************************************/
        
        
        /*Notifica a los otros mineros activos que ya se sabe si el bloque es valido*/
        /****************************************************************************************************/
            notifyActiveMiners(net, NULL);
            sleep(2);
        /****************************************************************************************************/
        

        /*Setea las variables pertinentes aunque algunas no se pa que*/
        /****************************************************************************************************/
            sem_wait(&net->netShMemory_mutex);
            net->last_winner = getpid();
            net->roundInProgress = 0;
            for(i = 0; i < net->numWaiting_list; i++){
                printf("Mandando señal a %d\n", net->waiting_list[i]);
                kill(net->waiting_list[i], SIGUSR1);
                net->waiting_list[i] = -1;
            }
            for(i = 0; i < net->numWaiting_list; i++){
                sigusr1_received = 0; 
                while(!sigusr1_received){
                    printf("Bloqueado\n");
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
            printf("Waiting for the winner to signal the rest of miners can vote\n\n");
            while(!sigusr1_received) sigsuspend(&emptyMask); /*espera a que el ganador le mande sigusr1 para que se contabilice su voto*/
            sigusr1_received = 0;
        /****************************************************************************************************/
            

        /*Vota si la solución propuesta es válida o no*/   
        /****************************************************************************************************/
            printf("Voting...\n");
            vote(net, shBlock);
            /*TODO: hacer algo con lo de aux xd*/
            sleep(2);
            printf("Notifying the winner the vote has been casted...\n");
            mq_send(minersQueue, (char *)&aux, sizeof(int), 0);
            sleep(2);
        /****************************************************************************************************/


        /*Entra en espera no activa hasta que se sepa si el bloque es valido o no*/
        /****************************************************************************************************/
            printf("Waiting for the winner to decide if the solution is valid or not...\n");
            while(!sigusr1_received) sigsuspend(&emptyMask); /*esperan a que el ganador comunica si el bloque es valido*/
            sigusr1_received = 0;
            if(shBlock->is_valid){
                /*TODO: añadir al blockchain de cada minero el bloque creado*/    
                printf("The block is valid!\n");
            }
        /****************************************************************************************************/


        /*Entra en espera no activa hasta que se sepa si la ronda ha terminado*/
        /****************************************************************************************************/
            printf("Waiting for the winner to notify the round is over\n");
            while(!sigusr1_received) sigsuspend(&emptyMask); /*esperan a que el ganador comunica si el bloque es valido*/
        /****************************************************************************************************/     
        }
    }

    fprintf(stdout, "Max rounds reached, finishing program...\n");
    exit(EXIT_SUCCESS); 
        
    
}


