#!/bin/bash
#
# Cross-compile XYScope for Windows using Docker + MinGW-w64
# Produces release/xyscope.exe with required DLLs
#

set -e

IMAGE=xyscope-win

echo "Building Docker image..."
docker build -f Dockerfile.windows -t $IMAGE .

echo "Cross-compiling for Windows..."
docker run --rm -v "$(pwd)":/src $IMAGE

echo ""
echo "Done! Windows build:"
ls -lh release/xyscope.exe release/*.dll
