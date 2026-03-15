#!/bin/sh
docker build -f Dockerfile-windows -t xyscope-windows .
docker run -it --rm -v $(pwd):/usr/src/xyscope.dist xyscope-windows
