#!/bin/sh
docker build -t xyscope .
docker run -it --rm --name xyscope1 -v $(pwd):/usr/src/xyscope.dist xyscope
