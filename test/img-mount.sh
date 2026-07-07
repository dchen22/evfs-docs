#!/bin/bash
# this script should prepare the image and mount the file system

if (($EUID != 0)); then
    echo "You need ROOT priviledge"
    exit 1
fi

if [[ ! -v TEVFS_WORKSPACE ]]; then
    echo "workspace check fail"
    exit 1
fi

mkdir -p $TEVFS_MOUNTPT

if [[ $1 == "--no-data-journaling" ]]; then
	mount -o loop,data=writeback $TEVFS_IMAGEPATH $TEVFS_MOUNTPT
else
	mount -o loop,data=journal $TEVFS_IMAGEPATH $TEVFS_MOUNTPT
fi

chown $SUDO_USER:$SUDO_USER $TEVFS_MOUNTPT

