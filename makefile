SRC=./src/
HDR=./hdr
THR_L=-lpthread 
RT_L=-lrt
FLAGS = -Wall -pedantic -g -I $(HDR)

all: miner monitor
	
miner: 
	gcc -g -o miner $(FLAGS) $(SRC)miner.c $(THR_L) $(RT_L)
monitor:
	gcc -g -o monitor $(FLAGS) $(SRC)monitor.c $(THR_L) $(RT_L)
clean:
	@echo "Cleaning up files..."
	rm -f miner 
	rm -f monitor

memClean:
	rm /dev/shm/netdata /dev/shm/block /dev/mqueue/minerQueue /dev/mqueue/monitorQueue

