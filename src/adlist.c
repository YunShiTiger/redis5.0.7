/* 
 * adlist.c - A generic doubly linked list implementation
 */
#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 * On error, NULL is returned. Otherwise the pointer to the new list. */
/* 创建对应的双向链表结构 在函数内部分配空间 给函数外部使用 返回的是空间指向 需要外部单独处理释放 */
list *listCreate(void) {
	//首先声明对应的结构
    struct list *list;
	//进行分配空间并检测是否分配空间正常
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
	//初始化头节点和尾节点指向
    list->head = list->tail = NULL;
	//设置当前存储的元素个数为0
    list->len = 0;
	//初始化3个函数为空
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
	//返回对应的双向链表结构指向
    return list;
}

/* Remove all the elements from the list without destroying the list itself. */
/* 释放链表结构中所有元素节点占据的空间 */
void listEmpty(list *list) {
    unsigned long len;
    listNode *current, *next;
	//记录当前的头节点元素
    current = list->head;
	//记录需要消耗元素的个数
    len = list->len;
	//循环进行消耗元素
    while(len--) {
		//记录下一个需要释放的元素
        next = current->next;
		//获取是否配置元素释放函数
        if (list->free) 
			//触发释放对应的节点元素中的内容
			list->free(current->value);
		//释放对应的节点占据的空间
        zfree(current);
		//记录下一个需要移除的元素
        current = next;
    }
	//重置头节点和尾节点的指向为空
    list->head = list->tail = NULL;
	//重置元素个数为0
    list->len = 0;
}

/* Free the whole list. This function can't fail. */
/* 释放给定的双向链表结构 */
void listRelease(list *list){
	//首先释放对应的链表元素空间
    listEmpty(list);
	//然后释放对应的双向链表占据的空间
    zfree(list);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/* 在链表头添加新节点元素 */
list *listAddNodeHead(list *list, void *value) {
    listNode *node;
	//分配节点空间并检测是否成功
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//记录元素节点的值指向
    node->value = value;
	//检测当前是否有元素
    if (list->len == 0) {
		//更新链表的头尾节点指向
        list->head = list->tail = node;
		//更新节点元素的前置和后置节点指向
        node->prev = node->next = NULL;
    } else {
		//设置新节点的前置指向
        node->prev = NULL;
		//设置新节点的后置指向
        node->next = list->head;
		//处理老的头节点的前置指向
        list->head->prev = node;
		//设置链表头节点指向
        list->head = node;
    }
	//设置新的链表元素个数
    list->len++;
	//返回链表指向
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/* 在链表尾添加节点元素 */
list *listAddNodeTail(list *list, void *value) {
    listNode *node;
	//分配节点空间并检测是否成功
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//记录元素节点的值指向
    node->value = value;
	//检测当前是否有元素
    if (list->len == 0) {
		//更新链表的头尾节点指向
        list->head = list->tail = node;
		//更新节点元素的前置和后置节点指向
        node->prev = node->next = NULL;
    } else {
		//设置新节点的前置指向
        node->prev = list->tail;
		//设置新节点的后置指向
        node->next = NULL;
		//处理老的尾节点的后置指向
        list->tail->next = node;
		//设置链表尾节点指向
        list->tail = node;
    }
	//设置新的链表元素个数
    list->len++;
	//返回链表指向
    return list;
}

/* 在给定的老节点指定方向上添加节点 */
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;
	//创建一个新的节点空间
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//给新节点设置元素指向
    node->value = value;
	//根据方向进行插入节点操作处理
    if (after) {
		//设置新节点的前置和后置指向
        node->prev = old_node;
        node->next = old_node->next;
		//检测老节点是否是尾节点
        if (list->tail == old_node) {
			//更新链表尾节点指向
            list->tail = node;
        }
    } else {
    	//设置新节点的前置和后置指向
        node->next = old_node;
        node->prev = old_node->prev;
		//检测老节点是否是头节点
        if (list->head == old_node) {
			//更新链表头节点指向
            list->head = node;
        }
    }
	//更新新节点的前置节点的后置指向
    if (node->prev != NULL) {
        node->prev->next = node;
    }
	//更新新节点的后置节点的后置指向
    if (node->next != NULL) {
        node->next->prev = node;
    }
	//增加元素个数值
    list->len++;
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 * This function can't fail. */
/* 在链表中删除对应的节点元素 */
void listDelNode(list *list, listNode *node) {
	//处理节点的前置节点指向变动
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
	//处理节点的后置节点指向变动
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
	//释放元素节点中值元素占据的空间
    if (list->free) 
		list->free(node->value);
	//释放元素节点占据的空间
    zfree(node);
	//减少元素节点数量
    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
/* 给对应的链表创建迭代器 */
listIter *listGetIterator(list *list, int direction) {
    listIter *iter;
	//给迭代器创建对应的空间并检测是否成功
    if ((iter = zmalloc(sizeof(*iter))) == NULL) 
		return NULL;
	//根据配置的遍历方向设置指向的遍历节点
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
	//设置迭代器遍历的方向
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
/* 释放迭代器对象占据的空间 */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
/* 初始化一个从头进行遍历的迭代器 */
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

/* 初始化一个从尾进行遍历的迭代器 */
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 * */
/* 根据迭代器获取一个遍历的节点 */
listNode *listNext(listIter *iter) {
	//获取待遍历的节点指向
    listNode *current = iter->next;
	//检测待遍历的节点是否存在
    if (current != NULL) {
		//根据迭代器配置的方向设置下一个待遍历的节点指向
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
	//返回遍历到的节点
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
/* 拷贝指定的链表结构 */
list *listDup(list *orig) {
    list *copy;
    listIter iter;
    listNode *node;
	//首先创建链表节点并检测分配空间是否成功
    if ((copy = listCreate()) == NULL)
        return NULL;
	//拷贝对应的三个处理函数
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
	//初始化一个从头开始遍历的迭代器
    listRewind(orig, &iter);
	//使用迭代器循环遍历对应的链表结构
    while((node = listNext(&iter)) != NULL) {
        void *value;
		//检测是否配置了拷贝处理函数
        if (copy->dup) {
			//拷贝对应的值元素
            value = copy->dup(node->value);
			//检测拷贝是否成功
            if (value == NULL) {
				//拷贝失败就释放新创建的链表结构
                listRelease(copy);
                return NULL;
            }
        } else
            value = node->value;
		//从尾部进行插入节点操作处理
        if (listAddNodeTail(copy, value) == NULL) {
			//插入失败进行释放新创建的链表结构
            listRelease(copy);
            return NULL;
        }
    }
	//返回拷贝后的链表结构
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists NULL is returned. */
/*  搜索指向的元素是否在链表中 */
listNode *listSearchKey(list *list, void *key) {
    listIter iter;
    listNode *node;
	//初始化一个迭代器
    listRewind(list, &iter);
	//循环遍历链表中的节点
    while((node = listNext(&iter)) != NULL) {
		//检测是否配置比较函数
        if (list->match) {
			//进行比较操作处理
            if (list->match(node->value, key)) {
                return node;
            }
        } else {
            if (key == node->value) {
                return node;
            }
        }
    }
	//没有找到返回对应的空对象
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
/* 根据给定的索引找到在链表中的节点    */
listNode *listIndex(list *list, long index) {
    listNode *n;
	//根据索引正负值确定查找方向
    if (index < 0) {
        index = (-index)-1;
        n = list->tail;
		//循环遍历查找对应索引的节点
        while(index-- && n) 
			n = n->prev;
    } else {
        n = list->head;
		//循环遍历查找对应索引的节点
        while(index-- && n) 
			n = n->next;
    }
	//返回遍历到的索引节点的位置指向
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
/* 将链表中的尾元素节点放置到头节点前 即实现将尾节点移动到当前头节点前面成为新的头节点*/
void listRotate(list *list) {
	//首先记录对应的尾节点指向
    listNode *tail = list->tail;

	//检测是否需要进行翻转操作处理
    if (listLength(list) <= 1) 
		return;

    /* Detach current tail */
	//处理记录新的尾节点处理
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
	//处理老的头节点指向问题
    list->head->prev = tail;
	//设置新的头节点的前置和后置指向
    tail->prev = NULL;
    tail->next = list->head;
	//设置新的头节点指向
    list->head = tail;
}

/* Add all the elements of the list 'o' at the end of the list 'l'. The list 'other' remains empty but otherwise valid. */
/* 将给定的链表元素链接到指定的链表的后面 */
void listJoin(list *l, list *o) {
	//将o链表的头节点链接到l链表的尾节点上
    if (o->head)
        o->head->prev = l->tail;
	//检测l链表是否有元素
    if (l->tail)
		//将l链表的尾节点链接到o链表的头节点上
        l->tail->next = o->head;
    else
		//没有元素 需要初始化l链表的头节点指向
        l->head = o->head;
	//处理尾节点的指向
    if (o->tail) 
		l->tail = o->tail;
	//修改元素的个数值
    l->len += o->len;

    /* Setup other as an empty list. */
	//置空o链表的头和尾节点指向
    o->head = o->tail = NULL;
	//清空元素值
    o->len = 0;
}



