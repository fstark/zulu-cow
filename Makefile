CXX = g++
CXXFLAGS = -std=c++23 -Wall -Wextra -O2 -g
# CXXFLAGS = -std=c++23 -Wall -Wextra -O0 -g
DEPFLAGS = -MMD -MP
MAKEFLAGS += -j12

TARGET = cow_test
SOURCES = cow_test.cpp zulu_cow.cpp
OBJECTS = $(SOURCES:.cpp=.o)
DEPS = $(SOURCES:.cpp=.d)

# Default target
all: $(TARGET)

# Include dependency files
-include $(DEPS)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS)

# Compile each source file to object file and generate dependencies
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPS)

.PHONY: all clean install
