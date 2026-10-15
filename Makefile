# wispviz - needs a MinGW-w64 toolchain (g++ and windres), e.g. the portable w64devkit. Run:  make
CXX      ?= g++
WINDRES  ?= windres
CXXFLAGS  = -Os -s -mwindows -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections
LIBS      = -luser32 -lgdi32 -lole32 -luuid -lshell32 -ladvapi32

wispviz.exe: src/main.cpp src/media.cpp src/media.h src/wispviz.rc assets/wispviz.ico
	$(WINDRES) src/wispviz.rc -O coff -o icon.o
	$(CXX) $(CXXFLAGS) src/main.cpp src/media.cpp icon.o -o $@ $(LIBS)

clean:
	rm -f wispviz.exe icon.o
