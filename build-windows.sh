#!/bin/sh
if [ -z "${UID}" ]; then
	UID=$(id -u)
fi

# Build the toolchain image (cached after first run)
docker build --platform linux/amd64 -f Dockerfile-windows -t xyscope-windows .

# Compile using docker run (BuildKit RUN has linker issues on Apple Silicon)
docker run --platform linux/amd64 --rm \
    -u ${UID} \
    -v "$(pwd)":/usr/src/xyscope.dist:Z \
    xyscope-windows bash -c '
	mkdir -p /usr/src/xyscope.dist/release/windows/
    x86_64-w64-mingw32-gcc -O3 -c /usr/src/xyscope/winmain.c -o /tmp/winmain.o
    x86_64-w64-mingw32-g++ -O3 -std=c++11 -x c++ -c /usr/src/xyscope/xyscope.mm \
        -I/usr/x86_64-w64-mingw32/include/SDL2 -o /tmp/xyscope.o
    x86_64-w64-mingw32-g++ /tmp/winmain.o /tmp/xyscope.o \
        -L/usr/x86_64-w64-mingw32/lib \
        -lSDL2 -lSDL2_ttf -lfftw3 \
        -lopengl32 -lole32 -luuid -lwinmm -ldxgi \
        -mwindows -static-libgcc -static-libstdc++ \
        -o /usr/src/xyscope.dist/release/windows/xyscope.exe
    x86_64-w64-mingw32-g++ -O3 -std=c++11 -x c++ /usr/src/xyscope/xyscope-calibrate.mm \
        -I/usr/x86_64-w64-mingw32/include/SDL2 \
        -L/usr/x86_64-w64-mingw32/lib \
        -lSDL2 -lopengl32 -lm \
        -mwindows -static-libgcc -static-libstdc++ \
        -o /usr/src/xyscope.dist/release/windows/xyscope-calibrate.exe
    x86_64-w64-mingw32-objcopy --subsystem=console /usr/src/xyscope.dist/release/windows/xyscope-calibrate.exe
    cp /usr/x86_64-w64-mingw32/bin/SDL2.dll        /usr/src/xyscope.dist/release/windows/
    cp /usr/x86_64-w64-mingw32/bin/SDL2_ttf.dll    /usr/src/xyscope.dist/release/windows/
    cp /usr/x86_64-w64-mingw32/lib/libfftw3-3.dll  /usr/src/xyscope.dist/release/windows/
'
