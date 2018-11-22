umount /dev/nvme0n1
rmmod f2fs.ko
nvme lnvm remove -n mylightpblk
