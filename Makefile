PORT = 12345
FLAGS = -DPORT=$(PORT) -Wall -g -std=gnu99 

server : server.o network.o game.o
	gcc $(FLAGS) -o $@ $^

%.o : %.c network.h game.h
	gcc $(FLAGS) -c $<

clean : 
	rm *.o server
