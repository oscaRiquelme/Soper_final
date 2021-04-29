#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>


#include "miner.h"

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819

typedef struct _workerInfo {
    int target; /*Target to be found in the pow*/
    int numWorkers; /*argv[1]*/
    int numThread;/*Identifier comprehended betweeen 1 and argv[2]*/
    int solution;
} WorkerInfo;

static volatile int solution_found;

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

int open_netShmemory(){
    
    int fd;
    if((fd = shm_open(SHM_NAME_NET, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1){
        if(errno == EEXIST){
            fd = shm_open(SHM_NAME_NET, O_RDWR, 0);
            if(fd == -1){
                perror("open_shmemory(existing)");
                return ERR;
            }
            else{
                return fd;
            }
        }
        else{
            perror("open_shmemory(non_existing)");
            return ERR;
        }
    }
    else{
        if(ftruncate(fd, sizeof(NetData)) == -1){
            perror("ftruncate");
            shm_unlink(SHM_NAME_NET);
            close(fd);
            return ERR;
        }
        return fd;
    }
}

NetData* map_netShmemory(int fd){

    NetData* data = NULL;

    if ((data = (NetData*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return data;
}

/*Creo que block debería de ser la memoría compartida de los bloques*/
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
            if(pow->target == simple_hash(i)){
                solution_found = 1;
                pow->solution = i;
                pthread_exit(pow);
            }
        }
        return NULL;
}

int main(int argc, char *argv[]) {
    long int i;
    int solution;
    pthread_t workers[MAX_WORKERS];
    short numWorkers;
    WorkerInfo info[MAX_WORKERS];

    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <NUM_WORKERS>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    numWorkers = atoi(argv[1]);
    if(numWorkers > 10 || numWorkers < 1){
        fprintf(stderr, "Manito ponme al menos 1 trabajador y no mas de 10\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < numWorkers; i++){
        info[i].numWorkers = numWorkers;
        info[i].target = 1000;
        info[i].solution = -1;
        info[i].numThread = i;
        if(pthread_create(&workers[i], NULL, worker, &info[i]) != 0){
            fprintf(stderr, "Manito fracase creando hilos...\n");
            exit(EXIT_FAILURE);
        }
    }

    for(i = 0; i < numWorkers; i++){
        pthread_join(workers[i], NULL);
        if(info[i].solution != -1 ) solution = info[i].solution;
    }

    fprintf(stdout, "Solucion: %d\n", solution);
    exit(EXIT_SUCCESS); 
        
    
}


