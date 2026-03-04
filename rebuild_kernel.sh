#!/bin/bash

# exit immediately if a command fails
set -e 

# Capture the start time
START_TIME=$(date +%s)

cd /home/evie/code/linux-6.8
make -j$(nproc)
sudo make modules_install
sudo make install
sudo update-grub

# Capture the end time
END_TIME=$(date +%s)

# Calculate duration
ELAPSED_TIME=$((END_TIME - START_TIME))
MINUTES=$((ELAPSED_TIME / 60))
SECONDS=$((ELAPSED_TIME % 60))

echo "---"
echo "✅ Done! Kernel rebuilt and installed successfully."
echo "⏱️  Total time taken: ${MINUTES}m ${SECONDS}s"
echo "---"

# now reboot