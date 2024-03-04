#!/bin/bash
#set -x

BENCHMARKS="fillrandom,readrandom"
NUMKEYS="10000000"
# NoveLSM specific parameters
# NoveLSM uses memtable levels, always set to num_levels 2
# write_buffer_size DRAM memtable size in MBs
# write_buffer_size_2 specifies NVM memtable size; set it in few GBs for perfomance;
OTHERPARAMS="--num_levels=2 --write_buffer_size=$DRAMBUFFSZ --nvm_buffer_size=$NVMBUFFSZ"
CACHEKVPARAMS_BASE="--dlock_way=4 --dlock_size=12582912 --skiplistSync_threshold=65536 --subImm_partition=0 --compactImm_threshold=10 --subImm_thread=1"
NUMREADTHREADS="0"
VALUSESZ=64
NUMTHREAD_VALUES=(4 8 16 24)
SUBIMMTHREAD_THRESHOLD_VALUES=(1)

SETUP() {
  if [ -z "$TEST_TMPDIR" ]
  then
        echo "DB path empty. Run source scripts/setvars.sh from source parent dir"
        exit
  fi
  rm -rf $TEST_TMPDIR/*
  mkdir -p $TEST_TMPDIR
}

MAKE() {
  cd $NOVELSMSRC
  # make clean
  sudo make -j8
}

for NUMTHREAD in "${NUMTHREAD_VALUES[@]}"; do
    for RUN in {1..5}; do
        echo "Running with --threads=${NUMTHREAD}, Run #${RUN}" >> data/exp3.txt
        CACHEKVPARAMS="$CACHEKVPARAMS_BASE"
        SETUP
        MAKE
        modprobe msr
        ulimit -Sn 10240
        $APP_PREFIX $DBBENCH/db_bench --threads=$NUMTHREAD --num=$NUMKEYS \
        --benchmarks=$BENCHMARKS --value_size=$VALUSESZ $OTHERPARAMS $CACHEKVPARAMS --num_read_threads=$NUMREADTHREADS >> data/exp3.txt
        SETUP
    done
done

