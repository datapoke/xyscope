#!/bin/sh
sudo -u docker docker build -t xyscope .
sudo -u docker docker run -it --rm -v $(pwd):/usr/src/xyscope.dist xyscope
