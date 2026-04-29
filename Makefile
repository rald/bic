bic: main.cpp
	g++ main.cpp -o bic -std=c++11 -lfltk -lpthread

.PHONY: clean

clean:
	rm bic
