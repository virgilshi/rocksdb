#!/bin/bash
set -e

# Increase max number of file descriptors.  This will be inherited
#  by processes spawned from this script.
ulimit -n 16384

RESULTS_DIR=`date +%Y%m%d_%H%M%S`
mkdir $RESULTS_DIR

[ -e /dev/nvme0n1 ] || (echo "No /dev/nvme0n1 device node found." && exit 1)

echo -n Building RocksDB...
[ -e db_bench ] || make -j16 release &> $RESULTS_DIR/build_log.txt
echo done.

cd $RESULTS_DIR

echo -n Creating and mounting XFS filesystem...
sudo mkdir -p /mnt/rocksdb
sudo umount /mnt/rocksdb || true
sudo mkfs.xfs -d agcount=32 -l su=4096 -f /dev/nvme0n1 &> mkfs_xfs.txt
sudo mount /dev/nvme0n1 /mnt/rocksdb
sudo chown $USER /mnt/rocksdb
echo done.

cp ../common_flags.txt insert_flags.txt
echo "--benchmarks=fillseq" >> insert_flags.txt
echo "--threads=1" >> insert_flags.txt
echo "--disable_wal=1" >> insert_flags.txt
echo "--use_existing_db=0" >> insert_flags.txt

cp ../common_flags.txt overwrite_flags.txt
echo "--benchmarks=overwrite" >> overwrite_flags.txt
echo "--threads=1" >> overwrite_flags.txt
echo "--duration=120" >> overwrite_flags.txt
echo "--disable_wal=0" >> overwrite_flags.txt
echo "--use_existing_db=1" >> overwrite_flags.txt

cp ../common_flags.txt readwrite_flags.txt
echo "--benchmarks=readwhilewriting" >> readwrite_flags.txt
echo "--threads=4" >> readwrite_flags.txt
echo "--duration=120" >> readwrite_flags.txt
echo "--disable_wal=0" >> readwrite_flags.txt
echo "--use_existing_db=1" >> readwrite_flags.txt

echo -n Inserting keys and values into database...
cat /sys/block/nvme0n1/stat > blockdev_stats_insert.txt
/usr/bin/time perf record ../db_bench --flagfile=insert_flags.txt &> db_bench_insert.txt
cat /sys/block/nvme0n1/stat >> blockdev_stats_insert.txt
echo done.

echo -n Generating perf report for insertion phase...
sudo perf report -f -n | sed '/#/d' | sed '/%/!d' | sort -r > insert.perf.txt
rm perf.data
../postprocess.py `pwd` insert > insert_summary.txt
echo done.

echo -n Overwriting keys and values in database...
cat /sys/block/nvme0n1/stat > blockdev_stats_overwrite.txt
/usr/bin/time perf record ../db_bench --flagfile=overwrite_flags.txt &> db_bench_overwrite.txt
cat /sys/block/nvme0n1/stat >> blockdev_stats_overwrite.txt
echo done.

echo -n Generating perf report for overwrite phase...
sudo perf report -f -n | sed '/#/d' | sed '/%/!d' | sort -r > overwrite.perf.txt
rm perf.data
../postprocess.py `pwd` overwrite > overwrite_summary.txt
echo done.

echo -n Reading and overwriting keys and values in database...
cat /sys/block/nvme0n1/stat > blockdev_stats_readwrite.txt
/usr/bin/time perf record ../db_bench --flagfile=readwrite_flags.txt &> db_bench_readwrite.txt
cat /sys/block/nvme0n1/stat >> blockdev_stats_readwrite.txt
echo done.

echo -n Generating perf report for read/write phase...
sudo perf report -f -n | sed '/#/d' | sed '/%/!d' | sort -r > readwrite.perf.txt
rm perf.data
../postprocess.py `pwd` readwrite > readwrite_summary.txt
echo done.

