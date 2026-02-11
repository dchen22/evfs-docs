cd ~/code/linux-6.8
make -j$(nproc)
sudo make modules_install
sudo make install
sudo update-grub

# now reboot