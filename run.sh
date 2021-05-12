#!/bin/sh
docker build -t xyscope .
docker run -it --rm --name xyscope1 \
		-v $(pwd):/usr/src/xyscope \
		-v $HOME/Music:/home/user/Music \
		-v /tmp/.X11-unix:/tmp/.X11-unix \
		-v $HOME/.Xauthority:/root/.Xauthority \
		-v $XDG_RUNTIME_DIR:$XDG_RUNTIME_DIR \
		--env=XDG_RUNTIME_DIR \
		--env=WAYLAND_DISPLAY \
		--env=DISPLAY \
		--device=/dev/snd:/dev/snd \
		--ipc=host \
		--net=host \
		--privileged \
		xyscope \
		/usr/src/xyscope/docker-entrypoint.sh
#		-v /var/run/dbus:/var/run/dbus \
