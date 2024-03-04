﻿#include <cassert>
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
#include "db/btree.h"
#include "db/btree_iterator.h"
#include "db/parameters.h"
#include "util/coding.h"
#include "btree.h"

namespace leveldb{
    
    pthread_mutex_t print_mtx;
    unsigned long write_latency_in_ns = 0;
    unsigned long long search_time_in_insert = 0;
    unsigned int gettime_cnt = 0;
    unsigned long long clflush_time_in_insert = 0;
    unsigned long long update_time_in_insert = 0;
    int clflush_cnt = 0;
    int node_cnt = 0;

    Page::Page(uint32_t level = 0) {
        hdr.level = level;
        records[0].ptr = NULL;
    }

    // this is called when tree grows
    Page::Page(Page *left, entry_key_t key, Page *right, uint32_t level = 0) {
    hdr.leftmost_ptr = left;
    hdr.level = level;
    records[0].key = key;
    records[0].ptr = (char *)right;
    records[1].ptr = NULL;

    hdr.last_index = 0;

    clflush((char *)this, sizeof(Page));
    }


    int Page::count() {
    uint8_t previous_switch_counter;
    int count = 0;
    do {
        previous_switch_counter = hdr.switch_counter;
        count = hdr.last_index + 1;

        while (count >= 0 && records[count].ptr != NULL) {
        if (IS_FORWARD(previous_switch_counter))
            ++count;
        else
            --count;
        }

        if (count < 0) {
        count = 0;
        while (records[count].ptr != NULL) {
            ++count;
        }
        }

    } while (previous_switch_counter != hdr.switch_counter);

    return count;
    }

    inline bool Page::remove_key(entry_key_t key) {
    // Set the switch_counter
    if (IS_FORWARD(hdr.switch_counter))
        ++hdr.switch_counter;

    bool shift = false;
    int i;
    for (i = 0; records[i].ptr != NULL; ++i) {
        if (!shift && records[i].key == key) {
        records[i].ptr =
            (i == 0) ? (char *)hdr.leftmost_ptr : records[i - 1].ptr;
        shift = true;
        }

        if (shift) {
        records[i].key = records[i + 1].key;
        records[i].ptr = records[i + 1].ptr;

        // flush
        uint64_t records_ptr = (uint64_t)(&records[i]);
        int remainder = records_ptr % CACHE_LINE_SIZE;
        bool do_flush =
            (remainder == 0) ||
            ((((int)(remainder + sizeof(Entry)) / CACHE_LINE_SIZE) == 1) &&
            ((remainder + sizeof(Entry)) % CACHE_LINE_SIZE) != 0);
        if (do_flush) {
            clflush((char *)records_ptr, CACHE_LINE_SIZE);
        }
        }
    }

    if (shift) {
        --hdr.last_index;
    }
    return shift;
    }

    bool Page::remove(Btree *bt, entry_key_t key, bool only_rebalance = false,
                bool with_lock = true) {
        hdr.mtx->lock();

        bool ret = remove_key(key);

        hdr.mtx->unlock();

        return ret;
    }

    /*
    * Although we implemented the rebalancing of B+-Tree, it is currently blocked
    * for the performance. Please refer to the follow. Chi, P., Lee, W. C., &
    * Xie, Y. (2014, August). Making B+-tree efficient in PCM-based main memory.
    * In Proceedings of the 2014 international symposium on Low power electronics
    * and design (pp. 69-74). ACM.
    */
    bool Page::remove_rebalancing(Btree *bt, entry_key_t key,
                            bool only_rebalance = false, bool with_lock = true) {
    if (with_lock) {
        hdr.mtx->lock();
    }
    if (hdr.is_deleted) {
        if (with_lock) {
        hdr.mtx->unlock();
        }
        return false;
    }

    if (!only_rebalance) {
        register int num_entries_before = count();

        // This node is root
        if (this == (Page *)bt->root) {
        if (hdr.level > 0) {
            if (num_entries_before == 1 && !hdr.sibling_ptr) {
            bt->root = (char *)hdr.leftmost_ptr;
            clflush((char *)&(bt->root), sizeof(char *));

            hdr.is_deleted = 1;
            }
        }

        // Remove the key from this node
        bool ret = remove_key(key);

        if (with_lock) {
            hdr.mtx->unlock();
        }
        return true;
        }

        bool should_rebalance = true;
        // check the node utilization
        if (num_entries_before - 1 >= (int)((cardinality - 1) * 0.5)) {
        should_rebalance = false;
        }

        // Remove the key from this node
        bool ret = remove_key(key);

        if (!should_rebalance) {
        if (with_lock) {
            hdr.mtx->unlock();
        }
        return (hdr.leftmost_ptr == NULL) ? ret : true;
        }
    }

    // Remove a key from the parent node
    entry_key_t deleted_key_from_parent = 0;
    bool is_leftmost_node = false;
    Page *left_sibling;
    bt->btree_delete_internal(key, (char *)this, hdr.level + 1,
                                &deleted_key_from_parent, &is_leftmost_node,
                                &left_sibling);

    if (is_leftmost_node) {
        if (with_lock) {
        hdr.mtx->unlock();
        }

        if (!with_lock) {
        hdr.sibling_ptr->hdr.mtx->lock();
        }
        hdr.sibling_ptr->remove(bt, hdr.sibling_ptr->records[0].key, true,
                                with_lock);
        if (!with_lock) {
        hdr.sibling_ptr->hdr.mtx->unlock();
        }
        return true;
    }

    if (with_lock) {
        left_sibling->hdr.mtx->lock();
    }

    while (left_sibling->hdr.sibling_ptr != this) {
        if (with_lock) {
        Page *t = left_sibling->hdr.sibling_ptr;
        left_sibling->hdr.mtx->unlock();
        left_sibling = t;
        left_sibling->hdr.mtx->lock();
        } else
        left_sibling = left_sibling->hdr.sibling_ptr;
    }

    register int num_entries = count();
    register int left_num_entries = left_sibling->count();

    // Merge or Redistribution
    int total_num_entries = num_entries + left_num_entries;
    if (hdr.leftmost_ptr)
        ++total_num_entries;

    entry_key_t parent_key;

    if (total_num_entries > cardinality - 1) { // Redistribution
        register int m = (int)ceil(total_num_entries / 2);

        if (num_entries < left_num_entries) { // left -> right
        if (hdr.leftmost_ptr == nullptr) {
            for (int i = left_num_entries - 1; i >= m; i--) {
            insert_key(left_sibling->records[i].key,
                        left_sibling->records[i].ptr, &num_entries);
            }

            left_sibling->records[m].ptr = nullptr;
            clflush((char *)&(left_sibling->records[m].ptr), sizeof(char *));

            left_sibling->hdr.last_index = m - 1;
            clflush((char *)&(left_sibling->hdr.last_index), sizeof(int16_t));

            parent_key = records[0].key;
        } else {
            insert_key(deleted_key_from_parent, (char *)hdr.leftmost_ptr,
                    &num_entries);

            for (int i = left_num_entries - 1; i > m; i--) {
            insert_key(left_sibling->records[i].key,
                        left_sibling->records[i].ptr, &num_entries);
            }

            parent_key = left_sibling->records[m].key;

            hdr.leftmost_ptr = (Page *)left_sibling->records[m].ptr;
            clflush((char *)&(hdr.leftmost_ptr), sizeof(Page *));

            left_sibling->records[m].ptr = nullptr;
            clflush((char *)&(left_sibling->records[m].ptr), sizeof(char *));

            left_sibling->hdr.last_index = m - 1;
            clflush((char *)&(left_sibling->hdr.last_index), sizeof(int16_t));
        }

        if (left_sibling == ((Page *)bt->root)) {
            Page *new_root =
                new Page(left_sibling, parent_key, this, hdr.level + 1);
            bt->setNewRoot((char *)new_root);
        } else {
            bt->btree_insert_internal((char *)left_sibling, parent_key,
                                    (char *)this, hdr.level + 1);
        }
        } else { // from leftmost case
        hdr.is_deleted = 1;
        clflush((char *)&(hdr.is_deleted), sizeof(uint8_t));

        Page *new_sibling = new Page(hdr.level);
        new_sibling->hdr.mtx->lock();
        new_sibling->hdr.sibling_ptr = hdr.sibling_ptr;

        int num_dist_entries = num_entries - m;
        int new_sibling_cnt = 0;

        if (hdr.leftmost_ptr == nullptr) {
            for (int i = 0; i < num_dist_entries; i++) {
            left_sibling->insert_key(records[i].key, records[i].ptr,
                                    &left_num_entries);
            }

            for (int i = num_dist_entries; records[i].ptr != NULL; i++) {
            new_sibling->insert_key(records[i].key, records[i].ptr,
                                    &new_sibling_cnt, false);
            }

            clflush((char *)(new_sibling), sizeof(Page));

            left_sibling->hdr.sibling_ptr = new_sibling;
            clflush((char *)&(left_sibling->hdr.sibling_ptr), sizeof(Page *));

            parent_key = new_sibling->records[0].key;
        } else {
            left_sibling->insert_key(deleted_key_from_parent,
                                    (char *)hdr.leftmost_ptr, &left_num_entries);

            for (int i = 0; i < num_dist_entries - 1; i++) {
            left_sibling->insert_key(records[i].key, records[i].ptr,
                                    &left_num_entries);
            }

            parent_key = records[num_dist_entries - 1].key;

            new_sibling->hdr.leftmost_ptr =
                (Page *)records[num_dist_entries - 1].ptr;
            for (int i = num_dist_entries; records[i].ptr != NULL; i++) {
            new_sibling->insert_key(records[i].key, records[i].ptr,
                                    &new_sibling_cnt, false);
            }
            clflush((char *)(new_sibling), sizeof(Page));

            left_sibling->hdr.sibling_ptr = new_sibling;
            clflush((char *)&(left_sibling->hdr.sibling_ptr), sizeof(Page *));
        }

        if (left_sibling == ((Page *)bt->root)) {
            Page *new_root =
                new Page(left_sibling, parent_key, new_sibling, hdr.level + 1);
            bt->setNewRoot((char *)new_root);
        } else {
            bt->btree_insert_internal((char *)left_sibling, parent_key,
                                    (char *)new_sibling, hdr.level + 1);
        }

        new_sibling->hdr.mtx->unlock();
        }
    } else {
        hdr.is_deleted = 1;
        clflush((char *)&(hdr.is_deleted), sizeof(uint8_t));

        if (hdr.leftmost_ptr)
        left_sibling->insert_key(deleted_key_from_parent,
                                (char *)hdr.leftmost_ptr, &left_num_entries);

        for (int i = 0; records[i].ptr != NULL; ++i) {
        left_sibling->insert_key(records[i].key, records[i].ptr,
                                &left_num_entries);
        }

        left_sibling->hdr.sibling_ptr = hdr.sibling_ptr;
        clflush((char *)&(left_sibling->hdr.sibling_ptr), sizeof(Page *));
    }

    if (with_lock) {
        left_sibling->hdr.mtx->unlock();
        hdr.mtx->unlock();
    }

    return true;
    }

    inline void Page::insert_key(entry_key_t key, char *ptr, int *num_entries,
                        bool flush = true, bool update_last_index = true) {
    // update switch_counter
    if (!IS_FORWARD(hdr.switch_counter))
        ++hdr.switch_counter;

    // FAST
    if (*num_entries == 0) { // this Page is empty
        Entry *new_entry = (Entry *)&records[0];
        Entry *array_end = (Entry *)&records[1];
        new_entry->key = (entry_key_t)key;
        new_entry->ptr = (char *)ptr;

        array_end->ptr = (char *)NULL;

        if (flush) {
        clflush((char *)this, CACHE_LINE_SIZE);
        }
    } else {
        int i = *num_entries - 1, inserted = 0, to_flush_cnt = 0;
        records[*num_entries + 1].ptr = records[*num_entries].ptr;
        if (flush) {
        if ((uint64_t) & (records[*num_entries + 1].ptr) % CACHE_LINE_SIZE == 0){
            clflush((char *)&(records[*num_entries + 1].ptr), sizeof(char *));
            }
        }

        // FAST
        for (i = *num_entries - 1; i >= 0; i--) {
        if (key < records[i].key) {
            records[i + 1].ptr = records[i].ptr;
            records[i + 1].key = records[i].key;

            if (flush) {
            uint64_t records_ptr = (uint64_t)(&records[i + 1]);

            int remainder = records_ptr % CACHE_LINE_SIZE;
            bool do_flush =
                (remainder == 0) ||
                ((((int)(remainder + sizeof(Entry)) / CACHE_LINE_SIZE) == 1) &&
                ((remainder + sizeof(Entry)) % CACHE_LINE_SIZE) != 0);
            if (do_flush) {
                clflush((char *)records_ptr, CACHE_LINE_SIZE);
                to_flush_cnt = 0;
            } else
                ++to_flush_cnt;
            }
        } else {
            records[i + 1].ptr = records[i].ptr;
            records[i + 1].key = key;
            records[i + 1].ptr = ptr;

            if (flush)
            clflush((char *)&records[i + 1], sizeof(Entry));
            inserted = 1;
            break;
        }
        }
        if (inserted == 0) {
        records[0].ptr = (char *)hdr.leftmost_ptr;
        records[0].key = key;
        records[0].ptr = ptr;
        if (flush){
            clflush((char *)&records[0], sizeof(Entry));
            }
        }
    }

    if (update_last_index) {
        hdr.last_index = *num_entries;
    }
    ++(*num_entries);
    }

    // Insert a new key - FAST and FAIR
    Page* Page::store(Btree *bt, char *left, entry_key_t key, char *right, bool flush,
                bool with_lock, Page *invalid_sibling = NULL) {
    if (with_lock) {
        hdr.mtx->lock(); // Lock the write lock
    }
    if (hdr.is_deleted) {
        if (with_lock) {
        hdr.mtx->unlock();
        }

        return NULL;
    }

    // If this node has a sibling node,
    if (hdr.sibling_ptr && (hdr.sibling_ptr != invalid_sibling)) {
        // Compare this key with the first key of the sibling
        if (key > hdr.sibling_ptr->records[0].key) {
        if (with_lock) {
            hdr.mtx->unlock(); // Unlock the write lock
        }
        return hdr.sibling_ptr->store(bt, NULL, key, right, true, with_lock,
                                        invalid_sibling);
        }
    }

    register int num_entries = count();
    // FAST
    if (num_entries < cardinality - 1) {
        insert_key(key, right, &num_entries, flush);
        if (with_lock) {
            hdr.mtx->unlock(); // Unlock the write lock
        }
        return this;
    } else { 
        // FAIR
        // overflow
        // create a new node
        Page* tmp_page;         // use tmp_page as the extend of current page
        Page* cur_page = this;
        int tmp_cnt = 0;
        // std::cout<<"inCompact: "<<bt->getInCompact()<<std::endl;
        if(bt->getInCompact()){ // if in compaction, insert kv-pair but postpone the split
            // insert into a new temporary page
            if(cur_page->hdr.extend_ptr){  // current page is extended
                while(cur_page->hdr.extend_ptr){  // find the last extend page
                    // std::cout<<"cur_page: "<<cur_page<<" is full, goto extend page: "<<cur_page->hdr.extend_ptr<<std::endl;
                    cur_page = cur_page->hdr.extend_ptr;
                } // end when there is no extend page or extend page is not full
                if(cur_page->count() >= cardinality - 1){ // the last extend page is full
                    cur_page->hdr.extend_ptr = tmp_page = new Page(hdr.level); 
                    tmp_cnt = 0;
                }else{ // extend page is not full
                    tmp_page = cur_page;
                    tmp_cnt = cur_page->count();
                }
            }
            else if(!cur_page->hdr.extend_ptr){ // there is no extend page
                cur_page->hdr.extend_ptr = tmp_page = new Page(hdr.level);
                tmp_cnt = 0;
            }

            // std::cout<<"start_page: "<<this<<", tmp_page: "<<tmp_page<<std::endl;

            tmp_page->insert_key(key, right, &tmp_cnt, flush);
            // tmp_page->print();
            if (with_lock) {
                hdr.mtx->unlock(); // Unlock the write lock
            }
            bt->pending_split_page.push_back(this); // add the start page into waiting list
            return tmp_page;
        }
        else{ // not in compaction, split the page(s) and insert the new kv
            cur_page = this; // start split from the origin page
            // std::cout<<"cur_page: "<<cur_page<<" , cur_page->count(): "<<cur_page->count()<<std::endl;
            while( cur_page == this || cur_page->hdr.extend_ptr ){ // the page needs to split 
                Page *sibling = new Page(hdr.level);
                register int m = (int)ceil(num_entries / 2);
                entry_key_t split_key = cur_page->records[m].key;
                // std::cout<<"num_entries: "<<num_entries<<std::endl;
                // std::cout<<"split_key: "<<split_key<<std::endl;

                // migrate half of keys into the sibling
                int sibling_cnt = 0;
                if (cur_page->hdr.leftmost_ptr == NULL) { // leaf node
                    for (int i = m; i < num_entries; ++i) {
                        sibling->insert_key(cur_page->records[i].key, cur_page->records[i].ptr, &sibling_cnt,
                                            false);
                }
                } else { // internal node
                    for (int i = m + 1; i < num_entries; ++i) {
                        sibling->insert_key(cur_page->records[i].key, cur_page->records[i].ptr, &sibling_cnt,
                                            false);
                    }
                    sibling->hdr.leftmost_ptr = (Page *)cur_page->records[m].ptr;
                }

                sibling->hdr.sibling_ptr = cur_page->hdr.sibling_ptr;
                clflush((char *)sibling, sizeof(Page));

                cur_page->hdr.sibling_ptr = sibling;
                clflush((char *)&hdr, sizeof(hdr));

                // set to NULL
                if (IS_FORWARD(cur_page->hdr.switch_counter))
                    cur_page->hdr.switch_counter += 2;
                else
                    ++cur_page->hdr.switch_counter;
                cur_page->records[m].ptr = NULL;
                clflush((char *)&cur_page->records[m], sizeof(Entry));

                cur_page->hdr.last_index = m - 1;
                clflush((char *)&(cur_page->hdr.last_index), sizeof(int16_t));

                num_entries = cur_page->hdr.last_index + 1;
                std::cout<<"page: "<<cur_page<<" is splitted, next page: "<<cur_page->hdr.extend_ptr<<", next page cnt: "<<cur_page->hdr.extend_ptr->count()<<std::endl;
                cur_page = cur_page->hdr.extend_ptr;
            }
            // Page *sibling = new Page(hdr.level);
            // register int m = (int)ceil(num_entries / 2);
            // entry_key_t split_key = records[m].key;

            // // migrate half of keys into the sibling
            // int sibling_cnt = 0;
            // if (hdr.leftmost_ptr == NULL) { // leaf node
            //     for (int i = m; i < num_entries; ++i) {
            //         sibling->insert_key(records[i].key, records[i].ptr, &sibling_cnt,
            //                             false);
            // }
            // } else { // internal node
            //     for (int i = m + 1; i < num_entries; ++i) {
            //         sibling->insert_key(records[i].key, records[i].ptr, &sibling_cnt,
            //                             false);
            //     }
            //     sibling->hdr.leftmost_ptr = (Page *)records[m].ptr;
            // }

            // sibling->hdr.sibling_ptr = hdr.sibling_ptr;
            // clflush((char *)sibling, sizeof(Page));

            // hdr.sibling_ptr = sibling;
            // clflush((char *)&hdr, sizeof(hdr));

            // // set to NULL
            // if (IS_FORWARD(hdr.switch_counter))
            //     hdr.switch_counter += 2;
            // else
            //     ++hdr.switch_counter;
            // records[m].ptr = NULL;
            // clflush((char *)&records[m], sizeof(Entry));

            // hdr.last_index = m - 1;
            // clflush((char *)&(hdr.last_index), sizeof(int16_t));

            // num_entries = hdr.last_index + 1;

            Page *ret = NULL;
            cur_page = this;
            std::cout<<"key: "<<key<<" needs to be inserted"<<std::endl;
            std::cout<<"start page: "<<this<<", current page: "<<cur_page<<", extend page: "<<cur_page->hdr.extend_ptr<<std::endl;
            cur_page->print();
            while(cur_page == this || cur_page->hdr.extend_ptr){
                int num_entries_in_cur_page = cur_page->count();
                std::cout<<"cur_page: "<<cur_page<<", num_entries: "<<num_entries<<", num_entries in cur: "<<num_entries_in_cur_page<<", cur_page->records[num_entries].key: "<<cur_page->records[num_entries_in_cur_page-1].key<<std::endl;
                if (key < cur_page->records[num_entries].key) { // key should be inserted into current page
                    std::cout<<"insert into current page"<<std::endl;
                    cur_page->insert_key(key, right, &num_entries);
                    ret = cur_page;
                    if(cur_page == this)break;
                } else if(key >= cur_page->records[num_entries].key && key < cur_page->hdr.sibling_ptr->records[num_entries].key){  // key should be inserted into sibling page
                    std::cout<<"insert into sibling page"<<std::endl;
                    cur_page->hdr.sibling_ptr->insert_key(key,right, &num_entries);
                    ret = cur_page->hdr.sibling_ptr;
                    if(cur_page == this)break;
                } else{
                    std::cout<<"not found, keep searching"<<std::endl;
                    cur_page = cur_page->hdr.extend_ptr; // key should be inserted into extend page, but don't know the exact place, keep searching
                    continue;
                }
            }

            if(with_lock){
                hdr.mtx->unlock();
            }

            // Set a new root or insert the split key to the parent
            // if (bt->root == (char *)this) { // only one node can update the root ptr
            //     Page *new_root =
            //         new Page((Page *)this, cur_page->records[num_entries].key, cur_page->hdr.sibling_ptr, hdr.level + 1);
            //     bt->setNewRoot((char *)new_root);

            //     if (with_lock) {
            //         hdr.mtx->unlock(); // Unlock the write lock
            //     }
            // } else {
            //     if (with_lock) {
            //         hdr.mtx->unlock(); // Unlock the write lock
            //     }
            //     while(cur_page->hdr.extend_ptr){
            //         bt->btree_insert_internal(NULL, cur_page->records[num_entries].key, (char *)cur_page->hdr.sibling_ptr,
            //                                     hdr.level + 1);
            //         cur_page = cur_page->hdr.extend_ptr;
            //     }
            // }
            return ret;
        } 
    } // end of FAIR
} // end of store function
 

// original
//  Insert a new key - FAST and FAIR
//   Page* Page::store(Btree *bt, char *left, entry_key_t key, char *right, bool flush,
//               bool with_lock, Page *invalid_sibling = NULL) {
//     if (with_lock) {
//       hdr.mtx->lock(); // Lock the write lock
//     }
//     if (hdr.is_deleted) {
//       if (with_lock) {
//         hdr.mtx->unlock();
//       }

//       return NULL;
//     }

//     // If this node has a sibling node,
//     if (hdr.sibling_ptr && (hdr.sibling_ptr != invalid_sibling)) {
//       // Compare this key with the first key of the sibling
//       if (key > hdr.sibling_ptr->records[0].key) {
//         if (with_lock) {
//           hdr.mtx->unlock(); // Unlock the write lock
//         }
//         return hdr.sibling_ptr->store(bt, NULL, key, right, true, with_lock,
//                                       invalid_sibling);
//       }
//     }

//     register int num_entries = count();

//     // FAST
//     if (num_entries < cardinality - 1) {
//       insert_key(key, right, &num_entries, flush);

//       if (with_lock) {
//         hdr.mtx->unlock(); // Unlock the write lock
//       }

//       return this;
//     } else { // FAIR
//       // overflow
//       // create a new node
//       Page *sibling = new Page(hdr.level);
//       register int m = (int)ceil(num_entries / 2);
//       entry_key_t split_key = records[m].key;

//       // migrate half of keys into the sibling
//       int sibling_cnt = 0;
//       if (hdr.leftmost_ptr == NULL) { // leaf node
//         for (int i = m; i < num_entries; ++i) {
//           sibling->insert_key(records[i].key, records[i].ptr, &sibling_cnt,
//                               false);
//         }
//       } else { // internal node
//         for (int i = m + 1; i < num_entries; ++i) {
//           sibling->insert_key(records[i].key, records[i].ptr, &sibling_cnt,
//                               false);
//         }
//         sibling->hdr.leftmost_ptr = (Page *)records[m].ptr;
//       }

//       sibling->hdr.sibling_ptr = hdr.sibling_ptr;
//       clflush((char *)sibling, sizeof(Page));

//       hdr.sibling_ptr = sibling;
//       clflush((char *)&hdr, sizeof(hdr));

//       // set to NULL
//       if (IS_FORWARD(hdr.switch_counter))
//         hdr.switch_counter += 2;
//       else
//         ++hdr.switch_counter;
//       records[m].ptr = NULL;
//       clflush((char *)&records[m], sizeof(Entry));

//       hdr.last_index = m - 1;
//       clflush((char *)&(hdr.last_index), sizeof(int16_t));

//       num_entries = hdr.last_index + 1;

//       Page *ret;

//       // insert the key
//       if (key < split_key) {
//         insert_key(key, right, &num_entries);
//         ret = this;
//       } else {
//         sibling->insert_key(key, right, &sibling_cnt);
//         ret = sibling;
//       }

//       // Set a new root or insert the split key to the parent
//       if (bt->root == (char *)this) { // only one node can update the root ptr
//         Page *new_root =
//             new Page((Page *)this, split_key, sibling, hdr.level + 1);
//         bt->setNewRoot((char *)new_root);

//         if (with_lock) {
//           hdr.mtx->unlock(); // Unlock the write lock
//         }
//       } else {
//         if (with_lock) {
//           hdr.mtx->unlock(); // Unlock the write lock
//         }
//         bt->btree_insert_internal(NULL, split_key, (char *)sibling,
//                                   hdr.level + 1);
//       }

//       return ret;
//     }
//   }

    // Search keys with linear search
    void Page::linear_search_range(entry_key_t min, entry_key_t max,
                            unsigned long *buf) {
    int i, off = 0;
    uint8_t previous_switch_counter;
    Page *current = this;

    while (current) {
        int old_off = off;
        do {
        previous_switch_counter = current->hdr.switch_counter;
        off = old_off;

        entry_key_t tmp_key;
        char *tmp_ptr;

        if (IS_FORWARD(previous_switch_counter)) {
            if ((tmp_key = current->records[0].key) > min) {
            if (tmp_key < max) {
                if ((tmp_ptr = current->records[0].ptr) != NULL) {
                if (tmp_key == current->records[0].key) {
                    if (tmp_ptr) {
                    buf[off++] = (unsigned long)tmp_ptr;
                    }
                }
                }
            } else
                return;
            }

            for (i = 1; current->records[i].ptr != NULL; ++i) {
            if ((tmp_key = current->records[i].key) > min) {
                if (tmp_key < max) {
                if ((tmp_ptr = current->records[i].ptr) !=
                    current->records[i - 1].ptr) {
                    if (tmp_key == current->records[i].key) {
                    if (tmp_ptr)
                        buf[off++] = (unsigned long)tmp_ptr;
                    }
                }
                } else
                return;
            }
            }
        } else {
            for (i = count() - 1; i > 0; --i) {
            if ((tmp_key = current->records[i].key) > min) {
                if (tmp_key < max) {
                if ((tmp_ptr = current->records[i].ptr) !=
                    current->records[i - 1].ptr) {
                    if (tmp_key == current->records[i].key) {
                    if (tmp_ptr)
                        buf[off++] = (unsigned long)tmp_ptr;
                    }
                }
                } else
                return;
            }
            }

            if ((tmp_key = current->records[0].key) > min) {
            if (tmp_key < max) {
                if ((tmp_ptr = current->records[0].ptr) != NULL) {
                if (tmp_key == current->records[0].key) {
                    if (tmp_ptr) {
                    buf[off++] = (unsigned long)tmp_ptr;
                    }
                }
                }
            } else
                return;
            }
        }
        } while (previous_switch_counter != current->hdr.switch_counter);

        current = current->hdr.sibling_ptr;
    }
    }

    char* Page::linear_search(entry_key_t key) {
    int i = 1;
    uint8_t previous_switch_counter;
    char *ret = NULL;
    char *t;
    entry_key_t k;

    if (hdr.leftmost_ptr == NULL) { // Search a leaf node
        do {
        previous_switch_counter = hdr.switch_counter;
        ret = NULL;

        // search from left ro right
        if (IS_FORWARD(previous_switch_counter)) {
            if ((k = records[0].key) == key) {
            if ((t = records[0].ptr) != NULL) {
                if (k == records[0].key) {
                ret = t;
                continue;
                }
            }
            }

            for (i = 1; records[i].ptr != NULL; ++i) {
            if ((k = records[i].key) == key) {
                if (records[i - 1].ptr != (t = records[i].ptr)) {
                if (k == records[i].key) {
                    ret = t;
                    break;
                }
                }
            }
            }
        } else { // search from right to left
            for (i = count() - 1; i > 0; --i) {
            if ((k = records[i].key) == key) {
                if (records[i - 1].ptr != (t = records[i].ptr) && t) {
                if (k == records[i].key) {
                    ret = t;
                    break;
                }
                }
            }
            }

            if (!ret) {
            if ((k = records[0].key) == key) {
                if (NULL != (t = records[0].ptr) && t) {
                if (k == records[0].key) {
                    ret = t;
                    continue;
                }
                }
            }
            }
        }
        } while (hdr.switch_counter != previous_switch_counter);

        if (ret) {
        return ret;
        }

        if ((t = (char *)hdr.sibling_ptr) && key >= ((Page *)t)->records[0].key)
        return t;

        return NULL;
    } else { // internal node
        do {
        previous_switch_counter = hdr.switch_counter;
        ret = NULL;

        if (IS_FORWARD(previous_switch_counter)) {
            if (key < (k = records[0].key)) {
            if ((t = (char *)hdr.leftmost_ptr) != records[0].ptr) {
                ret = t;
                continue;
            }
            }

            for (i = 1; records[i].ptr != NULL; ++i) {
            if (key < (k = records[i].key)) {
                if ((t = records[i - 1].ptr) != records[i].ptr) {
                ret = t;
                break;
                }
            }
            }

            if (!ret) {
            ret = records[i - 1].ptr;
            continue;
            }
        } else { // search from right to left
            for (i = count() - 1; i >= 0; --i) {
            if (key >= (k = records[i].key)) {
                if (i == 0) {
                if ((char *)hdr.leftmost_ptr != (t = records[i].ptr)) {
                    ret = t;
                    break;
                }
                } else {
                if (records[i - 1].ptr != (t = records[i].ptr)) {
                    ret = t;
                    break;
                }
                }
            }
            }
        }
        } while (hdr.switch_counter != previous_switch_counter);

        if ((t = (char *)hdr.sibling_ptr) != NULL) {
        if (key >= ((Page *)t)->records[0].key)
            return t;
        }

        if (ret) {
        return ret;
        } else
        return (char *)hdr.leftmost_ptr;
    }

    return NULL;
    }

    // print a node
    void Page::print() {
    if (hdr.leftmost_ptr == NULL)
        printf("[%d] leaf %x \n", this->hdr.level, this);
    else
        printf("[%d] internal %x \n", this->hdr.level, this);
    printf("last_index: %d\n", hdr.last_index);
    printf("switch_counter: %d\n", hdr.switch_counter);
    printf("search direction: ");
    if (IS_FORWARD(hdr.switch_counter))
        printf("->\n");
    else
        printf("<-\n");

    if (hdr.leftmost_ptr != NULL)
        printf("%x ", hdr.leftmost_ptr);

    for (int i = 0; records[i].ptr != NULL; ++i)
        printf("%ld,%x ", records[i].key, records[i].ptr);

    printf("%x ", hdr.sibling_ptr);

    printf("\n");
    }

    void Page::printAll() {
    if (hdr.leftmost_ptr == NULL) {
        printf("printing leaf node: ");
        print();
    } else {
        printf("printing internal node: ");
        print();
        ((Page *)hdr.leftmost_ptr)->printAll();
        for (int i = 0; records[i].ptr != NULL; ++i) {
        ((Page *)records[i].ptr)->printAll();
        }
    }
    }

    Btree::Btree() {
        root = (char *)new Page();
        height = 1;
        inCompact = 0;
    }

    void Btree::setNewRoot(char *new_root) {
        this->root = (char *)new_root;
        clflush((char *)&(this->root), sizeof(char *));
        ++height;
    }

    char *Btree::btree_search(entry_key_t key) {
        Page *p = (Page *)root;

        while (p->hdr.leftmost_ptr != NULL) {
            p = (Page *)p->linear_search(key);
        }

        Page *t;
        while ((t = (Page *)p->linear_search(key)) == p->hdr.sibling_ptr) {
            p = t;
            if (!p) {
                break;
            }
        }

        if (!t) {
            printf("NOT FOUND %lu, t = %x\n", key, t);
            return NULL;
        }

        return (char *)t;
    }

    // insert the key in the leaf node
    void Btree::Insert(entry_key_t key, char *right) { // need to be string
        Page *p = (Page *)root;

        while (p->hdr.leftmost_ptr != NULL) {
            p = (Page *)p->linear_search(key);
        }

        if (!p->store(this, NULL, key, right, true, true)) { // store
            Btree::Insert(key, right);
        }
    }

    // insert the key in the leaf node (using char* as param)
    void Btree::Insert(char *key, char *right) { // need to be string
        Page *p = (Page *)root;
        entry_key_t key_int64 = int64_atoi(key, strlen(key));
        mapping[key_int64] = key;

        while (p->hdr.leftmost_ptr != NULL) {
            p = (Page *)p->linear_search(key_int64);
        }

        if (!p->store(this, NULL, key_int64, right, true, true)) { // store
            Btree::Insert(key_int64, right);
        }
    }


    // store the key into the node at the given level
    void Btree::btree_insert_internal(char *left, entry_key_t key, char *right,
                                        uint32_t level) {
        if (level > ((Page *)root)->hdr.level)
        return;

        Page *p = (Page *)this->root;

        while (p->hdr.level > level)
        p = (Page *)p->linear_search(key);

        if (!p->store(this, NULL, key, right, true, true)) {
        btree_insert_internal(left, key, right, level);
        }
    }

    void Btree::btree_delete(entry_key_t key) {
        Page *p = (Page *)root;

        while (p->hdr.leftmost_ptr != NULL) {
        p = (Page *)p->linear_search(key);
        }

        Page *t;
        while ((t = (Page *)p->linear_search(key)) == p->hdr.sibling_ptr) {
        p = t;
        if (!p)
            break;
        }

        if (p) {
        if (!p->remove(this, key)) {
            btree_delete(key);
        }
        } else {
        printf("not found the key to delete %lu\n", key);
        }
    }

    void Btree::btree_delete_internal(entry_key_t key, char *ptr, uint32_t level,
                                        entry_key_t *deleted_key,
                                        bool *is_leftmost_node, Page **left_sibling) {
        if (level > ((Page *)this->root)->hdr.level)
        return;

        Page *p = (Page *)this->root;

        while (p->hdr.level > level) {
        p = (Page *)p->linear_search(key);
        }

        p->hdr.mtx->lock();

        if ((char *)p->hdr.leftmost_ptr == ptr) {
        *is_leftmost_node = true;
        p->hdr.mtx->unlock();
        return;
        }

        *is_leftmost_node = false;

        for (int i = 0; p->records[i].ptr != NULL; ++i) {
        if (p->records[i].ptr == ptr) {
            if (i == 0) {
            if ((char *)p->hdr.leftmost_ptr != p->records[i].ptr) {
                *deleted_key = p->records[i].key;
                *left_sibling = p->hdr.leftmost_ptr;
                p->remove(this, *deleted_key, false, false);
                break;
            }
            } else {
            if (p->records[i - 1].ptr != p->records[i].ptr) {
                *deleted_key = p->records[i].key;
                *left_sibling = (Page *)p->records[i - 1].ptr;
                p->remove(this, *deleted_key, false, false);
                break;
            }
            }
        }
        }

        p->hdr.mtx->unlock();
    }

    // Function to search keys from "min" to "max"
    void Btree::btree_search_range(entry_key_t min, entry_key_t max,
                                    unsigned long *buf) {
        Page *p = (Page *)root;

        while (p) {
        if (p->hdr.leftmost_ptr != NULL) {
            // The current Page is internal
            p = (Page *)p->linear_search(min);
        } else {
            // Found a leaf
            p->linear_search_range(min, max, buf);

            break;
        }
        }
    }

    void Btree::printAll() {
        pthread_mutex_lock(&print_mtx);
        int total_keys = 0;
        Page *leftmost = (Page *)root;
        printf("root: %x\n", root);
        do {
        Page *sibling = leftmost;
        while (sibling) {
            if (sibling->hdr.level == 0) {
            total_keys += sibling->hdr.last_index + 1;
            }
            sibling->print();
            sibling = sibling->hdr.sibling_ptr;
        }
        printf("-----------------------------------------\n");
        leftmost = leftmost->hdr.leftmost_ptr;
        } while (leftmost);

        printf("total number of keys: %d\n", total_keys);
        pthread_mutex_unlock(&print_mtx);
    }

    char* Btree::getMapping(int64_t key_int64){
        return mapping[key_int64];
    }

    void Btree::setInCompact(bool st){
        inCompact = st;
    }

    bool Btree::getInCompact(){
        return inCompact;
    }

    void Btree::setInSplit(bool st){
        inSplit = st;
    }

    bool Btree::getInSplit(){
        return inSplit;
    }

    // just for split the pending pages
    void Btree::splitPage(bool with_lock){
        Page* cur_page;
        Page* start_page;
        std::cout<<"split_page size: "<<pending_split_page.size()<<std::endl;
        for(int i=0; i<pending_split_page.size(); i++){
            // std::cout<<"i: "<<i<<", split_page size: "<<pending_split_page.size()<<std::endl;
            start_page = cur_page = pending_split_page[i]; // start split from the origin page
            if(with_lock){
                start_page->hdr.mtx->lock();
            }
            int num_entries = cur_page->count();
            // std::cout<<"cur_page: "<<cur_page<<" , cur_page->count(): "<<cur_page->count()<<std::endl;
            while(cur_page->hdr.extend_ptr && cur_page->count() >= cardinality - 1){ // the page needs to split 
                Page *sibling = new Page(cur_page->hdr.level);
                register int m = (int)ceil(num_entries / 2);
                entry_key_t split_key = cur_page->records[m].key;
                // std::cout<<"num_entries: "<<num_entries<<std::endl;
                // std::cout<<"split_key: "<<split_key<<std::endl;

                // migrate half of keys into the sibling
                int sibling_cnt = 0;
                if (cur_page->hdr.leftmost_ptr == NULL) { // leaf node
                    for (int i = m; i < num_entries; ++i) {
                        sibling->insert_key(cur_page->records[i].key, cur_page->records[i].ptr, &sibling_cnt,
                                            false);
                }
                } else { // internal node
                    for (int i = m + 1; i < num_entries; ++i) {
                        sibling->insert_key(cur_page->records[i].key, cur_page->records[i].ptr, &sibling_cnt,
                                            false);
                    }
                    sibling->hdr.leftmost_ptr = (Page *)cur_page->records[m].ptr;
                }

                sibling->hdr.sibling_ptr = cur_page->hdr.sibling_ptr;
                clflush((char *)sibling, sizeof(Page));

                cur_page->hdr.sibling_ptr = sibling;
                clflush((char *)&cur_page->hdr, sizeof(cur_page->hdr));

                // set to NULL
                if (IS_FORWARD(cur_page->hdr.switch_counter))
                    cur_page->hdr.switch_counter += 2;
                else
                    ++cur_page->hdr.switch_counter;
                cur_page->records[m].ptr = NULL;
                clflush((char *)&cur_page->records[m], sizeof(Entry));

                cur_page->hdr.last_index = m - 1;
                clflush((char *)&(cur_page->hdr.last_index), sizeof(int16_t));

                num_entries = cur_page->hdr.last_index + 1;
                // std::cout<<"page: "<<cur_page<<" is splitted, next page: "<<cur_page->hdr.extend_ptr<<", current page cnt: "<<cur_page->count()<<std::endl;
                cur_page = cur_page->hdr.extend_ptr;
            } 
            // TODO: update the root or insert the split key to parent
            bool with_lock = true; // set to with lock(default)
            if (this->root == (char *)start_page) { // only one node can update the root ptr
                Page *new_root =
                    new Page(start_page, cur_page->records[num_entries].key, cur_page->hdr.sibling_ptr, cur_page->hdr.level + 1);
                this->setNewRoot((char *)new_root);
                std::cout<<"set new root in split func"<<std::endl;

                if (with_lock) { 
                    start_page->hdr.mtx->unlock(); // Unlock the write lock
                }
            } else {
                if (with_lock) {
                    start_page->hdr.mtx->unlock(); // Unlock the write lock
                }
                while(cur_page->hdr.extend_ptr){
                    this->btree_insert_internal(NULL, cur_page->records[num_entries].key, (char *)cur_page->hdr.sibling_ptr,
                                                cur_page->hdr.level + 1);
                    cur_page = cur_page->hdr.extend_ptr;
                    std::cout<<"insert the split key to parent in split func"<<std::endl;
                }
            }
        }
        pending_split_page.clear();
        // std::cout<<"quit split func"<<std::endl;
    }

    BtreeIterator* Btree::getIterator(){
        return new BtreeIterator(this);
    }

}
