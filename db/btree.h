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
#include <map>
#include "db/btree_iterator.h"
// #include "db/db_impl.h"

#define PAGESIZE 1024

#define CPU_FREQ_MHZ (1994)
#define DELAY_IN_NS (1000)
#define CACHE_LINE_SIZE 64
#define QUERY_NUM 25

#define IS_FORWARD(c) (c % 2 == 0)

using entry_key_t = int64_t;

namespace leveldb{
  extern pthread_mutex_t print_mtx;
  class Header;
  class Page;
  class Btree;
  class BtreeIterator;
  // class MemTable;

  class Header {
  private:
    Page *leftmost_ptr;     // 8 bytes
    Page *sibling_ptr;      // 8 bytes
    Page *extend_ptr;       // 8 bytes
    uint32_t level;         // 4 bytes
    uint8_t switch_counter; // 1 bytes
    uint8_t is_deleted;     // 1 bytes
    int16_t last_index;     // 2 bytes
    std::mutex *mtx;        // 8 bytes

    friend class Page;
    friend class Btree;
    friend class BtreeIterator;

  public:
    Header() {
      mtx = new std::mutex();

      leftmost_ptr = NULL;
      sibling_ptr = NULL;
      extend_ptr = NULL;
      switch_counter = 0;
      last_index = -1;
      is_deleted = false;
    }

    ~Header() { delete mtx; }
  };

  class Entry {
  private:
    entry_key_t key; // 8 bytes
    char *ptr;       // 8 bytes

  public:
    Entry() {
      key = LONG_MAX;
      ptr = NULL;
    }

    friend class Page;
    friend class Btree;
    friend class BtreeIterator;
  };

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


  class Page{
    private:
        Header hdr;                 // header in persistent memory, 16 bytes
        Entry records[(PAGESIZE - sizeof(Header)) / sizeof(Entry)]; // slots in persistent memory, 16 bytes * n
    public:
        friend class Btree;
        friend class BtreeIterator;
        Page(uint32_t level = 0);
        Page(Page *left, entry_key_t key, Page *right, uint32_t level = 0);
        int count();
        bool remove_key(entry_key_t key);
        bool remove(Btree *bt, entry_key_t key, bool only_rebalance = false,
                  bool with_lock = true);
        bool remove_rebalancing(Btree *bt, entry_key_t key,
                                bool only_rebalance = false, bool with_lock = true);
        void insert_key(entry_key_t key, char *ptr, int *num_entries,
                            bool flush = true, bool update_last_index = true);
        Page *store(Btree *bt, char *left, entry_key_t key, char *right, bool flush,
                    bool with_lock, Page *invalid_sibling = NULL);
        void linear_search_range(entry_key_t min, entry_key_t max,
                                unsigned long *buf);
        char *linear_search(entry_key_t key);
        void print();
        void printAll();
  };

  class Btree {
    private:
      int height;
      char *root;
      bool inCompact;
      bool inSplit;
      std::vector<Page *>pending_split_page;
      std::map< int64_t, char* >mapping;

    public:
      Btree();
      void setNewRoot(char *);
      void Insert(entry_key_t, char *);
      void Insert(char *, char *);
      void btree_insert_internal(char *, entry_key_t, char *, uint32_t);
      void btree_delete(entry_key_t);
      void btree_delete_internal(entry_key_t, char *, uint32_t, entry_key_t *,
                                bool *, Page **);
      char *btree_search(entry_key_t);
      void btree_search_range(entry_key_t, entry_key_t, unsigned long *);
      void printAll();
      char* getMapping(int64_t key_int64);
      void setInCompact(bool st);
      bool getInCompact();
      void setInSplit(bool st);
      bool getInSplit();
      void splitPage(bool with_lock);

      BtreeIterator* getIterator();

      friend class Page;
      friend class BtreeIterator;
  };

} //namespace leveldb

#endif