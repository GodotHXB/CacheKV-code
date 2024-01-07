// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_SPINLOCK_H_
#define STORAGE_LEVELDB_UTIL_SPINLOCK_H_

#include "port/port.h"
#include "port/thread_annotations.h"

#include <atomic>

namespace leveldb {

class SCOPED_LOCKABLE SpinLock {
private:
    std::atomic<bool>flag;
public:
    SpinLock() : flag(false){};
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock) = delete;
    void lock(){   //acquire spin lock
        bool expected = false;
        while(!flag.compare_exchange_weak(expected, true)){
            expected = false;    
        }   
    }
    void unlock(){   //release spin lock
        // std::cout<<"lock released"<<std::endl;
        flag.store(false);
    }
    void getLockStatus(){
        std::cout<<"lock status: "<<flag.load()<<std::endl;
    }
};

}  // namespace leveldb


#endif  // STORAGE_LEVELDB_UTIL_SPINLOCK_H_
