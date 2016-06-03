#!/bin/bash
set -e

RESULTS_DIR=`date +%Y%m%d_%H%M%S`
mkdir $RESULTS_DIR

[ -e /dev/nvme0n1 ] || (echo "No /dev/nvme0n1 device node found." && exit 1)

echo -n Building RocksDB...
[ -e db_bench ] || make -j16 release &> $RESULTS_DIR/build_log.txt
echo done.

echo -n Creating and mounting XFS filesystem...
sudo mkdir -p /mnt/rocksdb
sudo umount /mnt/rocksdb || true
sudo mkfs.xfs -d agcount=32 -l su=4096 -f /dev/nvme0n1 &> $RESULTS_DIR/mkfs_xfs.txt
sudo mount /dev/nvme0n1 /mnt/rocksdb
sudo chown $USER /mnt/rocksdb
echo done.

echo -n Inserting keys and values into database...
/usr/bin/time perf record ./db_bench --flagfile=./seq_insert_flags.txt &> $RESULTS_DIR/db_bench_insert.txt
echo done.

echo -n Generating perf report for insertion phase...
mv perf.data $RESULTS_DIR/insert.perf.data
sudo perf report -f -n -i $RESULTS_DIR/insert.perf.data | sed '/#/d' | sed '/%/!d' | sort -r > $RESULTS_DIR/insert.perf.txt
rm $RESULTS_DIR/insert.perf.data
./postprocess.py $RESULTS_DIR insert > $RESULTS_DIR/insert_summary.txt
echo done.

echo -n Overwriting keys and values in database...
/usr/bin/time perf record ./db_bench --flagfile=./overwrite_flags.txt &> $RESULTS_DIR/db_bench_overwrite.txt
echo done.

echo -n Generating perf report for overwrite phase...
mv perf.data $RESULTS_DIR/overwrite.perf.data
sudo perf report -f -n -i $RESULTS_DIR/overwrite.perf.data | sed '/#/d' | sed '/%/!d' | sort -r > $RESULTS_DIR/overwrite.perf.txt
rm $RESULTS_DIR/overwrite.perf.data
./postprocess.py $RESULTS_DIR overwrite > $RESULTS_DIR/overwrite_summary.txt
echo done.

