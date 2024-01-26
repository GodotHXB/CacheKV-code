#!/bin/bash
#set -x

SETUP() {
  if [ -z "$TEST_TMPDIR" ]
  then
        echo "DB path empty. Run source scripts/setvars.sh from source parent dir"
        exit
  fi
  rm -rf $TEST_TMPDIR/*
  mkdir -p $TEST_TMPDIR
}

#single thread
gdb /home/hxb/CacheKV-code/out-static/db_bench