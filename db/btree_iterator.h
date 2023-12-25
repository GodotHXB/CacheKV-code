#ifndef _BTREE_ITERATOR_H
#define _BTREE_ITERATOR_H

#include "db/btree.h"

using entry_key_t = int64_t;

namespace leveldb {
class Btree;
class Entry;
class Page;

class BtreeIterator{
public:
  explicit BtreeIterator(Btree* b);

  bool Valid() const;

  void SeekToFirst();

  void SeekToLast();

  void Seek(const entry_key_t& key);

  void Next();

  void Prev();

  entry_key_t key() const;

  Btree* GetBtree() const;

  Page* GetCurPage() const;

  void* value() const;

private:
  Btree* btree;
  Entry* cur;
  Page* cur_page;
  int index;
  bool valid; // validity of current entry
  bool last;
};

} // namespace leveldb

#endif // _BTREE_ITERATOR_H
