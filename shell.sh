#!/bin/sh
docker exec -it \
		-u user \
		--env=XDG_RUNTIME_DIR \
		--env=WAYLAND_DISPLAY \
		--env=DISPLAY \
		xyscope1 \
		bash
