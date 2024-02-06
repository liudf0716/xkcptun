#!/usr/bin/env bash

sudo apt-get -y install libevent-dev
mkdir build && cd build
cmake -DBUILD_STATIC_LINK=yes ..
make
