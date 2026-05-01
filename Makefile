CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2
LDLIBS = -lfltk -lpthread -lX11

# Default target name
TARGET = bic

SRCS = main.cpp irc_model.cpp irc_view.cpp irc_controller.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

.PHONY: all clean