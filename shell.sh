#!/bin/sh
docker exec -it \
		-u user \
		--env XDG_RUNTIME_DIR=/tmp \
		--env WAYLAND_DISPLAY=$WAYLAND_DISPLAY \
		--env=DISPLAY \
		xyscope1 \
		bash
