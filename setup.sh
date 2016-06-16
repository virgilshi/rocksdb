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

[ -e /dev/nvme0n1 ] || (echo "No /dev/nvme0n1 device node found." && exit 1)

echo -n Building RocksDB...
[ -e db_bench ] || make -j16 release &> $RESULTS_DIR/build_log.txt
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

echo -n Creating and mounting XFS filesystem...
sudo mkdir -p /mnt/rocksdb
sudo umount /mnt/rocksdb || true &> /dev/null
sudo mkfs.xfs -d agcount=32 -l su=4096 -f /dev/nvme0n1 &> mkfs_xfs.txt
sudo mount /dev/nvme0n1 /mnt/rocksdb
sudo chown $USER /mnt/rocksdb
echo done.

cp $INITIAL_DIR/common_flags.txt insert_flags.txt
echo "--benchmarks=fillseq" >> insert_flags.txt
echo "--threads=1" >> insert_flags.txt
echo "--disable_wal=1" >> insert_flags.txt
echo "--use_existing_db=0" >> insert_flags.txt

cp $INITIAL_DIR/common_flags.txt overwrite_flags.txt
echo "--benchmarks=overwrite" >> overwrite_flags.txt
echo "--threads=1" >> overwrite_flags.txt
echo "--duration=120" >> overwrite_flags.txt
echo "--disable_wal=0" >> overwrite_flags.txt
echo "--use_existing_db=1" >> overwrite_flags.txt

cp $INITIAL_DIR/common_flags.txt readwrite_flags.txt
echo "--benchmarks=readwhilewriting" >> readwrite_flags.txt
echo "--threads=4" >> readwrite_flags.txt
echo "--duration=120" >> readwrite_flags.txt
echo "--disable_wal=0" >> readwrite_flags.txt
echo "--use_existing_db=1" >> readwrite_flags.txt

run_step() {
	if [ -z "$1" ]
	then
		echo run_step called with no parameter
		exit 1
	fi

	if [ -e /opt/intel/bin64/emon ] && [ -e /dev/sepint4_0 ]
	then
		EMON="/opt/intel/bin64/emon `cat $INITIAL_DIR/emon_opts.txt` -f $1_emon.txt "
	fi

	echo -n Start $1 test phase...
	cat /sys/block/nvme0n1/stat > "$1"_blockdev_stats.txt
	$EMON /usr/bin/time perf record $INITIAL_DIR/db_bench --flagfile="$1"_flags.txt &> "$1"_db_bench.txt
	cat /sys/block/nvme0n1/stat >> "$1"_blockdev_stats.txt
	echo done.

	echo -n Generating perf report for $1 test phase...
	sudo perf report -f -n | sed '/#/d' | sed '/%/!d' | sort -r > $1.perf.txt
	rm perf.data
	$INITIAL_DIR/postprocess.py `pwd` $1 > $1_summary.txt
	echo done.
}

run_step insert
run_step overwrite
run_step readwrite

