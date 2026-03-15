#!/bin/sh
docker build -t xyscope .
docker run -it --rm -v $(pwd):/usr/src/xyscope.dist xyscope
