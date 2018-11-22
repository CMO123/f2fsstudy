f2fs-tools-1.11.0/mkfs/mkfs.f2fs /dev/nvme0n1
insmod f2fs.ko
mount -t f2fs /dev/nvme0n1 /root/test/
