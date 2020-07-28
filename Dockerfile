FROM debian:buster-slim

RUN apt-get update && \
	apt-get install -y --no-install-recommends \
		build-essential \
		libjack-jackd2-dev \
		freeglut3-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/xyscope
