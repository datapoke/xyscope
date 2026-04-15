#!/bin/sh
if [ -z "${UID}" ]; then
	UID=$(id -u)
fi
docker build --platform linux/amd64 -t xyscope .
docker run --platform linux/amd64 --rm \
    -u ${UID} \
    -v "$(pwd)":/usr/src/xyscope.dist:Z \
    xyscope
