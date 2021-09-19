#!/bin/sh
if [ -n "$1" ] ; then
    export JACK_DEVICE=$1
fi
if [ -n "$MUSIC_DIR" ] ; then
    MUSIC_DIR=$2
fi
docker build -t xyscope --build-args MUSIC_DIR=$(MUSIC_DIR) .
docker run -it --rm --name xyscope1 \
		-v $(pwd):/usr/src/xyscope \
		-v /tmp/.X11-unix:/tmp/.X11-unix \
		-v $HOME/.Xauthority:/root/.Xauthority \
		-v $XDG_RUNTIME_DIR:$XDG_RUNTIME_DIR \
		--env=XDG_RUNTIME_DIR \
		--env=WAYLAND_DISPLAY \
		--env=DISPLAY \
		--env=JACK_DEVICE \
		--device=/dev/snd:/dev/snd \
		--ipc=host \
		--net=host \
		--privileged \
		--ulimit rtprio=99 \
		--ulimit memlock=549755813888 \
		xyscope
