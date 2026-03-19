#!/bin/sh
# Build the toolchain image (cached after first run)
docker build --platform linux/amd64 -f Dockerfile-windows -t xyscope-windows .

# Compile using docker run (BuildKit RUN has linker issues on Apple Silicon)
docker run --platform linux/amd64 --rm \
    -v "$(pwd)":/src:ro \
    -v "$(pwd)/release/windows":/out \
    xyscope-windows bash -c '
    x86_64-w64-mingw32-gcc -O3 -c /src/winmain.c -o /tmp/winmain.o
    x86_64-w64-mingw32-g++ -O3 -std=c++11 -x c++ -c /src/xyscope.mm \
        -I/usr/x86_64-w64-mingw32/include/SDL2 -o /tmp/xyscope.o
    x86_64-w64-mingw32-g++ /tmp/winmain.o /tmp/xyscope.o \
        -L/usr/x86_64-w64-mingw32/lib \
        -lSDL2 -lSDL2_ttf -lfftw3 \
        -lopengl32 -lole32 -luuid -lwinmm \
        -mwindows -static-libgcc -static-libstdc++ \
        -o /out/xyscope.exe
    x86_64-w64-mingw32-gcc -O3 /src/xyscope-calibrate.c \
        -I/usr/x86_64-w64-mingw32/include/SDL2 \
        -L/usr/x86_64-w64-mingw32/lib \
        -lSDL2 -lm \
        -mwindows -static-libgcc \
        -o /out/xyscope-calibrate.exe
    x86_64-w64-mingw32-objcopy --subsystem=console /out/xyscope-calibrate.exe
    cp /usr/x86_64-w64-mingw32/bin/SDL2.dll /out/
    cp /usr/x86_64-w64-mingw32/bin/SDL2_ttf.dll /out/
    cp /usr/x86_64-w64-mingw32/lib/libfftw3-3.dll /out/
'
