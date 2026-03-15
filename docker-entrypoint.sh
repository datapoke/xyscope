#!/bin/bash
if [ -n "$1" ] ; then
    exec $@
fi

cd /usr/src/xyscope
cp -rv release /usr/src/xyscope.dist/
