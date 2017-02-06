#!/bin/bash
set -e

hash mkfs.xfs
hash perf
hash python
[ -e /usr/include/gflags/gflags.h ] || (echo "gflags not installed." && exit 1)

# Increase max number of file descriptors.  This will be inherited
#  by processes spawned from this script.
ulimit -n 16384

INITIAL_DIR=$PWD
mkdir -p results/old
# if there are any existing test results, move them into the "old" directory
ls results/testrun_* &> /dev/null && mv results/testrun_* results/old

RESULTS_DIR=$INITIAL_DIR/results/testrun_`date +%Y%m%d_%H%M%S`
mkdir $RESULTS_DIR
rm -f $INITIAL_DIR/results/last
ln -s $RESULTS_DIR $INITIAL_DIR/results/last

: ${CACHE_SIZE:=4096}
: ${DURATION:=120}
: ${NUM_KEYS:=500000000}

if [ "$NO_SPDK" = "1" ]
then
	[ -e /dev/nvme0n1 ] || (echo "No /dev/nvme0n1 device node found." && exit 1)
else
	[ -e /dev/nvme0n1 ] && (echo "/dev/nvme0n1 device found - need to run SPDK setup.sh script to bind to UIO." && exit 1)
fi

echo -n Building RocksDB...
[ -e db_bench ] || make -j16 db_bench DEBUG_LEVEL=0 &> $RESULTS_DIR/build_log.txt
echo done.

cd $RESULTS_DIR

SYSINFO_FILE=$RESULTS_DIR/sysinfo.txt
COMMAND="hostname"
echo ">> $COMMAND : " >> $SYSINFO_FILE
$COMMAND >> $SYSINFO_FILE
echo >> $SYSINFO_FILE

COMMAND="cat /proc/cpuinfo"
echo ">> $COMMAND : " >> $SYSINFO_FILE
$COMMAND >> $SYSINFO_FILE
echo >> $SYSINFO_FILE

COMMAND="cat /proc/meminfo"
echo ">> $COMMAND : " >> $SYSINFO_FILE
$COMMAND >> $SYSINFO_FILE
echo >> $SYSINFO_FILE

if [ "$NO_SPDK" = "1" ]
then
	echo -n Creating and mounting XFS filesystem...
	sudo mkdir -p /mnt/rocksdb
	sudo umount /mnt/rocksdb || true &> /dev/null
	sudo mkfs.xfs -d agcount=32 -l su=4096 -f /dev/nvme0n1 &> mkfs_xfs.txt
	sudo mount -o discard /dev/nvme0n1 /mnt/rocksdb
	sudo chown $USER /mnt/rocksdb
	echo done.
fi

cp $INITIAL_DIR/common_flags.txt insert_flags.txt
echo "--benchmarks=fillseq" >> insert_flags.txt
echo "--threads=1" >> insert_flags.txt
echo "--disable_wal=1" >> insert_flags.txt
echo "--use_existing_db=0" >> insert_flags.txt
echo "--num=$NUM_KEYS" >> insert_flags.txt

cp $INITIAL_DIR/common_flags.txt randread_flags.txt
echo "--benchmarks=readrandom" >> randread_flags.txt
echo "--threads=16" >> randread_flags.txt
echo "--duration=$DURATION" >> randread_flags.txt
echo "--disable_wal=1" >> randread_flags.txt
echo "--use_existing_db=1" >> randread_flags.txt
echo "--num=$NUM_KEYS" >> randread_flags.txt

cp $INITIAL_DIR/common_flags.txt overwrite_flags.txt
echo "--benchmarks=overwrite" >> overwrite_flags.txt
echo "--threads=1" >> overwrite_flags.txt
echo "--duration=$DURATION" >> overwrite_flags.txt
echo "--disable_wal=1" >> overwrite_flags.txt
echo "--use_existing_db=1" >> overwrite_flags.txt
echo "--num=$NUM_KEYS" >> overwrite_flags.txt

cp $INITIAL_DIR/common_flags.txt readwrite_flags.txt
echo "--benchmarks=readwhilewriting" >> readwrite_flags.txt
echo "--threads=4" >> readwrite_flags.txt
echo "--duration=$DURATION" >> readwrite_flags.txt
echo "--disable_wal=1" >> readwrite_flags.txt
echo "--use_existing_db=1" >> readwrite_flags.txt
echo "--num=$NUM_KEYS" >> readwrite_flags.txt

run_step() {
	if [ -z "$1" ]
	then
		echo run_step called with no parameter
		exit 1
	fi

	if [ -z "$NO_SPDK" ]
	then
	  echo "--spdk=/usr/local/etc/spdk/rocksdb.conf" >> "$1"_flags.txt
	  echo "--spdk_cache_size=$CACHE_SIZE" >> "$1"_flags.txt
	fi

	if [ "$NO_SPDK" = "1" ]
	then
	  cat /sys/block/nvme0n1/stat > "$1"_blockdev_stats.txt
	fi

	echo -n Start $1 test phase...
	sudo /usr/bin/time taskset 0xFFF perf record $INITIAL_DIR/db_bench --flagfile="$1"_flags.txt &> "$1"_db_bench.txt
	echo done.

	if [ "$NO_SPDK" = "1" ]
	then
	  cat /sys/block/nvme0n1/stat >> "$1"_blockdev_stats.txt
	fi

	echo -n Generating perf report for $1 test phase...
	sudo perf report -f -n | sed '/#/d' | sed '/%/!d' | sort -r > $1.perf.txt
	sudo rm perf.data
	$INITIAL_DIR/spdk_postprocess.py `pwd` $1 > $1_summary.txt
	echo done.
}

run_step insert
run_step randread
run_step overwrite
run_step readwrite

if [ "$NO_SPDK" = "1" ]
then
	echo -n Unmounting XFS filesystem...
	sudo umount /mnt/rocksdb || true &> /dev/null
	echo done.
fi
