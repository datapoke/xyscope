#!/bin/bash
if [ -n "$1" ] ; then
    exec $@
fi

cd /usr/src/xyscope/release/
mkdir -p /usr/src/xyscope.dist/release/
cp -rvT linux/ /usr/src/xyscope.dist/release/linux/
