#!/bin/bash

export TEVFS_WORKSPACE=$(dirname "$0")
cd $TEVFS_WORKSPACE

source img-var.sh

echo "$TEVFS_SIZE $TEVFS_NUM_INODES $TEVFS_IMAGEPATH $TEVFS_MOUNTPT"
rm -f $TEVFS_IMAGEPATH

truncate -s $TEVFS_SIZE $TEVFS_IMAGEPATH

if ! sudo -E ./img-mkfs.sh; then
	exit 1
fi

if ! sudo -E ./img-mount.sh --no-data-journaling; then
	exit 1
fi

fname1="a"
fname2="b"
file1=$TEVFS_MOUNTPT/"$fname1"
file2=$TEVFS_MOUNTPT/"$fname2"
	
if [[ $1 == "-n" || $1 == "--native" ]]; then
	rm build/extent_move.n.x
	make build/extent_move.n.x &> /dev/null
	
	tr '\0' 'A' < /dev/zero | dd of=$file1 bs=1024 count=4 conv=fdatasync 2>/dev/null
	tr '\0' 'B' < /dev/zero | dd of=$file2 bs=1024 count=4 conv=fdatasync 2>/dev/null
	
	echo -e "\n=== before :: file 1 ==="
	hexdump -C $file1
	
	echo -e "\n=== before :: file 2 ==="
	hexdump -C $file2
	
	sudo sync
	echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
	
	echo -e "\nrunning build/extent_move.n.x 0 1 $file1 $file2"
	build/extent_move.n.x 0 1 $file1 $file2
	
	echo "exit code: $?"
	sudo sync
	
	echo -e "\n=== after :: file 1 ==="
	hexdump -C $file1
	
	echo -e "\n=== after :: file 2 ==="
	res=$(hexdump -C $file2)
	echo -e "$res"

else
	rm build/balloc.x build/bfree.x build/iver.x build/extent_move.c.x
	make build/balloc.x build/bfree.x build/iver.x build/extent_move.c.x &> /dev/null
	
	tr '\0' 'A' < /dev/zero | dd of=$file1 bs=1024 count=2 conv=fdatasync 2>/dev/null	
	tr '\0' 'A' < /dev/zero | dd of=$file1 bs=1024 seek=3 count=2 conv=fdatasync 2>/dev/null
	sudo sync

	ino_num=$(stat $file1 | head -n3 | tail -n1 | awk '{print $4}')

	tr '\0' 'A' < /dev/zero | dd of=$file1 bs=1024 seek=6 count=2 conv=fdatasync 2>/dev/null
	sudo sync

	echo -e "\n=== before :: file 1 ==="
	hexdump -C $file1

	echo ""
	res=$(sudo debugfs -n -R "stat <$ino_num>" $TEVFS_IMAGEPATH)
	echo -e "$res"
	
	e_len=5
	echo -e "\nrunning build/balloc.x 2001-2005 $TEVFS_MOUNTPT"
	for (( i=2001; i < $((2001 + $e_len)); i++ )); do
		build/balloc.x $i $TEVFS_MOUNTPT
	done

	sudo sync
	echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
	
	echo -e "\nrunning build/iver.x $file1"
	iver=$(build/iver.x $file1)
	echo -e "$iver"

	echo -e "\nrunning build/extent_move.c.x $iver $ino_num 1 2001 $e_len $TEVFS_MOUNTPT"
	res=$(build/extent_move.c.x $iver $ino_num 1 2001 $e_len $TEVFS_MOUNTPT)
	echo -e "$res"
	
	echo -e "\nexit code: $?"
	sudo sync

	echo -e "\n=== after :: file 1 ==="
	hexdump -C $file1

	echo ""
	sudo debugfs -n -R "stat <$ino_num>" $TEVFS_IMAGEPATH | xargs -I {} echo "{}"

	# TODO free the blocks return by the program
fi

echo ""

if ! sudo -E ./img-umount.sh; then
	exit 1
fi


echo ""
echo "------- new fsck test ---------"
sudo fsck.ext4 -f -n $TEVFS_IMAGEPATH


if [[ $1 == "-n" || $1 == "--native" ]]; then
	if echo $res | grep "0400" > /dev/null; then
		echo "OK"
	else
		echo "Failed"
	fi
else
	nums=$(echo -e "$res" | tail +2 | awk '{print $3}')
        sum=0
	for num in $nums; do
		sum=$(($sum + $num))
	done

	if [ "$sum" -eq "$e_len" ]; then
		echo "OK"
	else
		echo "Failed"
	fi
fi
