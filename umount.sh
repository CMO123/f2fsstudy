umount /dev/nvme0n1
sleep 1
nvme lnvm remove -n mylightpblk
sleep 1
rmmod f2fs.ko