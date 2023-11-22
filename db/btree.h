/*
   Copyright (c) 2018, UNIST. All rights reserved.  The license is a free
   non-exclusive, non-transferable license to reproduce, use, modify and display
   the source code version of the Software, with or without modifications solely
   for non-commercial research, educational or evaluation purposes. The license
   does not entitle Licensee to technical support, telephone assistance,
   enhancements or updates to the Software. All rights, title to and ownership
   interest in the Software, including all intellectual property rights therein
   shall remain in UNIST.

   Please use at your own risk.
*/
#ifndef _BTREE_H
#define _BTREE_H

#include <cassert>
#include <climits>
#include <fstream>
#include <future>
#include <iostream>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define PAGESIZE 512

#define CPU_FREQ_MHZ (1994)
#define DELAY_IN_NS (1000)
#define CACHE_LINE_SIZE 64
#define QUERY_NUM 25

#define IS_FORWARD(c) (c % 2 == 0)

using entry_key_t = int64_t;

namespace leveldb{
  extern pthread_mutex_t print_mtx;
  // class MemTable;

  static inline void cpu_pause() { __asm__ volatile("pause" ::: "memory"); }
  static inline unsigned long read_tsc(void) {
    unsigned long var;
    unsigned int hi, lo;

    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    var = ((unsigned long long int)hi << 32) | lo;

    return var;
  }

  extern unsigned long write_latency_in_ns;
  extern unsigned long long search_time_in_insert;
  extern unsigned int gettime_cnt;
  extern unsigned long long clflush_time_in_insert;
  extern unsigned long long update_time_in_insert;
  extern int clflush_cnt;
  extern int node_cnt;


  inline void mfence() { asm volatile("mfence" ::: "memory"); }

  inline void clflush(char *data, int len) {
    volatile char *ptr = (char *)((unsigned long)data & ~(CACHE_LINE_SIZE - 1));
    mfence();
    for (; ptr < data + len; ptr += CACHE_LINE_SIZE) {
      unsigned long etsc =
          read_tsc() + (unsigned long)(write_latency_in_ns * CPU_FREQ_MHZ / 1000);
      asm volatile("clflush %0" : "+m"(*(volatile char *)ptr));
      while (read_tsc() < etsc)
        cpu_pause();
      //++clflush_cnt;
    }
    mfence();
  }

  class page;

  class btree {
  private:
    int height;
    char *root;

  public:
    btree();
    void setNewRoot(char *);
    void getNumberOfNodes();
    void btree_insert(entry_key_t, char *);
    void btree_insert_internal(char *, entry_key_t, char *, uint32_t);
    void btree_delete(entry_key_t);
    void btree_delete_internal(entry_key_t, char *, uint32_t, entry_key_t *,
                              bool *, page **);
    char *btree_search(entry_key_t);
    void btree_search_range(entry_key_t, entry_key_t, unsigned long *);
    void printAll();

    friend class page;
  };

  class header;

  class header {
  private:
    page *leftmost_ptr;     // 8 bytes
    page *sibling_ptr;      // 8 bytes
    uint32_t level;         // 4 bytes
    uint8_t switch_counter; // 1 bytes
    uint8_t is_deleted;     // 1 bytes
    int16_t last_index;     // 2 bytes
    std::mutex *mtx;        // 8 bytes

    friend class page;
    friend class btree;

  public:
    header() {
      mtx = new std::mutex();

      leftmost_ptr = NULL;
      sibling_ptr = NULL;
      switch_counter = 0;
      last_index = -1;
      is_deleted = false;
    }

    ~header() { delete mtx; }
  };

  class entry {
  private:
    entry_key_t key; // 8 bytes
    char *ptr;       // 8 bytes

  public:
    entry() {
      key = LONG_MAX;
      ptr = NULL;
    }

    friend class page;
    friend class btree;
  };

  extern const int cardinality;
  extern const int count_in_line;

} //namespace leveldb

#endif