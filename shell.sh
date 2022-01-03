#!/bin/sh
docker exec -it \
		-u user \
		--env=XDG_RUNTIME_DIR \
		--env=WAYLAND_DISPLAY \
		--env=DISPLAY \
		-w /home/user/Music \
		xyscope1 \
		bash
