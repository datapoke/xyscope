FROM debian:buster-slim

RUN apt-get update && \
	apt-get install -y --no-install-recommends \
		build-essential \
        mpv \
		libjack-dev \
		freeglut3-dev 
	# && rm -rf /var/lib/apt/lists/*

WORKDIR /root/Music
