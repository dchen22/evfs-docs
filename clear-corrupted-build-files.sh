cd /home/evie/code/linux-6.8
rm arch/x86/xen/mmu.o || true
rm vmlinux.a vmlinux.o || true
find . -name ".*.cmd" -delete