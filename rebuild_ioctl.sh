#!/bin/bash
cd ~/code/linux-6.8

echo "==> Recompiling ioctl.c..."
make fs/ext4/ioctl.o

echo "==> Relinking kernel..."
make vmlinux

echo "==> Creating boot image..."
make arch/x86/boot/bzImage

echo "==> Installing kernel..."
sudo cp arch/x86/boot/bzImage /boot/vmlinuz-6.8.0-evfs-unstable

echo "==> Done! Reboot to load the new kernel."
echo "    sudo reboot"