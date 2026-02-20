#!/bin/bash

if (($EUID != 0)); then
    echo "You need to run this with root priviledge" >& 2
    exit 1
fi

WORKPLACE=$(dirname "$0")
SIZE="1M"
NUM_INODES="120"
IMAGEDIR="/tmp/ext4-$SIZE-$NUM_INODES.img"
MOUNTPT="/tmp/a"

cd $WORKPLACE

make clean
make

truncate -s $SIZE $IMAGEDIR
mkfs.ext4 $IMAGEDIR -b 1024 -N $NUM_INODES
mount -o loop --mkdir $IMAGEDIR $MOUNTPT
./test-hello $MOUNTPT

sleep 1 # add some delay to wait for printk buffer flushed

if dmesg | grep "bad apple" > /dev/null; then
    echo "OK"
else
    echo "Failed"
fi

umount $IMAGEDIR
rm $IMAGEDIR
