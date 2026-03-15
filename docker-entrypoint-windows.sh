#!/bin/bash
if [ -n "$1" ] ; then
    exec $@
fi

cd /usr/src/xyscope-windows/release/
mkdir -p /usr/src/xyscope.dist/release/
cp -rvT windows/ /usr/src/xyscope.dist/release/windows/
