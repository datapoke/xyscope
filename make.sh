#!/bin/sh
docker build -t xyscope .
docker run --rm -it \
		-v $(pwd):/usr/src/xyscope \
		xyscope \
		make
