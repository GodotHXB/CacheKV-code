// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ARENA_H_
#define STORAGE_LEVELDB_UTIL_ARENA_H_

#include <iostream>
#include <vector>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "port/port.h"
#include "util/hash.h"
#include "util/spinlock.h"
#include "leveldb/slice.h"

#include <atomic>
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

#define SUB_MEM_SIZE 2097152 

namespace leveldb {

//Overprovision
#define MEM_THRESH 1.5

class Arena {
public:
    Arena();
    ~Arena();

    // Return a pointer to a newly allocated memory block of "bytes" bytes.
    char* Allocate(size_t bytes);
    // char* AllocateByKey(size_t bytes, const Slice& key);

    // Allocate memory with the normal alignment guarantees provided by malloc
    virtual char* AllocateAligned(size_t bytes);
    char* AllocateAligned_submemIndex(size_t bytes, int sub_mem_index);
    char* AllocateFallback_submemIndex(size_t bytes, int sub_mem_index);

    // Skiplist -> B+-Tree
    char* AllocateAligned_submemIndex_Btree(size_t bytes, int sub_mem_index);
    char* AllocateFallback_submemIndex_Btree(size_t bytes, int sub_mem_index);


    // Returns an estimate of the total memory usage of data allocated
    // by the arena.
    size_t MemoryUsage() const {
        return reinterpret_cast<uintptr_t>(memory_usage_.NoBarrier_Load());
    }

    void* operator new(size_t size);
    void* operator new[](size_t size);
    void operator delete(void* ptr);
    virtual void* CalculateOffset(void* ptr);
    virtual void* getMapStart();
    size_t getAllocRem();
    void* map_start_;
    void* map_end_;
    int  is_largemap_set_;
    bool nvmarena_;
    long kSize;
    std::string mfile;
    int fd;
    bool allocation;

    //private:
    virtual char* AllocateFallback(size_t bytes);
    virtual char* AllocateNewBlock(size_t block_bytes);

    // Allocation state
    char* alloc_ptr_;
    size_t alloc_bytes_remaining_;
    bool isDataLock;


    // char** percore_alloc_ptr_;
    // size_t* percore_alloc_bytes_remaining_;
    char** sub_mem_alloc_ptr_;
    size_t* sub_mem_alloc_bytes_remaining;

    long cores;
    std::atomic_bool *sub_mem_bset;
    size_t sub_mem_count;
    std::atomic_bool *sub_immem_bset;
    size_t sub_immem_count;
    std::atomic_bool *in_trans_bset;

    std::vector<char*> *skiplist_blocks;
    char** skiplist_alloc_ptr_;
    size_t *skiplist_alloc_bytes_remaining_;

    // SkipList -> B+-Tree
    std::vector<char*> *btree_blocks;
    char** btree_alloc_ptr_;
    size_t *btree_alloc_bytes_remaining_;

    size_t dlock_way;
    size_t dlock_size;

    // Array of new[] allocated memory blocks
    std::vector<char*> blocks_;
protected:
    // Total memory usage of the arena.
    port::AtomicPointer memory_usage_;

    // No copying allowed
    //Arena(const Arena&);
};

inline char* Arena::Allocate(size_t bytes) {
    // The semantics of what to return are a bit messy if we allow
    // 0-byte allocations, so we disallow them here (we don't need
    // them for our internal use).
    assert(bytes > 0);
    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    // We waste the remaining space in the current block.
    return AllocateFallback(bytes);
}


class ArenaNVM : public Arena{
public:
#ifdef ENABLE_RECOVERY
    ArenaNVM(long size, std::string *filename, bool recovery);
#else
    ArenaNVM();
#endif
    ~ArenaNVM();
    void* operator new(size_t size);
    void* operator new[](size_t size);
    void operator delete(void* ptr);
    char* AllocateNVMBlock(size_t block_bytes);
    char* AllocateFallbackNVM(size_t bytes);
    // Allocate memory with the normal alignment guarantees provided by malloc
    char* AllocateAligned(size_t bytes);
    char* AllocateAlignedNVM(size_t bytes);
    char* Allocate(size_t bytes);
    char* AllocateByKey(size_t bytes, Slice const& key);
    void* CalculateOffset(void* ptr);
    void* getMapStart();
    int alloc_sub_mem(int sub_mem_index);
    int swap_sub_mem(int sub_mem_index);
    void reclaim_sub_mem(int sub_mem_index);
    void setSubMemToImm();
    int init_memory(char* mmap_ptr, size_t sz);
    int dlock_exit(void);

    // Returns an estimate of the total memory usage of data allocated
    // by the arena.
    size_t MemoryUsage() const {
        return reinterpret_cast<uintptr_t>(memory_usage_.NoBarrier_Load());
    }
    // Total memory usage of the arena.
};

inline char* ArenaNVM::Allocate(size_t bytes) {
    assert(bytes > 0);

    SpinLock splock;
    splock.lock();
    
    if(!allocation && !AllocateFallbackNVM(bytes)){
        splock.unlock();
        return NULL;
    }
    
    unsigned int cpu;
    if(syscall(SYS_getcpu, &cpu, NULL, NULL)) {
        return NULL;
    }


    if(bytes > sub_mem_alloc_bytes_remaining[cpu])
        if(sub_mem_alloc_ptr_[cpu]) {
            if(swap_sub_mem(cpu) == -1)
            {
                splock.unlock();
                return NULL;
            } 
        }
        else {
            if(alloc_sub_mem(cpu) == -1){
                splock.unlock();
                return NULL;
            }
        }
    char* result = sub_mem_alloc_ptr_[cpu];
    sub_mem_alloc_ptr_[cpu] += bytes;
    sub_mem_alloc_bytes_remaining[cpu] -= bytes;
#if defined(ENABLE_RECOVERY)
    memory_usage_.NoBarrier_Store(reinterpret_cast<void*>(MemoryUsage() + bytes + sizeof(char*)));
#endif
    splock.unlock();
    return result;


    /*if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
#if defined(ENABLE_RECOVERY)
        memory_usage_.NoBarrier_Store(
                reinterpret_cast<void*>(MemoryUsage() + bytes + sizeof(char*)));
#endif
        return result;
    }
    return AllocateFallbackNVM(bytes);
    */
}

inline char* ArenaNVM::AllocateByKey(size_t bytes, Slice const& key) {
    assert(bytes > 0);

    SpinLock splock;
    splock.lock();
    
    if(!allocation && !AllocateFallbackNVM(bytes)){
        splock.unlock();
        return NULL;
    }
    
    const char* key_ptr = key.data();
    unsigned int key_size = (unsigned int)key.size(); 
    // std::cout<<"key: "<< key_ptr << std::endl;
    // std::cout<<"key_size: "<< key_size << std::endl;
    // std::cout<<"sub_mem_count: "<< sub_mem_count << std::endl;

    unsigned int random_sub_mem_index = Time33Hash(key_ptr, key_size, sub_mem_count);  // randomly summon a index of sub-MemTable by hash algorithm

    // std::cout<<"random_sub_mem_index: "<< random_sub_mem_index << std::endl;

    if(bytes > sub_mem_alloc_bytes_remaining[random_sub_mem_index])
        if(sub_mem_alloc_ptr_[random_sub_mem_index]) {
            if(swap_sub_mem(random_sub_mem_index) == -1){
                splock.unlock();
                return NULL;
            }
        }
        else {
            if(alloc_sub_mem(random_sub_mem_index) == -1){
                splock.unlock();
                return NULL;
            }
        }
    char* result = sub_mem_alloc_ptr_[random_sub_mem_index];
    sub_mem_alloc_ptr_[random_sub_mem_index] += bytes;
    sub_mem_alloc_bytes_remaining[random_sub_mem_index] -= bytes;
#if defined(ENABLE_RECOVERY)
    memory_usage_.NoBarrier_Store(reinterpret_cast<void*>(MemoryUsage() + bytes + sizeof(char*)));
#endif
    splock.unlock();
    return result;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ARENA_H_
