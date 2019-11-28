/* 
 * adlist.h - A generic doubly linked list implementation
 * 双向链接结构实现
 */

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */
/* 双向链接中节点元素结构 */
typedef struct listNode {
	//指向前一个节点的指针
    struct listNode *prev;
	//指向后一个节点的指针
    struct listNode *next;
	//存储对应的节点元素值
    void *value;
} listNode;

/* 双向链表中遍历节点的迭代器结构 */
typedef struct listIter {
	//指向待遍历的节点
    listNode *next;
	//记录遍历的方向
    int direction;
} listIter;

/* 双向链接结构 */
typedef struct list {
	//指向对应的头结点
    listNode *head;
	//指向对应的尾节点
    listNode *tail;
	//进行元素复制的处理函数
    void *(*dup)(void *ptr);
	//进行释放元素的处理函数
    void (*free)(void *ptr);
	//进行比较的处理函数
    int (*match)(void *ptr, void *key);
	//记录存储的元素个数
    unsigned long len;
} list;

/* Functions implemented as macros */
/* 双向链接结构中提供的宏函数 */
#define listLength(l) ((l)->len)//获取链表的元素个数
#define listFirst(l) ((l)->head)//获取链表的头节点
#define listLast(l) ((l)->tail)//获取链表的尾节点

#define listPrevNode(n) ((n)->prev)//获取给定节点指向的前一个节点
#define listNextNode(n) ((n)->next)//获取给定节点指向的后一个节点
#define listNodeValue(n) ((n)->value)//获取给定节点的元素指向

#define listSetDupMethod(l,m) ((l)->dup = (m))//设置链表的复制函数
#define listSetFreeMethod(l,m) ((l)->free = (m))//设置链表的释放函数
#define listSetMatchMethod(l,m) ((l)->match = (m))//设置链表的比较函数

#define listGetDupMethod(l) ((l)->dup)//获取链表的复制函数
#define listGetFree(l) ((l)->free)//获取链表的释放函数
#define listGetMatchMethod(l) ((l)->match)//获取链表的比较函数

/* Prototypes */
/* 双向链表结构提供的相关方法 */
list *listCreate(void);//创建对应的双向链表结构
void listEmpty(list *list);//释放链表结构中的所有元素
void listRelease(list *list);//释放给定的双向链表结构
list *listAddNodeHead(list *list, void *value);//在链表头插入新元素
list *listAddNodeTail(list *list, void *value);//在链表尾插入新元素
list *listInsertNode(list *list, listNode *old_node, void *value, int after);//在给定的老节点指定方向上添加节点
void listDelNode(list *list, listNode *node);//在链表中删除对应的节点元素
listIter *listGetIterator(list *list, int direction);//获取链表结构的一个迭代器对象
listNode *listNext(listIter *iter);//根据迭代器获取一个遍历的节点
void listReleaseIterator(listIter *iter);//释放迭代器对象占据的空间
list *listDup(list *orig);//拷贝指定的链表结构
listNode *listSearchKey(list *list, void *key);//搜索指向的元素是否在链表中
listNode *listIndex(list *list, long index);//根据给定的索引找到在链表中的节点
void listRewind(list *list, listIter *li);//初始化一个从头进行遍历的迭代器
void listRewindTail(list *list, listIter *li);//初始化一个从尾进行遍历的迭代器
void listRotate(list *list);//将链表中的尾元素节点放置到头节点前成为新的头节点
void listJoin(list *l, list *o);//将给定的链表元素链接到指定的链表的后面

/* Directions for iterators */
//记录迭代方向的宏定义
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __ADLIST_H__ */



