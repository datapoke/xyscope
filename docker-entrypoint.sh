#!/bin/sh
apt install -y jackd1
cd /usr/src/xyscope
make
su user -c "jackd -R -d alsa -r 192000 -p 512 -d hw:1" &
while [ 1 ] ; do sleep 5 ; su user -c "./xyscope" ; done
