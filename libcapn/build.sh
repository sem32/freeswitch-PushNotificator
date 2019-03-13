#!/bin/bash

if [ ! -n "$LIB_APN_PREFIX" ]; then
	LIB_APN_PREFIX='/usr'
else
	echo '${LIB_APN_PREFIX}/lib/capn' > /etc/ld.so.conf.d/carusto-freeswitch-apn.conf
fi

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${LIB_APN_PREFIX}/ ../libcapn
make
make install

# fix for library path for some nix systems
ln -sf ${LIB_APN_PREFIX}/lib64/capn ${LIB_APN_PREFIX}/lib/capn
