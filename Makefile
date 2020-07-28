#
# $Id: Makefile,v 1.14 2007/03/08 02:13:53 datapoke Exp $
#
CXX = g++

CXX_FLAGS = -Wall -O3

LD_LIBS = -lpthread -lglut -lGL -ljack

all: xyscope

xyscope: xyscope.cpp Makefile
	${CXX} ${CXX_FLAGS} xyscope.cpp ${LD_FLAGS} ${LD_LIBS} -o xyscope

clean:
	rm -f core *.o xyscope
