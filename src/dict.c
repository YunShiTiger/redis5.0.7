/* 
 * Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 */

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#ifndef DICT_BENCHMARK_MAIN
#include "redisassert.h"
#else
#include <assert.h>
#endif

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
static int dict_can_resize = 1;
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static long _dictKeyIndex(dict *ht, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, int len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Create a new hash table */
/* 创建一个新的字典结构 */
dict *dictCreate(dictType *type, void *privDataPtr) {
	//首先分配对应的空间
    dict *d = zmalloc(sizeof(*d));
	//初始化字典结构的相关参数信息
    _dictInit(d,type,privDataPtr);
	//返回创建的字典结构指向
    return d;
}

/* Initialize the hash table */
/* 初始化字典结构的相关参数信息 */
int _dictInit(dict *d, dictType *type, void *privDataPtr) {
	//进行重置字典结构中的两个hash表数据
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);
    d->type = type;
    d->privdata = privDataPtr;
    d->rehashidx = -1;
    d->iterators = 0;
	//返回初始化成功标识
    return DICT_OK;
}

/* Reset a hash table already initialized with ht_init(). NOTE: This function should only be called by ht_destroy(). */
/* 重置hash表结构中的参数数据 */
static void _dictReset(dictht *ht) {
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}


/* Resize the table to the minimal size that contains all the elements, but with the invariant of a USED/BUCKETS ratio near to <= 1 */
/* 对字典结构进行空间扩容操作处理 目的是使得数组大小和元素数量的比例接近于1 */
int dictResize(dict *d) {
    int minimal;
	//检测当前是否处于重hash或者不能改变尺寸阶段
    if (!dict_can_resize || dictIsRehashing(d)) 
		return DICT_ERR;
	//获取当前字典结构的元素值------->这个地方为什么只需要在第一张hash表中获取 而不是进行计算两张表的值----->不处于重hash那么一定只有第一张表中有数据
    minimal = d->ht[0].used;
	//特殊处理小于初始值的问题------>即创建的对应的hash表中的数组长度最小是4个
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
	//尝试进行扩展字典结构的尺寸信息
    return dictExpand(d, minimal);
}

/* Expand or create the hash table */
/* 创建或者对已有的字典结构进行扩展操作处理 */
/* 注意明确这里说的扩容只是将对应的空间结构分配好,并将设置到字典结构中的第二张hash表位置,等待进行数据转移处理 */
int dictExpand(dict *d, unsigned long size) {
    /* the size is invalid if it is smaller than the number of elements already inside the hash table */
	//检测当前是否处于重hash阶段或者需要扩展的尺寸小于元素数量值
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

	//创建一个临时的hash结构
    dictht n; /* the new hash table */
	//获取需要扩容到的大小
    unsigned long realsize = _dictNextPower(size);

    /* Rehashing to the same table size is not useful. */
	//进一步检测重新扩容的大小和当前的数组尺寸是否相同
    if (realsize == d->ht[0].size) 
		return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
	//对新的hash表结构进行相关初始化操作处理
    n.size = realsize;
    n.sizemask = realsize-1;
	//分配对应的数组空间
    n.table = zcalloc(realsize*sizeof(dictEntry*));
    n.used = 0;

    /* Is this the first initialization? If so it's not really a rehashing we just set the first hash table so that it can accept keys. */
	//此处检测是否是字典结构中首次进行扩容操作处理,即需要将配置好的hash表配置到字典结构的第一张hash表的位置上
	if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
	//给第二张hash表准备好空间,准备进行扩容重hash操作处理
    d->ht[1] = n;
    d->rehashidx = 0;
    return DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
/* 根据给定的索引数量进行重hash操作处理 */
int dictRehash(dict *d, int n) {
	//设置一个访问空桶个数的最大值,超过本值进行退出处理
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
	//检测当前是否还需要进行重hash操作处理
    if (!dictIsRehashing(d)) 
		return 0;
	
	//循环操作进行重hash操作处理----->即完成给定数量的槽位重hash操作处理
    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more elements because ht[0].used != 0 */
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
		//循环找到一个对应的可以移动节点的桶链表位置
        while(d->ht[0].table[d->rehashidx] == NULL) {
			//增加已经完成元素移动的索引值位置
            d->rehashidx++;
			//检测是否超过了预设的访问空桶数量
            if (--empty_visits == 0) 
				//返回还需要进行重hash操作的处理-------->即尚未完成所有元素的移动操作处理
				return 1;
        }
		//获取对应索引位置上的节点链表指向
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
		//循环本节点链表,进行节点元素的移动操作处理
        while(de) {
            uint64_t h;
			//记录下一个需要移动的节点位置
            nextde = de->next;
            /* Get the index in the new hash table */
			//计算对应的在新的hash表结构中的索引位置
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
			//设置本节点新的下一个元素的指向---------->此处完成断掉原始链的处理
            de->next = d->ht[1].table[h];
			//将本元素节点直接连接到对应hash表的索引位置上
            d->ht[1].table[h] = de;
			//减少第一张hash表中元素的数量
            d->ht[0].used--;
			//增加第二张表中的元素数量
            d->ht[1].used++;
			//设置下一个需要遍历的元素
            de = nextde;
        }
		//移动完对应的索引位置上的元素节点后,设置本索引位置上的元素节点指向置空
        d->ht[0].table[d->rehashidx] = NULL;
		//设置需要遍历的下一个索引位置
        d->rehashidx++;
    }

    /* Check if we already rehashed the whole table... */
	//检测是否已经完成所有元素的重hash操作处理
    if (d->ht[0].used == 0) {
		//释放第一张hash表中的空间
        zfree(d->ht[0].table);
		//设置第二张hash表为第一张表
        d->ht[0] = d->ht[1];
		//重置第二张表中的数据------>即相关数据置空
        _dictReset(&d->ht[1]);
		//设置不处于重hash操作处理标记
        d->rehashidx = -1;
		//返回完成本阶段的重hash操作处理
        return 0;
    }

    /* More to rehash... */
	//返回还有索引位置需要进行重hash操作处理
    return 1;
}

/* 获取当前时间对应的毫秒值 */
long long timeInMilliseconds(void) {
    struct timeval tv;
	//获取对应的时间
    gettimeofday(&tv,NULL);
	//计算对应的毫秒值
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash for an amount of time between ms milliseconds and ms+1 milliseconds */
/* 在给定的时间内进行重hash操作处理--------->本函数主要是进行键管理操作是进行的 即完成在指定时间内处理键的迁移操作处理 */
int dictRehashMilliseconds(dict *d, int ms) {
	//记录进行重hash操作处理的时间
    long long start = timeInMilliseconds();
    int rehashes = 0;
	
	//在指定的时间内进行重hash操作次数处理 这个地方有一个问题 如果对应的一个槽位上元素数量过多 那么占据的时间可能会很长
    while(dictRehash(d,100)) {
        rehashes += 100;
		//检测是否时间已经超过了对应的时间
        if (timeInMilliseconds()-start > ms) 
			break;
    }
	//返回进行重hash操作处理对应的大约次数
    return rehashes;
}

/* This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2 while it is actively used. */
/* 在没有安全迭代器的情况下尝试进行一次重hash操作处理 */
static void _dictRehashStep(dict *d) {
	//检测当前字典结构是否存在迭代器---->如果有对应的迭代器 进不进行数据元素的迁移操作处理了
    if (d->iterators == 0) 
		//没有的情况下,尝试进行一次重hash处理操作----->即尽量完成一个索引节点的位置移动操作处理------>注意不是一个元素 而是一个索引位置上的所有节点
		dictRehash(d,1);
}

/* Add an element to the target hash table */
/* 在字典结构中尝试添加一个元素 */
int dictAdd(dict *d, void *key, void *val) {
	//尝试向字典结构中插入对应的键对象-----> 返回null说明本键对象已经存在 返回对象说明没有本键对象,同时创建了本节点对象,进行返回本节点对象,方便进行值对象的设置处理
    dictEntry *entry = dictAddRaw(d,key,NULL);
	//检测返回对象是否为null 即是否已经存在了对应的键对象
	if (!entry) 
		//说明对应的键对象已经存在了,返回插入失败标识
		return DICT_ERR;
	//将对应的值对象设置到对应的插入节点上
    dictSetVal(d, entry, val);
	//返回插入键值对成功标识
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
 /* 向字典中添加对应的键对象
  *   如果在字典结构中存储本键对象 那么返回值为null 但是可以设置existing参数来接收对应的节点对应的指向
  *   如果在字典结构中不存在本键对象 那么就创建一个对应的元素节点 将对应的键部分设置的元素节点上,同时将节点插入到字典对应的索引位置上 同时返回对应的新创建的本节点位置指向
  */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing) {
    long index;
    dictEntry *entry;
    dictht *ht;
	//检测当前是否处于重hash阶段中 即是否处于数据搬动过程中
    if (dictIsRehashing(d)) 
		//触发一次数据槽位的迁移操作处理  即尽快完成重hash阶段
		_dictRehashStep(d);

    /* Get the index of the new element, or -1 if the element already exists. */
	//首先检测对应的字典结构中是否有对应的本键对象   返回-1说明存着,同时existing记录了对应的节点的指向
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed more frequently. */
    //根据是否处于重hash阶段来确定在那张hash表中进行插入元素操作处理
    //通过此处可以发现 如果处于重hash阶段 新插入的节点会直接放置到第二张hash表结构上
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
	//分配对应的节点实体结构
    entry = zmalloc(sizeof(*entry));
	//设置新节点对应的下一个位置指向
    entry->next = ht->table[index];
	//将新节点插入到对应索引位置,即新插入的节点都处于节点链表的头部位置
    ht->table[index] = entry;
	//设置元素个数增加操作处理
    ht->used++;

    /* Set the hash entry fields. */
	//给创建的新节点设置字段键部分
    dictSetKey(d, entry, key);
	//返回新创建的元素节点-------->此处返回它的目的是进行值对象的设置处理
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update operation. */
/* 在字典结构中添加或者替换对应的键值对处理 */
int dictReplace(dict *d, void *key, void *val) {
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key does not exists dictAdd will succeed. */
	//首先尝试向字典结构中添加对应的键对象
    entry = dictAddRaw(d,key,&existing);
	//检测是否是插入新的节点元素成功
    if (entry) {
		//给新添加的节点元素设置对应的值对象
        dictSetVal(d, entry, val);
		//返回添加元素成功的标识
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the reverse. */
    //首先记录原始节点的数据
    auxentry = *existing;
	//给对应的节点设置需要替换的值对象
    dictSetVal(d, existing, val);
	//最后释放节点元素原始的值对象空间
    dictFreeVal(d, &auxentry);
	//返回替换元素节点值信息成功标识
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
/* 在字典结构中添加或者查找对应的键对象 */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
	//尝试添加对应的键对象
    entry = dictAddRaw(d,key,&existing);
	//如果是新增的节点 直接返回新创建的对应键的节点 如果存在 就返回对应存在的节点指向
    return entry ? entry : existing;
}

/* Search and remove an element. This is an helper function for
 * dictDelete() and dictUnlink(), please check the top comment of those functions. */
/* 在字典结构中删除对应的键对象的节点   */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;
	
	//首先检测字典中是否有元素
    if (d->ht[0].used == 0 && d->ht[1].used == 0) 
		return NULL;
	//检测当前是否处于重hash阶段
    if (dictIsRehashing(d)) 
		//尝试进行一次数据槽位的转移操作处理
		_dictRehashStep(d);
	//获取键对应的hash值
    h = dictHashKey(d, key);
	
	//循环两张hash表 在对应的索引位置查找是否有对应的键对象
    for (table = 0; table <= 1; table++) {
		//获取对应的索引值位置
        idx = h & d->ht[table].sizemask;
		//获取索引值对应的元素节点链表
        he = d->ht[table].table[idx];
		//用于记录待删除的节点的前一个节点的位置指向
        prevHe = NULL;
		//循环链表节点,检测是否有对应的键对象
        while(he) {
			//检测对应的键对象是否相等
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
				//检测是否有前驱节点 即在链表头直接找到了对应的待删除节点
                if (prevHe)
					//直接进行链接操作处理
                    prevHe->next = he->next;
                else
					//设置后续节点为链表的头节点
                    d->ht[table].table[idx] = he->next;
				//检测是否需要进行空间的释放操作处理
                if (!nofree) {
					//释放键对象空间
                    dictFreeKey(d, he);
					//释放值对象空间
                    dictFreeVal(d, he);
					//释放节点结构空间
                    zfree(he);
                }
				//设置字典元素减一操作处理
                d->ht[table].used--;
				//返回对应的删除节点的指向
                return he;
            }
			//记录本次遍历的节点, 方便后期删除节点时能够进行链接后续节点的处理
            prevHe = he;
			//设置需要遍历的下一个位置
            he = he->next;
        }
		//检测当前是否处于重hash操作处理              如果不是就可以直接跳出,不进行查找第二个hash表操作处理了
        if (!dictIsRehashing(d)) 
			break;
    }
	//没有找到返回空对象
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the element was not found. */
/* 在字典结构中删除对应键的节点 并释放对应的空间 */
/* free(str)后指针仍然指向原来的堆地址，即你仍然可以继续使用，但很危险，
 * 因为操作系统已经认为这块内存可以使用，他会毫不考虑的将他分配给其他程序，
 * 于是你下次使用的时候可能就已经被别的程序改掉了，这种情况就叫“野指针”，所以最好free（）了以后再置空str = NULL;即本程序已经放弃再使用他。
 */
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
/* 在字典结构中卸载对应键的节点元素,但不删除此元素节点,返回给用户进行操作处理 */
dictEntry *dictUnlink(dict *ht, const void *key) {
	//进行不释放空间的删除节点操作处理
    return dictGenericDelete(ht,key,1);
}

/* You need to call this function to really free the entry after a call to dictUnlink(). It's safe to call this function with 'he' = NULL. */
/* 将给定的节点元素进行释放空间操作处理 */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
	//检测给定的删除空间的节点是否存在
    if (he == NULL) 
		return;
	//释放对应的键空间
    dictFreeKey(d, he);
	//释放对应的值空间
    dictFreeVal(d, he);
	//释放对应的节点空间
    zfree(he);
}

/* Destroy an entire dictionary */
/* 释放对应的hash表中的数据和自身结构占据的空间 */
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    unsigned long i;

    /* Free all the elements */
	//循环删除hash表结构中的所有元素节点
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;
		//检测是否设置了回调函数 同时每隔 65535 来触发一次回调函数
        if (callback && (i & 65535) == 0) 
			callback(d->privdata);
		//检测当前索引位置上是否有节点链表需要删除
        if ((he = ht->table[i]) == NULL) 
			continue;
		//循环删除节点链表上的节点数据
        while(he) {
			//记录下一个需要删除的节点
            nextHe = he->next;
			//释放键空间
            dictFreeKey(d, he);
			//释放值空间
            dictFreeVal(d, he);
			//释放节点结构空间
            zfree(he);
			//减少元素个数
            ht->used--;
			//设置下一个需要删除的节点指向
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
	//释放对应的hash表对应的数组的空间
    zfree(ht->table);
    /* Re-initialize the table */
	//释放对应的hash表结构
    _dictReset(ht);
	//返回释放空间成功标识
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
/* 释放对应的字典结构 */
void dictRelease(dict *d) {
	//释放第一张hash表中的数据
    _dictClear(d,&d->ht[0],NULL);
	//释放第二张hash表中的数据
    _dictClear(d,&d->ht[1],NULL);
	//释放字典结构的空间
    zfree(d);
}

/* 在字典结构中查询对应键对象的节点信息 */
dictEntry *dictFind(dict *d, const void *key) {
    dictEntry *he;
    uint64_t h, idx, table;
	
	//首先检测对应的字典是否是有元素节点
    if (d->ht[0].used + d->ht[1].used == 0) 
		return NULL; /* dict is empty */
	//检测当前是否处于重hash处理阶段
    if (dictIsRehashing(d)) 
		//尝试进行一次数据槽位的迁移操作处理
		_dictRehashStep(d);
	//获取键对象对应的hash值
    h = dictHashKey(d, key);
	//循环两张表结构查询是否有对应的键对应的节点
    for (table = 0; table <= 1; table++) {
		//获取对应的索引位置
        idx = h & d->ht[table].sizemask;
		//获取对应索引位置上的元素链表
        he = d->ht[table].table[idx];
		//循环处理本链表上的元素,查找是否有对应的键对象
        while(he) {
			//检测是否有对应的键对象
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;
			//设置下一个需要遍历的元素节点
            he = he->next;
        }
		//检测是否处于重hash阶段---->没有就可以直接跳出了 因为第二张表中其实是没有数据的
        if (!dictIsRehashing(d)) 
			return NULL;
    }
	//返回没有找到对饮键对象的标识
    return NULL;
}

/* 获取对应键所对应的值对象 */
void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;
	//首先根据给定的键找到对应的节点
    he = dictFind(d,key);
	//获取找到节点上对应的值的信息
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
/* 获取当前时间字典结构的状态信息 将其组装成一个65bit的编码  用于确保不安全迭代器查询的字典结构是否发生了变化 */
/* 一个fingerprint为一个64位数值,用以表示某个时刻dict的状态,它由dict的一些属性通过位操作计算得到，当一个不安全的迭代器被初始化，我们就会得到该字典的fingerprint，并且在迭代器被释放时再一次检查fingerprint */
/* 如果两个fingerprints不同，这意味着这两个迭代器的user在进行字典迭代时执行了非法操作 */
long long dictFingerprint(dict *d) {
    long long integers[6], hash = 0;
    int j;
	
	//记录字典结构的相关信息
    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
	//返回当前状态下字典结构对应的编码信息
    return hash;
}

/* 获取字典结构的一个迭代器对象 */
dictIterator *dictGetIterator(dict *d) {
	//给对应的迭代器对象分配空间
    dictIterator *iter = zmalloc(sizeof(*iter));

	//初始化相关的迭代器参数
    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
	//返回迭代器对象的指向
    return iter;
}

/* 获取字典结构的一个安全迭代器对象 */
dictIterator *dictGetSafeIterator(dict *d) {
	//创建对应的迭代器对象
    dictIterator *i = dictGetIterator(d);
	//设置安全标志位为真
    i->safe = 1;
    return i;
}

/* 根据给定的迭代器来获取对应的下一个遍历到的元素节点 */
dictEntry *dictNext(dictIterator *iter) {
	//循环找到下一个可以遍历的元素  核心点在于两个hash表结构的切换上
    while (1) {
		//此处的处理是找到对应索引位置上对应的待遍历的首链表节点
        if (iter->entry == NULL) {
			//获取当前正在遍历的hash表
            dictht *ht = &iter->d->ht[iter->table];
			//检查是否处于初次遍历阶段
            if (iter->index == -1 && iter->table == 0) {
				//检查当前迭代器是否是安全迭代器
                if (iter->safe)
					//给对应的字典添加安全迭代器数量
                    iter->d->iterators++;
                else
					//获取当前字典结构对应的状态编码信息
                    iter->fingerprint = dictFingerprint(iter->d);
            }
			//设置下一个需要遍历的索引节点位置
            iter->index++;
			//检查需要遍历的索引节点位置是否超过了最大节点索引位置----->即有可能需要进行跳转到第二张hash表结构上
            if (iter->index >= (long) ht->size) {
				//检测当前是否处于重hash且遍历的是第一张表结构----->进行跳跃操作处理
                if (dictIsRehashing(iter->d) && iter->table == 0) {
					//设置遍历的是第二张hash表结构
                    iter->table++;
					//设置开始遍历的索引位置
                    iter->index = 0;
					//设置对应的hash表结构的位置指向
                    ht = &iter->d->ht[1];
                } else {
					//不满足条件就可以跳出操作处理了------>即没有需要遍历的元素了
                    break;
                }
            }
			//获取对应索引位置的节点链表头位置
            iter->entry = ht->table[iter->index];
        } else {
			//设置当前遍历到的元素节点位置
            iter->entry = iter->nextEntry;
        }

		//检测当前遍历到的节点位置是否为空
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user may delete the entry we are returning. */
			//记录对应的下一次需要遍历到的位置-------->原因是用户有可能删除我们返回的节点位置
            iter->nextEntry = iter->entry->next;
			//返回当前遍历到的节点位置
            return iter->entry;
        }
    }
	//没有元素需要遍历直接返回空对象
    return NULL;
}

/* 释放对应的迭代器占据的空间 */
void dictReleaseIterator(dictIterator *iter) {
	//检测当前的迭代器是否处于刚刚创建还没有进行迭代操作处理-------->即还没有将对应的信息设置到字典上
    if (!(iter->index == -1 && iter->table == 0)) {
		//检测是否是安全迭代器
        if (iter->safe)
			//减少字典中安全迭代器的数量
            iter->d->iterators--;
        else
			//检测对应的迭代器迭代的字典结构是否发生了变化
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
	//进行空间释放操作处理
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to implement randomized algorithms */
/* 在字典结构中随机选择一个节点 这个地方有一个问题 就是随机对应槽位的过程 不确定性 可能会耗时很长 才能找到一个可用的链表头节点 所以应用要少用*/
/* 核心点在于随机槽位值 然后随机对应链表上元素数量值所在的节点元素 */
dictEntry *dictGetRandomKey(dict *d) {
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;
	//首先检测字典结构中是否有元素
    if (dictSize(d) == 0) 
		return NULL;
	//检测当前是否重hash过程中
    if (dictIsRehashing(d))
		//触发一个槽位的重hash操作处理
		_dictRehashStep(d);
	//进一步检测是否还处于重hash阶段
    if (dictIsRehashing(d)) {
        do {
            /* We are sure there are no elements in indexes from 0 to rehashidx-1 */
			//这个地方设置很精妙---->完成重hash操作处理的索引上一定没有值了 所以就不需要再次在上面进行随机元素了
            h = d->rehashidx + (random() % (d->ht[0].size + d->ht[1].size - d->rehashidx));
			//获取对应的元素节点链表
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] : d->ht[0].table[h];
        } while(he == NULL);
    } else {
		//循环操作处理,直到在第一个压缩列表的一个随机位置上找到了一个有元素链表的节点链表
        do {
			//获取对应的随机槽位索引值
            h = random() & d->ht[0].sizemask;
			//获取对应槽位上的链表头节点
            he = d->ht[0].table[h];
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and select a random index. */
    listlen = 0;
	//首先记录找到的元素节点链表指向
    orighe = he;
	//循环遍历获取元素的个数
    while(he) {
		//记录下一个遍历的元素位置
        he = he->next;
		//记录链上的元素数量值
        listlen++;
    }
	//获取一个对应个数的随机值
    listele = random() % listlen;
    he = orighe;
	//循环处理找到对应的随机值对应的节点位置
    while(listele--) 
		he = he->next;
	//返回对应的随机节点位置
    return he;
}

/* This function samples the dictionary to return a few keys from random locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey() at producing N elements. */
/* 返回count个key，并且将地址存在des数组中
 *   这个地方的查找策略好像是: 首先随机一个索引位置,然后从给定的索引位置开始,查找在1和2表中对应的索引位置上的元素添加到存储集合中，一次向后进行遍历后续索引位置,当出现特殊情况时需要重置索引位置进行查找出来
 * 注意本方法获取的元素不保值不会出现重复的情况
 */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

	//处理count大于等于字典的总节点数的情况
    if (dictSize(d) < count) 
		//设置返回的数量只能最多是元素数量
		count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
	//此处尝试进行对应次数的重hash操作处理--------->进行重hash的目的暂时不清楚   需要进一步分析------>感觉是目的让元素都集中在一个hash表中
    for (j = 0; j < count; j++) {
		//进一步检测是否处于重hash操作过程中
        if (dictIsRehashing(d))
			//进行尝试对一个索引位置上的元素进行重hash操作处理
            _dictRehashStep(d);
        else
            break;
    }
	
	//根据是否仍然处于重hash来确定需要遍历的表的数量						 如果正在rehash，则对两个tables进行操作，否则对一个
    tables = dictIsRehashing(d) ? 2 : 1;
	//初始设置对应的最大掩码值
    maxsizemask = d->ht[0].sizemask;
	//如果是对两个表，则要更新maxsizemask
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = d->ht[1].sizemask;

    /* Pick a random point inside the larger table. */
	//初始一个开始查询的索引位置
    unsigned long i = random() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
	//循环处理,直到找够对应数量的元素
    while(stored < count && maxsteps--) {
		//对指定数量的表进行遍历操作处理
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            //如果有两个表 同时当前处于处理第一个表 当前对应的索引小于已经重hash操作的索引时 进行特殊处理 即对应的索引位置在本1表中绝对没有数据了
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= d->ht[1].size)
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= d->ht[j].size) 
				continue; /* Out of range for this table. */
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = random() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = 8 * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction* bucketfn, void *privdata) {
    dictht *t0, *t1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    if (dictSize(d) == 0) 
		return 0;

    if (!dictIsRehashing(d)) {
        t0 = &(d->ht[0]);
        m0 = t0->sizemask;

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->sizemask;
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (bucketfn) bucketfn(privdata, &t1->table[v & m1]);
            de = t1->table[v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Expand the hash table if needed */
/* 检测是否有必要进行扩容操作处理 */
static int _dictExpandIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
	//首先检测是否已经处于了重hash操作处理中------>即正在进行扩容操作处理
    if (dictIsRehashing(d)) 
		return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
	//检测字典结构中的第一张hash表是否有对应的尺寸值 即字典处于开始节点 还尚未hash表分配对应的初始空间
    if (d->ht[0].size == 0) 
		//进行空间扩容初始化操作处理
		return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling the number of buckets. */
    //检测对应的元素个数和hash表的数组尺寸值之间是否满足了对应的比率关系----->满足就进行扩容操作处理
    if (d->ht[0].used >= d->ht[0].size && (dict_can_resize || d->ht[0].used/d->ht[0].size > dict_force_resize_ratio)) {
		//进行以2倍速率进行扩容操作处理
        return dictExpand(d, d->ht[0].used*2);
    }
	//返回对应的成功标识
    return DICT_OK;
}

/* Our hash table capability is a power of two */
/* 获取给定size所对应的2的最小次数所对应的数字 即后期分配的数组空间大小都是2的次数倍数字 */
static unsigned long _dictNextPower(unsigned long size) {
    unsigned long i = DICT_HT_INITIAL_SIZE;
	//检测扩展的长度值是否超过了最大值
    if (size >= LONG_MAX) 
		//返回一个固定的数组尺寸最值
		return LONG_MAX + 1LU;
	//循环获取给定size对应2的最小次方对应的数字
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* Returns the index of a free slot that can be populated with a hash entry for the given 'key'.
 * If the key already exists, -1 is returned and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the index is always returned in the context of the second (new) hash table. */
/* 返回找到对应键所对应的槽位 */
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing) {
    unsigned long idx, table;
    dictEntry *he;
    if (existing) 
		*existing = NULL;

    /* Expand the hash table if needed */
	//检测是否需要进行扩容操作处理-------> 需要明白扩容失败的原因: 
	//1 字典结构刚进行完开辟空间 第一个hash表还没有开启对应的空间处理 
	//2 字典结构中的元素 与对应的字典数组长度的比例超过一定的范围
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
	//循环遍历两张hash表进行检测对应的键对象是否在对应槽位的链表中
    for (table = 0; table <= 1; table++) {
		//根据键对应的hash值获取对应的槽位索引位置
        idx = hash & d->ht[table].sizemask;
        /* Search if this slot does not already contain the given key */
		//获取对应槽位上已经有的元素节点链表头
        he = d->ht[table].table[idx];
		//循环遍历对应的槽位链表
        while(he) {
			//检测对应的键值是否匹配---->即对应的键值对是否已经存储在字典结构中
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
				//检测是否配置了存储指向节点元素的指针
                if (existing) 
					//指向对应的键值对元素节点
					*existing = he;
				//返回不能插入对应的键值对的标识
                return -1;
            }
			//设置遍历的下一个元素指向
            he = he->next;
        }
		//检测当前是否处于重hash节点 通过此判断来确实是否查询第二张hash表
        if (!dictIsRehashing(d)) 
			break;
    }
	//返回找到的对应槽位
    return idx;
}

/* 清空字典结构中的数据,但不删除字典结构 */
void dictEmpty(dict *d, void(callback)(void*)) {
	//清空第一张表中的数据--->同时释放的表结构
    _dictClear(d,&d->ht[0],callback);
	//清空第二张表中的数据--->同时释放的表结构
    _dictClear(d,&d->ht[1],callback);
	//设置字典结构不处于重hash阶段
    d->rehashidx = -1;
	//设置字典结构迭代器数量为0
    d->iterators = 0;
}

/* 设置字典可以进行扩展尺寸处理 */
void dictEnableResize(void) {
    dict_can_resize = 1;
}

/* 设置字典不可以进行扩展尺寸处理 */
void dictDisableResize(void) {
    dict_can_resize = 0;
}

/* 获取给定键对象对应的hash值 */
uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
/* 通过给定的hash值和对应的老键值来查找是否有对应的节点对象 */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;
	
	//首先检测字典中是否有对应的元素
    if (d->ht[0].used + d->ht[1].used == 0) 
		return NULL; /* dict is empty */
	//循环处理两个hash表
    for (table = 0; table <= 1; table++) {
		//获取对应的索引位置
        idx = hash & d->ht[table].sizemask;
		//获取对应索引位置上的元素链表指向
        heref = &d->ht[table].table[idx];
        he = *heref;
		//循环遍历 查找是否有匹配上的节点
        while(he) {
			//检测是否相等
            if (oldptr==he->key)
                return heref;
			//设置需要遍历的下一个元素
            heref = &he->next;
            he = *heref;
        }
		//通过当前是否处于重hash来确定是否进行操作第二张表
        if (!dictIsRehashing(d)) 
			//在第一张表中 没有找到 且没有第二张表数据 直接返回空对象就可以了
			return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dictht *ht, int tableid) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (ht->used == 0) {
        return snprintf(buf,bufsize, "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) 
		clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) 
			maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %ld\n"
        " number of elements: %ld\n"
        " different slots: %ld\n"
        " max chain length: %ld\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        tableid, (tableid == 0) ? "main hash table" : "rehashing target",
        ht->size, ht->used, slots, maxchainlen,
        (float)totchainlen/slots, (float)ht->used/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) 
			continue;
        if (l >= bufsize) 
			break;
        l += snprintf(buf+l,bufsize-l,
            "   %s%ld: %ld (%.02f%%)\n",
            (i == DICT_STATS_VECTLEN-1)?">= ":"",
            i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }

    /* Unlike snprintf(), teturn the number of characters actually written. */
    if (bufsize) 
		buf[bufsize-1] = '\0';
    return strlen(buf);
}

/* */
void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,&d->ht[0],0);
    buf += l;
    bufsize -= l;
	
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,&d->ht[1],1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) 
		orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef DICT_BENCHMARK_MAIN

#include "sds.h"

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2) {
    int l1,l2;
    DICT_NOTUSED(privdata);
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) 
		return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    sdsfree(val);
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0);

/* dict-benchmark [count] */
int main(int argc, char **argv) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType,NULL);
    long count = 0;

    if (argc == 2) {
        count = strtol(argv[1],NULL,10);
    } else {
        count = 5000000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,sdsfromlonglong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        sdsfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
}
#endif




