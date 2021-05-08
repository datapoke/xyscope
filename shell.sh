#!/bin/sh
docker exec -it \
		--env XDG_RUNTIME_DIR=/tmp \
		--env WAYLAND_DISPLAY=$WAYLAND_DISPLAY \
		--env=DISPLAY \
		xyscope1 \
		bash
