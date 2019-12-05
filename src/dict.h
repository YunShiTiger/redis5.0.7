/* 
 * Hash Tables Implementation.
 * redis中的字典结构实现
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 */

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

/* hash表结构中的节点元素结构 主要是链式结构 解决多hash值相同放置到同一个链中 */
typedef struct dictEntry {
	//存储对应的key值部分
    void *key;
	//存储对应的value值部分
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
	//指向下一个hash节点，用来解决hash键冲突（collision）问题
    struct dictEntry *next;
} dictEntry;

/* This is our hash table structure. Every dictionary has two of this as we implement incremental rehashing, for the old to the new table. */
/* redis中实现的hash表结构---->哈希表 */
typedef struct dictht {
	//存放一个数组的地址，数组存放着哈希表节点dictEntry的地址
    dictEntry **table;
	//哈希表table的大小，初始化大小为4 标识数组的大小 不是总元素的个数 当总元素的个数是数组大小的2倍时触发进行重hash操作处理
    unsigned long size;
	//掩码值用于将哈希值映射到table的位置索引。它的值总是等于(size-1)
    unsigned long sizemask;
	//记录哈希表已有的节点（键值对）数量
    unsigned long used;
} dictht;

/* dictType记录了字典结构中使用的一些公共的操作处理函数 */
typedef struct dictType {
	//根据key值计算hash值的处理函数
    uint64_t (*hashFunction)(const void *key);
	//复制key的处理函数
    void *(*keyDup)(void *privdata, const void *key);
	//复制value的处理函数
    void *(*valDup)(void *privdata, const void *obj);
	//比较key的处理函数
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);
	//销毁key的析构函数
    void (*keyDestructor)(void *privdata, void *key);
	//销毁value的析构函数
    void (*valDestructor)(void *privdata, void *obj);
} dictType;


/* redis中实现的字典结构 */
typedef struct dict {
	//指向dictType结构，dictType结构中包含自定义的函数，这些函数使得
    dictType *type;
    void *privdata;
	//用于存储数据的两张哈希表结构---->注意这个地方使用的不是柔性数组
    dictht ht[2];
	//rehash的标记，rehashidx==-1，表示没在进行rehash
    long rehashidx;          /* rehashing not in progress if rehashidx == -1 */
	//正在迭代的迭代器数量
    unsigned long iterators; /* number of iterators currently running */
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext() should be called while iterating. */
/* 进行遍历字典结构的迭代器结构 */
typedef struct dictIterator {
	//对应的需要遍历的字典结构指向
    dict *d;
	//当前遍历到的索引位置
    long index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
	//对应的记录不安全迭代器需要记录的当前字典的状态信息
    long long fingerprint;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

/* This is the initial size of every hash table */
//初始化hash表结构中数组的初始大小
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
//释放对应节点值部分空间的处理宏
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

//给对应节点值部分设置值的处理宏
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

//给对应的节点值部分设置值为对应的有符号整数数据的处理宏
#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

//给对应的节点值部分设置值为对应的无符号整数数据的处理宏
#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

//给对应的节点值部分设置值为对应的double类型数据的处理宏
#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

//释放对应节点建部分空间的处理宏
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

//给对应节点键部分设置值的处理宏
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

//用于比较两个键部分是否相等的处理宏
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

//获取键对象对应的hash值
#define dictHashKey(d, key) (d)->type->hashFunction(key)
//获取对应的键部分
#define dictGetKey(he) ((he)->key)
//获取对应的值部分
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
//获取槽位值
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
//获取当前字典结构的元素个数
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
//获取当前字典结构是否处于重hash中
#define dictIsRehashing(d) ((d)->rehashidx != -1)

/* API */
/* 字典结构中提供的相关API函数 */
dict *dictCreate(dictType *type, void *privDataPtr);//创建一个新的字典结构
int dictExpand(dict *d, unsigned long size);//将对应的字典结构扩充到指定尺寸
int dictAdd(dict *d, void *key, void *val);//在字典结构中尝试添加一个元素
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);//尝试向字典中添加对应的键对象
dictEntry *dictAddOrFind(dict *d, void *key);//在字典结构中添加或者查找对应的键对象
int dictReplace(dict *d, void *key, void *val);//在字典结构中添加或者替换对应的键值对处理
int dictDelete(dict *d, const void *key);//在字典结构中删除对应键的节点 并释放对应的空间
dictEntry *dictUnlink(dict *ht, const void *key);//在字典结构中卸载对应键的节点元素,但不删除此元素节点,返回给用户进行操作处理
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);//将给定的节点元素进行释放空间操作处理
void dictRelease(dict *d);//释放对应的字典结构空间和对应的数据
dictEntry * dictFind(dict *d, const void *key);//在字典结构中查询对应键对象的节点信息
void *dictFetchValue(dict *d, const void *key);//获取对应键所对应的值对象
int dictResize(dict *d);//对字典结构进行空间扩容操作处理 目的是使得数组大小和元素数量的比例接近于1
dictIterator *dictGetIterator(dict *d);//获取字典结构的一个迭代器对象
dictIterator *dictGetSafeIterator(dict *d);//获取字典结构的一个安全迭代器对象
dictEntry *dictNext(dictIterator *iter);//根据给定的迭代器来获取对应的下一个遍历到的元素节点
void dictReleaseIterator(dictIterator *iter);//释放对应的迭代器占据的空间
dictEntry *dictGetRandomKey(dict *d);//在字典结构中随机选择一个节点
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);//
void dictGetStats(char *buf, size_t bufsize, dict *d);//
uint64_t dictGenHashFunction(const void *key, int len);//
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);//
void dictEmpty(dict *d, void(callback)(void*));//清空字典结构中的数据,但不删除字典结构
void dictEnableResize(void);//设置字典可以进行扩展尺寸处理
void dictDisableResize(void);//设置字典不可以进行扩展尺寸处理
int dictRehash(dict *d, int n);//根据给定的索引数量进行重hash操作处理
int dictRehashMilliseconds(dict *d, int ms);//在给定的时间内进行重hash操作处理
void dictSetHashFunctionSeed(uint8_t *seed);//
uint8_t *dictGetHashFunctionSeed(void);//
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);//
uint64_t dictGetHash(dict *d, const void *key);// 获取给定键对象对应的hash值
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);//通过给定的hash值和对应的老键值来查找是否有对应的节点对象

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */




