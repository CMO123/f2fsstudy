/root/qhw/femu/build-femu/x86_64-softmmu/qemu-system-x86_64 \
-name "pmy-FEMU-SSD" \
-m 32G -smp 32 --enable-kvm \
-hda image-revise/centos7-revise.qcow2 \
-device virtio-scsi-pci,id=scsi0 \
-drive file=image-revise/vssd1.raw,if=none,aio=threads,format=raw,id=id0 \
-device nvme,drive=id0,serial=serial0,id=nvme0,namespaces=1,lver=1,lmetasize=16,ll2pmode=0,nlbaf=5,lba_index=3,mdts=10,lnum_ch=32,lnum_lun=1,lnum_pln=1,lsec_size=4096,lsecs_per_pg=1,lpgs_per_blk=512,ldebug=1,femu_mode=0 \
-net nic,macaddr=52:54:00:12:34:78 -net tap,ifname=tap1,script=./qemu-ifup.sh,downscript=no \
-qmp unix:./qmp-sock,server,nowait \
-k en-us \
-sdl
#-hdb image-revise/testdisk.qcow2 \
