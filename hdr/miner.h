#ifndef MINER_H
#define MINER_H

#include <unistd.h>
#include <sys/types.h>

#define OK 0
#define MAX_WORKERS 10

#define SHM_NAME_NET "/netdata"
#define SHM_NAME_BLOCK "/block"

#define MAX_MINERS 200
#define ERR -1
#define OK 0

typedef struct _Block {
    int wallets[MAX_MINERS];
    long int target;
    long int solution;
    int id;
    int is_valid;
    sem_t blockShMemory_mutex;
    struct _Block *next;
    struct _Block *prev;
} Block;

typedef struct _NetData {
    pid_t miners_pid[MAX_MINERS];
    char voting_pool[MAX_MINERS];
    int last_miner;
    int total_miners;
    pid_t monitor_pid;
    pid_t last_winner;
    sem_t netShMemory_mutex;
} NetData; 

long int simple_hash(long int number);

void print_blocks(Block * plast_block, int num_wallets);

#endif