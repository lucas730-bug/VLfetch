CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
PREFIX   ?= /usr/local
TARGET    = vlfetch

all: $(TARGET)

$(TARGET): vlfetch.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
