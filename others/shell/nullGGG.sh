#!/bin/sh
set -x
./createPblkDevice.sh
resultFolder="./resultNull"
for i in {8,16,15}
do
fio -numjobs=$i -runtime=30 fionulldev_randread > ${resultFolder}/"proc_"$i"_null_rr"
fio -numjobs=$i -runtime=30 fionulldev_randwrite > ${resultFolder}/"proc_"$i"_null_rw"
done
#fio fiopblkdev_randread > ${resultFolder}/"proc_"$(nproc)"_pblk_rr1"
#fio fiopblkdev_randwrite > ${resultFolder}/"proc_"$(nproc)"_pblk_rw"
#fio fiopblkdev_randread > ${resultFolder}/"proc_"$(nproc)"_pblk_rr2"
#fio fionulldev_randread > ${resultFolder}/"proc_"$(nproc)"_null_rr"
#fio fionulldev_randwrite > ${resultFolder}/"proc_"$(nproc)"_null_rw"
