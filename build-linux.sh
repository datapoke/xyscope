#!/bin/sh
docker build --platform linux/amd64 -t xyscope .
docker run --platform linux/amd64 --rm \
    -u "$(id -u):$(id -g)" \
    -v "$(pwd)":/usr/src/xyscope.dist:Z \
    xyscope
