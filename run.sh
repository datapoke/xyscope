#!/bin/sh
if [ -n "$1" ] ; then
    export JACK_DEVICE=$1
fi
if [ -n "$2" ] ; then
    MUSIC_DIR=$2
fi
docker build -t xyscope .
docker run -it --rm --name xyscope1 \
		-v $(pwd):/usr/src/xyscope \
		-v /tmp/.X11-unix:/tmp/.X11-unix \
		-v $HOME/.Xauthority:/root/.Xauthority \
		-v $MUSIC_DIR:/home/user/Music \
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
