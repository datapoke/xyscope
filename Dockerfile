FROM debian:bullseye-slim

RUN apt-get update && \
	apt-get install -y --no-install-recommends \
		build-essential \
		freeglut3-dev \
		libjack-dev \
		pulseaudio-module-jack \
	&& rm -rf /var/lib/apt/lists/*

COPY ./                     /usr/src/xyscope
COPY ./docker-entrypoint.sh /usr/local/bin/

RUN    cd /usr/src/xyscope \
    && make

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
