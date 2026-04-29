bic: main.c
	g++ main.c -o bic -std=c++11 -lfltk -lpthread

.PHONY: clean

clean:
	rm bic
