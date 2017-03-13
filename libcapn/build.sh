#!/bin/bash

if [ ! -n "$LIB_APN_PREFIX" ]; then
	LIB_APN_PREFIX='/usr'
else
	echo '${LIB_APN_PREFIX}/lib/capn' > /etc/ld.so.conf.d/carusto-freeswitch-apn.conf
fi

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${LIB_APN_PREFIX}/ ../
make
make install

