CXX      = g++
CXXFLAGS = -O2 -Wall -std=c++11
LDFLAGS  = -lncursesw -lpthread

TARGET = tcfplay

$(TARGET): tcfplay.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) tplay.cpp $(LDFLAGS)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

clean:
	rm -f $(TARGET)

.PHONY: install clean
