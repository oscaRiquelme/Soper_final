SRC=./src/
HDR=./hdr/
THR_L=-lpthread 
RT_L=-lrt
FLAGS = -Wall -pedantic -ansi -g -I $(HDR)
OBJECTS = space.o command.o game.o game_reader.o game_loop.o graphic_engine.o game_reader.o screen.o object.o set.o dice.o link.o inventory.o player.o

all: miner 
	
miner: 
	gcc -g -o miner $(FLAGS) $(SRC)miner.c $(THR_L) $(RT_L)
#Borrar objetos
clean:
	@echo "Cleaning up files..."
	rm -f miner 

