#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "miner.h"

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819


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

int open_shmemory(){
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

int main(int argc, char *argv[]) {
    long int i, target;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <TARGET>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    target = atol(argv[1]);

    for (i = 0; i < PRIME; i++) {
        fprintf(stdout, "Searching... %6.2f%%\r", 100.0 * i / PRIME);
        if (target == simple_hash(i)) {
            fprintf(stdout, "\nSolution: %ld\n", i);
            exit(EXIT_SUCCESS);
        }
    }
    fprintf(stderr, "\nSearch failed\n");
    exit(EXIT_FAILURE);
}


