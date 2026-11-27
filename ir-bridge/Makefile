CXXFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS += -std=c++17 $(shell pkg-config --cflags libcamera)
LDLIBS   += $(shell pkg-config --libs libcamera)
PREFIX   ?= /usr/local

ir-bridge: ir-bridge.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

install: ir-bridge
	install -Dm755 ir-bridge $(DESTDIR)$(PREFIX)/bin/ir-bridge

clean:
	rm -f ir-bridge

.PHONY: install clean
