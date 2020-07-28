#
# $Id: Makefile,v 1.14 2007/03/08 02:13:53 datapoke Exp $
#
CXX = g++

CXX_FLAGS = -Wall -O3
# CXX_FLAGS = -Wall -O3 -march=pentium4 -mfpmath=sse
#CXX_FLAGS = -Wall -O3 -D_REENTRANT -mcpu=7400 -mhard-float -mpowerpc-gfxopt
#CXX_FLAGS = -Wall -g -ggdb -pg

#LD_FLAGS = -I/usr/X11R6/include -I/usr/local/include \
#           -L/usr/X11R6/lib -L/usr/local/lib
#LD_FLAGS = -I/usr/X11R6/include -I/sw/include \
#           -L/usr/X11R6/lib -L/sw/lib

LD_LIBS = -lpthread -lglut -lGL -ljack
#LD_LIBS = -framework glut -framework OpenGL -framework jack

all: xyscope

xyscope: xyscope.cpp Makefile
	${CXX} ${CXX_FLAGS} xyscope.cpp ${LD_FLAGS} ${LD_LIBS} -o xyscope

clean:
	rm -f core *.o xyscope

distclean:
	rm -rf core *.o xyscope CVS English.lproj/CVS xyscope.xcodeproj/CVS \
               .DS_Store build xyscope.xcodeproj/*.mode1 \
               xyscope.xcodeproj/*.pbxuser
