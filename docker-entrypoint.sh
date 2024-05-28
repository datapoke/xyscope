#!/bin/bash
if [ -n "$1" ] ; then
    exec $@
fi

cd /usr/src/xyscope
cp -v xyscope /usr/src/xyscope.dist/
