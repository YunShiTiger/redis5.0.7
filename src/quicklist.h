/* 
 * quicklist.h - A generic doubly linked quicklist implementation
 */

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

/* Node, quicklist, and Iterator are the only data structures used currently. */

/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true if node is temporarry decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 10 bits, free for future use; pads out the remainder of 32 bits */
/* quicklist结构中对应的链表节点结构 */
typedef struct quicklistNode {
	//前驱节点指针
    struct quicklistNode *prev;
	//后继节点指针
    struct quicklistNode *next;
	//不设置压缩数据参数recompress时指向一个ziplist结构
    //设置压缩数据参数recompress指向quicklistLZF结构
    unsigned char *zl;
	//压缩列表ziplist的总长度--------------->这个值在进行压缩操作处理是
    unsigned int sz;             /* ziplist size in bytes */
	//ziplist中包含的节点数，占16 bits长度
    unsigned int count : 16;     /* count of items in ziplist */
	//表示是否采用了LZF压缩算法压缩quicklist节点，1表示压缩过，2表示没压缩，占2 bits长度
    unsigned int encoding : 2;   /* RAW==1 or LZF==2 */
	//表示一个quicklistNode节点是否采用ziplist结构保存数据，2表示压缩了，1表示没压缩，默认是2，占2bits长度
    unsigned int container : 2;  /* NONE==1 or ZIPLIST==2 */
	//标记quicklist节点的ziplist之前是否被解压缩过，占1bit长度
	//如果recompress为1，则等待被再次压缩
    unsigned int recompress : 1; /* was this node previous compressed? */
	//测试时使用
    unsigned int attempted_compress : 1; /* node can't compress; too small */
	//额外扩展位，占10bits长度
    unsigned int extra : 10; /* more bits to steal for future usage */
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->zl is compressed, node->zl points to a quicklistLZF */
/* 当指定使用lzf压缩算法压缩ziplist的entry节点时，quicklistNode结构的zl成员指向quicklistLZF结构 */
typedef struct quicklistLZF {
	//表示被LZF算法压缩后的ziplist的大小
    unsigned int sz; /* LZF size in bytes*/
	//保存压缩后的ziplist的数组，柔性数组
    char compressed[];
} quicklistLZF;

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: -1 if compression disabled, otherwise it's the number of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor. */
/* quicklist列表结构的结构信息 */
typedef struct quicklist {
	//指向头部(最左边)quicklist节点的指针
    quicklistNode *head;
	//指向尾部(最右边)quicklist节点的指针
    quicklistNode *tail;
	//ziplist中的entry节点计数器---->即存储的总元素数量
    unsigned long count;        /* total count of all entries in all ziplists */
	//quicklist的quicklistNode节点计数器
    unsigned long len;          /* number of quicklistNodes */
	//保存ziplist的大小，配置文件设定，占16bits
    int fill : 16;              /* fill factor for individual nodes */
	//保存压缩程度值，配置文件设定，占16bits，0表示不压缩
    unsigned int compress : 16; /* depth of end nodes not to compress;0=off */
} quicklist;

/* quicklist列表结构的迭代器 */
typedef struct quicklistIter {
	//指向所属的quicklist结构的指针
    const quicklist *quicklist;
	//指向当前迭代的quicklist节点的指针
    quicklistNode *current;
	//指向当前quicklist节点中迭代的ziplist中对应的元素    ---->不是ziplist结构的指向
    unsigned char *zi;
	//当前ziplist结构中的偏移量
    long offset; /* offset in current ziplist */
	//进行迭代的方向
    int direction;
} quicklistIter;

/* 用于解析quicklist结构中一个节点元素对应的信息结构 */
typedef struct quicklistEntry {
	//指向所属的quicklist结构的指针
    const quicklist *quicklist;
	//指向所属的quicklistNode节点的指针
    quicklistNode *node;
	//指向当前ziplist结构的中遍历的节点元素指向 不是ziplist结构的指向
    unsigned char *zi;
	//指向当前ziplist结构的字符串vlaue成员
    unsigned char *value;
	//指向当前ziplist结构的整数value成员
    long long longval;
	//保存当前ziplist结构的字节数大小
    unsigned int sz;
	//保存相对ziplist的偏移量
    int offset;
} quicklistEntry;

/* 用于标识对应的头和尾 */
#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
/* 用于表示quicklistNode节点上存储的ziplist数据是否进行压缩操作处理的宏 1 原始类型 2 压缩类型 */
#define QUICKLIST_NODE_ENCODING_RAW 1
#define QUICKLIST_NODE_ENCODING_LZF 2

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist container formats */
/* 用于表示quicklistNode节点上存储的ziplist数据格式 是ziplist结构 还是压缩后的数据格式 */
#define QUICKLIST_NODE_CONTAINER_NONE 1
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2

/* 获取对应的quicklist链表节点对应的数据ziplist是否进行压缩处理 */
#define quicklistNodeIsCompressed(node) ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
quicklist *quicklistCreate(void);//创建对应的quicklist结构,并获取对应的空间指向
quicklist *quicklistNew(int fill, int compress);//创建指定填充因子和压缩因子的quicklist结构
void quicklistSetCompressDepth(quicklist *quicklist, int depth);//配置quicklist结构的压缩因子
void quicklistSetFill(quicklist *quicklist, int fill);//配置quicklist结构的填充因子
void quicklistSetOptions(quicklist *quicklist, int fill, int depth);//配置quicklist结构的压缩因子和填充因子
void quicklistRelease(quicklist *quicklist);//释放对应的quicklist结构中数据占据的空间和结构自身占据的空间
int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz);//在quicklist结构的头部链表节点上插入一个数据节点  ----->同时数据节点插入到对应的ziplist的头部
int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz);//在quicklist结构的尾部链表节点上插入一个数据节点  ----->同时数据节点插入到对应的ziplist的尾部
void quicklistPush(quicklist *quicklist, void *value, const size_t sz, int where);//封装的基于给定参数进行节点数据插入操作的处理---->注意这个地方是插入数据节点
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl);//将给定的ziplist结构数据链接到quicklist结构的尾链表节点后
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist, unsigned char *zl);//循环将一个ziplist中的元素插入到quicklist结构的尾部
quicklist *quicklistCreateFromZiplist(int fill, int compress, unsigned char *zl);//根据给定的压缩参数和填充参数以及存在的ziplist来构建对应的quicklist结构
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *node, void *value, const size_t sz);//封装的在给定的节点信息后插入元素
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *node, void *value, const size_t sz);//封装的在给定的节点信息前插入元素
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry);//删除给定元素的节点,如果删除成功需要对应迭代器的参数数据,用于指向下一个需要进行遍历的元素
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data, int sz);//在quicklist结构上替换给定索引位置上的数据
int quicklistDelRange(quicklist *quicklist, const long start, const long stop);//在quicklist结构中删除从指定索引位置开始的指定元素的数量
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction);//获取quicklist结构的指定方向上的迭代器
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist, int direction, const long long idx);//根据给定的索引位置和方向初始一个迭代器对象
int quicklistNext(quicklistIter *iter, quicklistEntry *node);//通过迭代器获取下一个可以遍历到的元素节点信息
void quicklistReleaseIterator(quicklistIter *iter);//释放对应的迭代器对象
quicklist *quicklistDup(quicklist *orig);//拷贝对应的quicklist结构
int quicklistIndex(const quicklist *quicklist, const long long index, quicklistEntry *entry);//获取指定索引位置处理的元素节点信息
void quicklistRewind(quicklist *quicklist, quicklistIter *li);
void quicklistRewindTail(quicklist *quicklist, quicklistIter *li);
void quicklistRotate(quicklist *quicklist);//将quicklist结构中的最后一个元素移动到第一个位置上
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data, unsigned int *sz, long long *sval, void *(*saver)(unsigned char *data, unsigned int sz));//在quicklist结构中进行数据弹出操作处理
int quicklistPop(quicklist *quicklist, int where, unsigned char **data, unsigned int *sz, long long *slong);//默认的在quicklist结构中进行数据弹出操作的处理函数
unsigned long quicklistCount(const quicklist *ql);//获取当前quicklist结构中总共多少数据元素节点
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len);//比较给定的两个字符串数据指向的内容是否相同
size_t quicklistGetLzf(const quicklistNode *node, void **data);//获取给定链表节点的压缩数据,同时返回对应的未进行压缩前的总字节数

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[]);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */




