# Makefile for BIC IRC Client (MVC version)
# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2
LDLIBS = -lfltk -lpthread -lX11

# Target executable
TARGET = bic_mvc

# Source files
SRCS = main.cpp irc_model.cpp irc_view.cpp irc_controller.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

# Default rule
all: $(TARGET)

# Link object files into final executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

# Compile C++ source to object file, generate dependency file
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean build artifacts
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

# Phony targets
.PHONY: all clean