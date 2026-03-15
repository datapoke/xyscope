#!/bin/sh
docker build -f Dockerfile.windows -t xyscope-win .
docker run -it --rm -v $(pwd):/usr/src/xyscope.dist xyscope-win
