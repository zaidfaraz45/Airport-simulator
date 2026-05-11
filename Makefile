all:
	gcc airport_sim_pro.c -o airport_sim_pro -lpthread -lncurses -Wall -O2

run:
	./airport_sim_pro

clean:
	rm -f airport_sim_pro logs.txt
