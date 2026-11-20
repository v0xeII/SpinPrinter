# PC tools for SpinScreen (they compile the real ../SpinScreen/ui.cpp):
#   make virtual_esp   the screen in a window + a serial port for SpinPi (needs SDL2; without it: headless only)
#   make sim bench     the older recorders: frames to stdout / speed per frame
#   make shots         screenshots of every screen (python ../../dev/make_shots.py ./shots ../screens)
CXX ?= c++
CC ?= cc
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -I../SpinScreen -Wall -Wno-unused-function
FW = ../SpinScreen/ui.cpp ../SpinScreen/*.h

ifneq ($(shell pkg-config --exists sdl2 && echo y),)
SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS = $(shell pkg-config --libs sdl2)
else
SDL_CFLAGS = -DNO_SDL
endif

all: virtual_esp

qr.o: ../SpinScreen/qrcodegen.c
	$(CC) -O2 -c $< -o $@

virtual_esp: virtual_esp.cpp $(FW) qr.o
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) virtual_esp.cpp ../SpinScreen/ui.cpp qr.o -o $@ $(SDL_LIBS) -lpthread

sim: sim.cpp $(FW) qr.o
	$(CXX) $(CXXFLAGS) sim.cpp ../SpinScreen/ui.cpp qr.o -o $@

bench: bench.cpp $(FW) qr.o
	$(CXX) $(CXXFLAGS) bench.cpp ../SpinScreen/ui.cpp qr.o -o $@

clean:
	rm -f virtual_esp sim bench dirty_test spinny_stress shots qr.o

.PHONY: all clean

dirty_test: dirty_test.cpp $(FW) qr.o
	$(CXX) $(CXXFLAGS) dirty_test.cpp ../SpinScreen/ui.cpp qr.o -o $@

spinny_stress: spinny_stress.cpp $(FW) qr.o
	$(CXX) $(CXXFLAGS) -DUI_PROBE spinny_stress.cpp ../SpinScreen/ui.cpp qr.o -o $@

shots: shots.cpp $(FW) qr.o
	$(CXX) $(CXXFLAGS) shots.cpp ../SpinScreen/ui.cpp qr.o -o $@
