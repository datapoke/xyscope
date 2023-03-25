#
# $Id: Makefile,v 1.14 2007/03/08 02:13:53 datapoke Exp $
#
CXX = g++

CXX_FLAGS = -Wall -O3

LD_LIBS   = -lpthread -lglut -lGL -ljack
LD_LIBS2  = -lpthread -lglut -lGL -lpipewire-0.3
LD_FLAGS2 = -D_REENTRANT -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2

all: xyscope

xyscope: xyscope.cpp Makefile
	${CXX} ${CXX_FLAGS} xyscope.cpp ${LD_FLAGS} ${LD_LIBS} -o xyscope

xyscope2: xyscope2.cpp Makefile
	${CXX} ${CXX_FLAGS} xyscope2.cpp ${LD_FLAGS2} ${LD_LIBS2} -o xyscope2

clean:
	rm -f core *.o xyscope xyscope2
