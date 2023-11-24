#include "db/parameters.h"

namespace leveldb{
    const int cardinality = (PAGESIZE - sizeof(Header)) / sizeof(Entry);
    const int count_in_line = CACHE_LINE_SIZE / sizeof(Entry);
}