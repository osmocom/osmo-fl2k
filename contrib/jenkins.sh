#!/bin/sh -ex
# This is how we build on jenkins.osmocom.org.

CFLAGS="-Werror"

if ! [ -x "$(command -v osmo-clean-workspace.sh)" ]; then
	echo "Error: We need to have scripts/osmo-clean-workspace.sh from osmo-ci.git in PATH!"
	exit 2
fi

osmo-clean-workspace.sh
cmake \
	-DINSTALL_UDEV_RULES=ON \
	-DCMAKE_C_FLAGS="$CFLAGS" \
	.
make $PARALLEL_MAKE
make DESTDIR="_install" install
osmo-clean-workspace.sh
