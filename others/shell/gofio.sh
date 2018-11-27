#!/bin/sh
set -x
./createPblkDevice.sh
resultFolder="./result"
for i in {1} # i indicates round
do
for j in {1,2,4,8,16} # j indicates threadnum
do
#fio fiopblkdev_randread > ${resultFolder}/"proc_"$(nproc)"_pblk_rr1"
fio -numjobs=$j fiopblkdev_randwrite > ${resultFolder}/"njob_"$j"_Round_"$i"_pblk_rw"
fio -numjobs=$j fiopblkdev_randread > ${resultFolder}/"njob_"$j"_Round_"$i"_pblk_rr"
done
done
#sync
#sleep 2
#shutdown -h now
