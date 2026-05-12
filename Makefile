bic: main.cpp irc_model.cpp irc_model.h irc_view.cpp irc_view.h irc_controller.cpp irc_controller.h
	g++ main.cpp irc_model.cpp irc_view.cpp irc_controller.cpp -o bic -lfltk

.PHONY: clean

clean:
	rm bic
