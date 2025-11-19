#
# $Id: Makefile,v 1.14 2007/03/08 02:13:53 datapoke Exp $
#

# Detect operating system
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS
    CXX = clang++
    CXX_FLAGS = -Wall -O3 -std=c++11 -fobjc-arc -I/opt/homebrew/include
    LD_LIBS = -lpthread -L/opt/homebrew/lib -lSDL2 -lSDL2_ttf -framework OpenGL -framework Accelerate -framework Foundation -framework ScreenCaptureKit -framework AVFoundation -framework CoreMedia
    SRC = xyscope.mm
else
    # Linux
    CXX = g++
    CXX_FLAGS = -Wall -O3 -march=native -mtune=native
    LD_LIBS = -lpthread -lglut -lGL -ljack -lfftw3
    SRC = xyscope.cpp
endif

all: xyscope

xyscope: $(SRC) Makefile
	${CXX} ${CXX_FLAGS} $(SRC) ${LD_FLAGS} ${LD_LIBS} -o xyscope

clean:
	rm -f core *.o xyscope
