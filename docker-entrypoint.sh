#!/bin/sh
apt install -y jackd1
cd /usr/src/xyscope
make
jackd -r -d alsa -r 96000 -p 512 -d hw:2 &
while [ 1 ] ; do ./xyscope ; sleep 5 ; done
