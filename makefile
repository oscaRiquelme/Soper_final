SRC=./src/
HDR=./hdr
THR_L=-lpthread 
RT_L=-lrt
FLAGS = -Wall -pedantic -g -I $(HDR)

all: miner 
	
miner: 
	gcc -g -o miner $(FLAGS) $(SRC)miner.c $(THR_L) $(RT_L)
#Borrar objetos
clean:
	@echo "Cleaning up files..."
	rm -f miner 

