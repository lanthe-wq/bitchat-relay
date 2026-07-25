# Host checks for the relay. No ESP32 and no BLE stack required.
#
#   make                 run the logic tests
#   make compile-check   type-check bitchat_relay.ino against NimBLE 2.x signatures
#   make check           both
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion

SKETCHFLAGS ?= -std=c++11 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Wconversion

BIN    = test_relay_core
SKETCH = ../bitchat_relay.ino
STUBS  = stubs

.PHONY: all check run compile-check clean
all: run

$(BIN): test_relay_core.cpp ../relay_core.h
	$(CXX) $(CXXFLAGS) -o $@ test_relay_core.cpp

run: $(BIN)
	./$(BIN)

# Parses and type-checks the sketch against stub headers that mirror the real
# NimBLE-Arduino 2.3.6 signatures. Catches the 1.x/2.x API drift the README
# warns about, without needing the ESP32 toolchain. See stubs/README.md.
compile-check: $(SKETCH) ../relay_core.h
	$(CXX) $(SKETCHFLAGS) -I $(STUBS) -include Arduino.h -x c++ $(SKETCH)
	@echo "compile-check: OK"

check: compile-check run

clean:
	rm -f $(BIN)
