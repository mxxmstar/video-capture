CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2
TARGET := v4l2
SRCS := v4l2.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)