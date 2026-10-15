@echo off
rem Build wispviz with MinGW-w64 (g++ and windres on PATH, e.g. w64devkit).
windres src\wispviz.rc -O coff -o icon.o
g++ -Os -s -mwindows -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections src\main.cpp src\media.cpp icon.o -o wispviz.exe -luser32 -lgdi32 -lole32 -luuid -lshell32 -ladvapi32
if errorlevel 1 (echo BUILD FAILED) else (echo OK: wispviz.exe)
