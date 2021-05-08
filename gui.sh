#!/bin/sh
docker build -t xyscope .
xhost +SI:localuser:root
docker run -it --rm --name xyscope1 \
		-v $(pwd):/usr/src/xyscope \
		-v $HOME/Music:/root/Music \
		-v /tmp/.X11-unix:/tmp/.X11-unix \
		-v $HOME/.Xauthority:/root/.Xauthority:rw \
		-v $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:/tmp/$WAYLAND_DISPLAY  \
		--env XDG_RUNTIME_DIR=/tmp \
		--env WAYLAND_DISPLAY=$WAYLAND_DISPLAY \
		--env=DISPLAY \
		--device=/dev/snd:/dev/snd \
		--ipc=host \
		--net=host \
		xyscope \
		/usr/src/xyscope/docker-entrypoint.sh
