#!/bin/bash
if [ -n "$1" ] ; then
    exec $@
fi
if [ -z "${JACK_DEVICE}" ] ; then
    JACK_DEVICE=hw:0
fi

cd /usr/src/xyscope
cp -v xyscope /usr/src/xyscope.dist/

#apt install -y jackd1
#su user -c "jackd -R -d alsa -r 192000 -p 512 -d ${JACK_DEVICE}" &
# su user -c "mkdir -p /home/user/.config/pulse"
# su user -c "cp /usr/src/xyscope/default.pa /home/user/.config/pulse/"
# su user -c "pulseaudio -D"
# su user -c "pactl load-module module-jack-sink channels=2"
# su user -c "pactl load-module module-jack-source channels=2"
# su user -c "pacmd set-default-sink jack_out"
#while [ 1 ] ; do sleep 5 ; su user -c "./xyscope" ; done

# make monitor
# su user -c "gdb ./monitor"
# exec bash
