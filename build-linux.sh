#!/bin/sh
docker build --platform linux/amd64 -t xyscope .
docker run --platform linux/amd64 --rm -v "$(pwd)":/usr/src/xyscope.dist xyscope
