/*
 * redis中列表对象的命令实现函数
 * 通过查看代码实现知道 现在列表的实现方式只有quicklist类型 其实quicklist类型就是双向链表和压缩列表的结合体
 * 以前版本代码列表可以用双向链表或者压缩列表来进行实现的
 */

#include "server.h"

/*-----------------------------------------------------------------------------
 * List API
 * 列表对象的相关api函数
 *----------------------------------------------------------------------------*/

/* The function pushes an element to the specified list object 'subject', at head or tail position as specified by 'where'.
 * There is no need for the caller to increment the refcount of 'value' as the function takes care of it if needed. */
/* 将对应的值插入到列表对象中 */
void listTypePush(robj *subject, robj *value, int where) {
	//检测列表对象的实现方式是否是quicklist类型
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
		//解析对应的插入位置标识
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
		//------------------------->这个地方为什么进行解密对象操作处理呢??????????????????????????????????
        value = getDecodedObject(value);
	 	//获取对应的插入数据的长度
        size_t len = sdslen(value->ptr);
		//将对应的数据插入到对应的列表对象中
        quicklistPush(subject->ptr, value->ptr, len, pos);
		//减少对应对象的引用计数--------->这个地方为什么要减少引用计数??????????????????????????
        decrRefCount(value);
    } else {
		//列表对象的类型实现方式有问题
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, unsigned int sz) {
    return createStringObject((char*)data,sz);
}

/* 在列表对象的指定标识位置进行数据元素的弹出操作处理 */
robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;
	
	//根据给定的标识获取对应的弹出索引位置
    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
	//检测对应的列表对象是否是quicklist结构类型
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
		//尝试进行弹出元素处理
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value, NULL, &vlong, listPopSaver)) {
			//检测获取到的数据是否是整数类型数据
            if (!value)
				//将获取到的整数数据转换成对应的字符串数据对象
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
	//返回获取到的元素对象
    return value;
}

/* 获取列表对象的元素数量 */
unsigned long listTypeLength(const robj *subject) {
	//检测给定列表对象的实现方式是是quicklist类型---------------->即在本版本中列表实现一种此一种方式
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
		//获取列表对象的元素数量
        return quicklistCount(subject->ptr);
    } else {
     	//编码类型不匹配的错误------------------>此处引起了redis的退出处理
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
/* 根据给定的列表对象和相关参数来初始化列表对象的迭代器 */
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction) {
	//给对应的列表迭代器分配空间
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
	//初始化迭代器的相关参数数据
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. */
    //配置遍历元素的方向
    int iter_direction = direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
	//检测对应的列表对象是否是quicklist类型编码实现
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
		//根据给定的索引位置获取一个对应的迭代器对象
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr, iter_direction, index);
    } else {
        serverPanic("Unknown list encoding");
    }
	//返回对应的列表对象迭代器指向
    return li;
}

/* Clean up the iterator. */
/* 释放对应列表对象迭代器占据的空间 */
void listTypeReleaseIterator(listTypeIterator *li) {
	//首先释放quicklist结构对应的迭代器对象
    zfree(li->iter);
	//然后释放列表对象对应的迭代器对象
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current entry is in fact an entry, 0 otherwise. */
/* 通过迭代器获取列表对象中下一个需要进行遍历的元素数据,其中对应的元素信息会存储到listTypeEntry结构中的entry上 */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
	//检测编码方式是否匹配
    serverAssert(li->subject->encoding == li->encoding);
	
	//给信息节点设置对应的迭代器指向
    entry->li = li;
	//检测对应的编码方式是否是quicklist类型
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
		//获取是否还有下一个元素需要遍历  
        return quicklistNext(li->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
	//返回没有下一个元素的标记
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
/* 根据给定的列表对象中的节点信息实体来获取对应的元素值对象 */
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
	//检测对应的编码方式是否是quicklist类型
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
		//检测是否是字符串类型的数据
        if (entry->entry.value) {
			//创建对应的字符串类型数据对象
            value = createStringObject((char *)entry->entry.value,entry->entry.sz);
        } else {
        	//根据整数数据来创建对应的字符串类型数据对象
            value = createStringObjectFromLongLong(entry->entry.longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
	//返回获取到的字符串类型对象
    return value;
}

/* 在给定的信息节点前后进行插入指定元素的操作处理 */
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
	//检测对应的编码方式是否是quicklist类型
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
		//进行解密对象数据操作处理-------->内部完成了创建新的字符串数据或者增加引用计数的操作处理
        value = getDecodedObject(value);
		//获取需要插入元素的字符串内容指向
        sds str = value->ptr;
		//获取插入字符串的数据长度
        size_t len = sdslen(str);
		//根据插入前后标识来触发不同的插入元素操作处理
        if (where == LIST_TAIL) {
            quicklistInsertAfter((quicklist *)entry->entry.quicklist, &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore((quicklist *)entry->entry.quicklist, &entry->entry, str, len);
        }
		//减少对象的引用计数值-------->此处减少引用计数主要是释放占据的空间--->因为上述的插入操作处理,已经将对应的数据插入到了List列表中了
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
/*  检测当前找到的记录了List列表中节点数据的信息实体的数据是否与给定的对象中记录的数据是否相同 */
int listTypeEqual(listTypeEntry *entry, robj *o) {
	//检测对应的编码方式是否是quicklist类型
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
		//进行数据内容比较操作处理---->返回比较的结果值
        return quicklistCompare(entry->entry.zi,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
/*  根据给定的List列表中元素的节点来进行删除操作处理,同时修改迭代器中的参数用于指向下一个需要遍历的元素 */
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
	//检测对应的编码方式是否是quicklist类型
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
		//进行元素删除操作处理,同时修改迭代器中的相关参数,从而指向下一个需要遍历的元素
        quicklistDelEntry(iter->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Create a quicklist from a single ziplist */
/* 根据给定的ziplist来构建一个对应的quicklist结构类型的对象 */
void listTypeConvert(robj *subject, int enc) {
	//检测原始对象类型是否是列表对象 同时对应的编码方式是否是压缩列表类型
    serverAssertWithInfo(NULL,subject,subject->type==OBJ_LIST);
    serverAssertWithInfo(NULL,subject,subject->encoding==OBJ_ENCODING_ZIPLIST);
	
	//检测需要转换成的编码方式是否是quicklist类型
    if (enc == OBJ_ENCODING_QUICKLIST) {
        size_t zlen = server.list_max_ziplist_size;
        int depth = server.list_compress_depth;
		//设置对象的指向为新创建的quicklist结构
        subject->ptr = quicklistCreateFromZiplist(zlen, depth, subject->ptr);
		//设置对象的新的编码方式为quicklist结构类型
        subject->encoding = OBJ_ENCODING_QUICKLIST;
    } else {
        serverPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 * 列表对象的相关命令实现
 *----------------------------------------------------------------------------*/

/* 通用的处理在对应标记位进行插入元素的处理命令 */
void pushGenericCommand(client *c, int where) {
    int j, pushed = 0;
	//根据给定的键获取对应的值对象
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);
	
	//检测对应的值对象是否是列表对象
    if (lobj && lobj->type != OBJ_LIST) {
		//向客户端端返回键对象对应的值对象不是列表对象------->返回给定的键所对应的值类型错误响应
        addReply(c,shared.wrongtypeerr);
        return;
    }
	
	//循环处理元素的插入操作处理-------->即一次可以向对应的List列表中插入多个元素
    for (j = 2; j < c->argc; j++) {
		//首先检测键所对应的值对象是否存在------>不存在就进行创建对应的值对象
        if (!lobj) {
			//创建对应的值对象
            lobj = createQuicklistObject();
			//配置对应的参数
            quicklistSetOptions(lobj->ptr, server.list_max_ziplist_size, server.list_compress_depth);
			//将对应的键值对插入到redis中进行存储-------->当前只是创建出了对应的结构,尚未对其进行插入元素处理
            dbAdd(c->db,c->argv[1],lobj);
        }
		//真正进行元素数据插入到对应的值对象中
        listTypePush(lobj,c->argv[j],where);
		//增加插入元素的数量
        pushed++;
    }
	//插入元素后,向客户端返回当前列表对象的元素个数
    addReplyLongLong(c, (lobj ? listTypeLength(lobj) : 0));
	//检测是否有对应的元素插入操作处理------>此处会引发对应的数据变化
    if (pushed) {
		//拼接对应的事件类型
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
		//触发通知对应的键所对应的值对象空间数据变化通知
        signalModifiedKey(c->db,c->argv[1]);
		//通知对应的值空间变化通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
	//触发整体redis对应的脏数据计数
    server.dirty += pushed;
}

/*
 * 将一个或多个值插入到列表对象头部。 
 *     如果 key 不存在，一个空列表会被创建并执行 LPUSH 操作。 当 key 存在但不是列表类型时，返回一个错误。 
 * 命令格式
 *     LPUSH key value1 [value2] 
 * 返回值
 *     执行 LPUSH 命令后，列表的长度
 */
void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD);
}

/*
 * 用于将一个或多个值插入到列表对象的尾部(最右边)
 *    如果列表不存在，一个空列表会被创建并执行 RPUSH 操作。 当列表存在但不是列表类型时，返回一个错误。
 * 命令格式
 *     RPUSH key value1 [value2]
 * 返回值
 *     执行 RPUSH 操作后，列表的长度
 */
void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL);
}

/* 通用的处理在已近存在的列表对象中进行元素的插入操作处理 */
void pushxGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *subject;

	//检测键所对应的值对象是否存在,且是否是对应的列表对象
    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL || checkType(c,subject,OBJ_LIST)) 
		return;

	//循环向已经存在的列表对象中插入对应的元素
    for (j = 2; j < c->argc; j++) {
        listTypePush(subject,c->argv[j],where);
        pushed++;
    }
	
	//向客户端返回对应的插入元素后的列表对象中元素的数量
    addReplyLongLong(c,listTypeLength(subject));
	
	//检测是否有对应的元素插入操作处理------>此处会引发对应的数据变化
    if (pushed) {
		//拼接对应的事件类型
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
		//触发通知对应的键所对应的值对象空间数据变化通知
        signalModifiedKey(c->db,c->argv[1]);
		//通知对应的值空间变化通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
	//触发整体redis对应的脏数据计数
    server.dirty += pushed;
}

/*
 * 将一个值插入到已存在的列表头部，列表不存在时操作无效
 * 命令格式
 *     LPUSHX key value1 [value2]
 * 返回值
 *     LPUSHX 命令执行之后，列表的长度
 */
void lpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_HEAD);
}

/*
 * 用于将一个值插入到已存在的列表对象尾部(最右边)。如果列表对象不存在，操作无效。
 * 命令格式
 *     RPUSHX key value1 [value2]
 * 返回值
 *     执行 Rpushx 操作后,列表的长度
 */
void rpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_TAIL);
}

/*
 * 用于在列表对象的元素前或者后插入元素。当指定元素不存在于列表对象中时，不执行任何操作 
 * 命令格式
 *     LINSERT key BEFORE|AFTER pivot value
 * 返回值
 *     如果命令执行成功，返回插入操作完成之后，列表对象的长度。 如果没有找到指定元素 ，返回 -1 。 如果 key 不存在或为空列表对象，返回 0 。
 */
void linsertCommand(client *c) {
    int where;
    robj *subject;
	//定义对应的List列表对象迭代器结构指向
    listTypeIterator *iter;
	//定义用于记录当前遍历List列表对象中所对应的数据节点信息
    listTypeEntry entry;
	//用于标识是否插入元素成功的标识
    int inserted = 0;

	//首先检测插入前后标识参数是否配置正确-------->注意此处不进行区分大小写操作处理
    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        where = LIST_TAIL;
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        where = LIST_HEAD;
    } else {
		//向客户端返回配置插入前后参数错误响应
        addReply(c,shared.syntaxerr);
        return;
    }

	//检测对应的键对象所对应的值对象是否存在于redis中,或者对应的值类型是否是List列表对象类型
    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL || checkType(c,subject,OBJ_LIST)) 
		return;

    /* Seek pivot from head to tail */
	//创建对应的从尾部进行遍历的迭代器对象
    iter = listTypeInitIterator(subject,0,LIST_TAIL);
	//循环检测对应的List列表对象中是否有对应的元素数据
    while (listTypeNext(iter,&entry)) {
		//检测当前遍历的元素的值于给定的值是否相同
        if (listTypeEqual(&entry,c->argv[3])) {
			//进行元素的插入操作处理
            listTypeInsert(&entry,c->argv[4],where);
			//设置插入成功标识
            inserted = 1;
			//跳出循环操作
            break;
        }
    }
	//释放对应的迭代器占据的空间
    listTypeReleaseIterator(iter);
	//检测在对应的元素前后是否插入元素成功
    if (inserted) {
		//触发通知对应的键所对应的值对象空间数据变化通知
        signalModifiedKey(c->db,c->argv[1]);
		//通知对应的值空间变化通知
        notifyKeyspaceEvent(NOTIFY_LIST, "linsert", c->argv[1], c->db->id);
		//redis服务记录脏数据次数
        server.dirty++;
    } else {
        /* Notify client of a failed insert */
		//向客户端返回没有插入成功的响应处理
        addReply(c,shared.cnegone);
        return;
    }
	//插入元素成功,向客户端返回处理后的List列表对象的元素个数
    addReplyLongLong(c,listTypeLength(subject));
}

/* 用于返回列表对象的长度。 
 *     如果列表 key 不存在，则 key 被解释为一个空列表，返回 0 。如果 key 不是列表类型，返回一个错误 
 * 命令格式
 *     LLEN KEY_NAME
 * 返回值
 *     列表的长度
 */
void llenCommand(client *c) {
	//根据给定的键获取对应的列表值对象的指向
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
	//检测对应的列表值对象是否存在,以及值对象是否是对应的列表数据类型
    if (o == NULL || checkType(c,o,OBJ_LIST)) 
		return;
	//向客户端响应一个包含本列表值对象中元素数量的长整型数据
    addReplyLongLong(c,listTypeLength(o));
}

/*
 * 用于通过索引获取列表对象中的元素。
 *     你也可以使用负数下标，以 -1 表示列表的最后一个元素， -2 表示列表的倒数第二个元素，以此类推。
 * 命令格式
 *     LINDEX KEY_NAME INDEX_POSITION
 * 返回值
 *     列表中下标为指定索引值的元素。 如果指定索引值不在列表的区间范围内，返回 nil 。
 */
void lindexCommand(client *c) {
	//根据对应的键对象获取对应的值对象
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk);
	//首先检测对应的键对象所对应的值对象是否存在,同时类型是否是List列表对象类型
    if (o == NULL || checkType(c,o,OBJ_LIST))
		return;
    long index;
    robj *value = NULL;
	
	//首先获取客户端对应参数的索引位置上对象中所记录的整数值----->如果获取对应的整数数据出现问题,直接向客户端响应对应的错误
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;
	
	//检测List列表底层的实现方式是否是quicklist类型
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
		//创建记录信息的实体对象
        quicklistEntry entry;
		//在List列表对象中查询指定索引位置元素,并将信息记录到创建的实体对象上----->找到对应的索引位置元素返回非零 否则返回零值
        if (quicklistIndex(o->ptr, index, &entry)) {
			//处理分析获取的数据是字符串类型还是整数类型
            if (entry.value) {
				//创建对应的字符串对象类型
                value = createStringObject((char*)entry.value,entry.sz);
            } else {
             	//创建对应的整数对象类型
                value = createStringObjectFromLongLong(entry.longval);
            }
			//将找到的对应的元素响应给客户端
            addReplyBulk(c,value);
			//减少创建出来的对象的引用计数----->即有可能进行对象空间释放操作处理
            decrRefCount(value);
        } else {
			//返回没有获取到对应的索引位置元素的响应
            addReply(c,shared.nullbulk);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/*
 * 通过索引来给列表对象设置元素的值。 
 *     当索引参数超出范围，或对一个空列表进行 LSET 时，返回一个错误
 * 命令格式
 *     LSET KEY_NAME INDEX VALUE
 * 返回值
 *     操作成功返回 ok ，否则返回错误信息
 */
void lsetCommand(client *c) {
	//首先根据对应的键对象获取对应的值对象
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
	//检测是否获取值对象,且对应的值对象是否是List列表对象类型
    if (o == NULL || checkType(c,o,OBJ_LIST))
		return;
    long index;
    robj *value = c->argv[3];
	
	//获取对应的设置数据的索引位置
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

	//检测对应的值对象的编码方式是否是quicklist类型
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
		//获取值对象中对应的quicklist结构指向位置
        quicklist *ql = o->ptr;
		//对给定索引位置上的元素进行替换成指定数据元素的操作处理
        int replaced = quicklistReplaceAtIndex(ql, index, value->ptr, sdslen(value->ptr));
	 	//检测是否进行替换操作处理
        if (!replaced) {
			//向客户端返回没有替换数据成功的响应
            addReply(c,shared.outofrangeerr);
        } else {
        	//向客户端返回替换成功的响应
            addReply(c,shared.ok);
			//发送键值对空间变化的信号
            signalModifiedKey(c->db,c->argv[1]);
			//发送执行操作命令的通知
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
			//增加脏数据计数
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/*
 * 统一进行处理数据弹出操作处理命令
 */
void popGenericCommand(client *c, int where) {
	//首先获取对应的键所对应的值对象
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk);
	//检测获取到的值对象是否是List列表对象类型
    if (o == NULL || checkType(c,o,OBJ_LIST))
		return;
	
	//根据弹出位置获取需要弹出的元素对象
    robj *value = listTypePop(o,where);
	//检测是否有对应的元素对象弹出
    if (value == NULL) {
		//向客户端返回没有对应弹出对应的响应
        addReply(c,shared.nullbulk);
    } else {
     	//拼接事件类型
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
		//向客户端返回弹出的元素对象
        addReplyBulk(c,value);
		//减少对数据元素的索引计数
        decrRefCount(value);
		//发送命令事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
		//检测当前的List列表是否还有元素,没有就进行整体List列表的空间释放操作处理
        if (listTypeLength(o) == 0) {
			//发送命令事件通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
			//在redis中彻底删除对应的键值对对象
            dbDelete(c->db,c->argv[1]);
        }
		//通知键值对空间变化信号
        signalModifiedKey(c->db,c->argv[1]);
		//改变脏数据计数
        server.dirty++;
    }
}

/*
 * 用于移除并返回列表对象的第一个元素
 * 命令格式
 *     Lpop KEY_NAME
 * 返回值
 *     列表对象的第一个元素。 当列表对象不存在时，返回 nil
 */
void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

/*
 * 用于移除并返回列表对象的最后一个元素
 * 命令格式
 *     RPOP KEY_NAME
 * 返回值
 *     列表对象的最后一个元素。 当列表对象不存在时，返回 nil
 */
void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

/*
 * 返回列表对象中指定区间内的元素，区间以偏移量 START 和 END 指定。 
 *     其中 0 表示列表对象的第一个元素， 1 表示列表对象的第二个元素，以此类推。 你也可以使用负数下标，以 -1 表示列表对象的最后一个元素， -2 表示列表的倒数第二个元素，以此类推。
 * 命令格式
 *     LRANGE KEY_NAME START END
 * 返回值
 *     一个列表对象，包含指定区间内的元素
 */
void lrangeCommand(client *c) {
    robj *o;
    long start, end, llen, rangelen;
	
	//首先获取客户端传入的获取元素的起始和结束位置值
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) || (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK))
		return;
	
	//获取对应键对象所对应的值对象是否存在,且是否是对应的List列表对象类型
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL || checkType(c,o,OBJ_LIST)) 
		return;
	
	//获取当前List列表对象中元素的数量
    llen = listTypeLength(o);

    /* convert negative indexes */
	//处理负数索引的问题
    if (start < 0) 
		start = llen+start;
    if (end < 0) 
		end = llen+end;
	//处理起始位置还为负值的情况 设置了负数起点位置 且对应的值超过了List列表中元素的个数
    if (start < 0) 
		start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0. The range is empty when start > end or start >= length. */
    //处理在非法的范围内获取数据的判断操作处理
    if (start > end || start >= llen) {
		//给对应的客户端返回一个空集合元素的处理
        addReply(c,shared.emptymultibulk);
        return;
    }
	
	//处理结束位置超过List列表元素的处理
    if (end >= llen) 
		end = llen-1;

	//计算需要返回的数据元素个数
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
	//创建能够返回多个值对象的结构操作
    addReplyMultiBulkLen(c,rangelen);
	//检测对应的列表对象的编码方式是否正常
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
		//创建对应索引位置的迭代器对象
        listTypeIterator *iter = listTypeInitIterator(o, start, LIST_TAIL);
		//循环遍历需要返回的元素个数
        while(rangelen--) {
			//定义存储获取数据元素的结构对象
            listTypeEntry entry;
			//获取对应的下一个元素
            listTypeNext(iter, &entry);
			//获取对应位置上元素的数据信息实体对象
            quicklistEntry *qe = &entry.entry;
			//检测对应的数据是否是字符串类型数据
            if (qe->value) {
				//将对应的字符串类型数据添加到返回集合中
                addReplyBulkCBuffer(c,qe->value,qe->sz);
            } else {
            	//将对应的整数类型数据添加到返回集合中
                addReplyBulkLongLong(c,qe->longval);
            }
        }
		//释放对应的迭代器对象
        listTypeReleaseIterator(iter);
    } else {
        serverPanic("List encoding is not QUICKLIST!");
    }
}

/*
 * 对一个列表对象进行修剪(trim)，就是说，让列表对象只保留指定区间内的元素，不在指定区间之内的元素都将被删除
 *     下标 0 表示列表对象的第一个元素，以 1 表示列表对象的第二个元素，以此类推。 你也可以使用负数下标，以 -1 表示列表对象的最后一个元素， -2 表示列表的倒数第二个元素，以此类推
 * 命令格式
 *     LTRIM KEY_NAME START STOP
 * 返回值
 *     命令执行成功时，返回 ok 
 */
void ltrimCommand(client *c) {
    robj *o;
    long start, end, llen, ltrim, rtrim;
	
	//获取保留范围的起始和结束索引位置
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) || (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) 
        return;
	
	//检测键所对应的值对象是否存在,且是否是List列表类型
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL || checkType(c,o,OBJ_LIST)) 
		return;
	
	//获取当前List列表对象的元素个数
    llen = listTypeLength(o);

    /* convert negative indexes */
	//处理负索引问题
    if (start < 0) 
		start = llen+start;
    if (end < 0) 
		end = llen+end;
	//进一步处理设置负数超过了元素个数的问题
    if (start < 0) 
		start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0. The range is empty when start > end or start >= length. */
	//检测是否出现了范围越界
	if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
		//设置左侧删除的结束位置----->即进行一次全部删除元素处理
        ltrim = llen;
		//设置右侧删除的开始位置,此值也是需要删除的数量
        rtrim = 0;
    } else {
		//检测是否需要修正结束位置索引值
        if (end >= llen) 
			end = llen-1;
		//设置左侧删除的结束位置
        ltrim = start;
		//设置右侧删除的开始位置,此值也是需要删除的数量
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
	//检测值对象的编码类型是否是quicklist类型
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
		//首先进行左侧元素删除操作处理
        quicklistDelRange(o->ptr,0,ltrim);
		//然后进行右侧元素删除操作处理
        quicklistDelRange(o->ptr,-rtrim,rtrim);
    } else {
        serverPanic("Unknown list encoding");
    }
	
	//发送执行对应命令的通知处理
    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
	//检测当前的list列表对象中是否还有元素
    if (listTypeLength(o) == 0) {
		//删除对应的键值对
        dbDelete(c->db,c->argv[1]);
		//发送删除键值对的命令通知操作
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
	//发送键值对空间数据变化的通知
    signalModifiedKey(c->db,c->argv[1]);
	//增加脏计数值
    server.dirty++;
	//返回进行截取对应范围数据成功的响应
    addReply(c,shared.ok);
}

/*
 * 根据参数 COUNT 的值，移除列表中与参数 VALUE 相等的元素
 *     COUNT 的值可以是以下几种： 
 *         count > 0 : 从表头开始向表尾搜索，移除与 VALUE 相等的元素，数量为 COUNT 
 *         count < 0 : 从表尾开始向表头搜索，移除与 VALUE 相等的元素，数量为 COUNT 的绝对值
 *         count = 0 : 移除表中所有与 VALUE 相等的值
 * 命令格式
 *     LREM KEY_NAME COUNT VALUE
 * 返回值
 *     被移除元素的数量。 列表不存在时返回 0 
 */
void lremCommand(client *c) {
    robj *subject, *obj;
    obj = c->argv[3];
    long toremove;
    long removed = 0;
	
	//获取客户端传入的需要删除相同元素的数目值
    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != C_OK))
        return;
	
	//获取键所对应的值对象
    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
	//检测值对象是否存在,且类型是否是List列表类型
    if (subject == NULL || checkType(c,subject,OBJ_LIST))
		return;

    listTypeIterator *li;
	//根据需要删除对应元素数量的正负来确定开始遍历的起始位置
    if (toremove < 0) {
        toremove = -toremove;
		//创建一个从头部开始进行遍历删除的迭代器
        li = listTypeInitIterator(subject,-1,LIST_HEAD);
    } else {
		//创建一个从尾部开始进行遍历删除的迭代器
        li = listTypeInitIterator(subject,0,LIST_TAIL);
    }

    listTypeEntry entry;
	//循环遍历List列表中的元素
    while (listTypeNext(li,&entry)) {
		//检测对应的元素的内容是否与给定的值相同
        if (listTypeEqual(&entry,obj)) {
			//在List列表中删除本元素
            listTypeDelete(li, &entry);
			//增加脏计数值
            server.dirty++;
			//增加删除元素数量
            removed++;
			//检测是否还需要继续进行删除操作处理
            if (toremove && removed == toremove) 
				break;
        }
    }
	//释放对应的迭代器占据的空间
    listTypeReleaseIterator(li);
	
	//检测是否有对应的元素被删除了
    if (removed) {
		//发送键值对空间变化的信号
        signalModifiedKey(c->db,c->argv[1]);
		//发送触发删除List列表元素的通知
        notifyKeyspaceEvent(NOTIFY_LIST,"lrem",c->argv[1],c->db->id);
    }
	
	//检测当前List列表中是否还有元素,没有就直接将对应的键值对对象在redis中进行删除操作处理
    if (listTypeLength(subject) == 0) {
		//删除对应的键值对
        dbDelete(c->db,c->argv[1]);
		//发送通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
	
	//向客户端返回删除对应元素的个数
    addReplyLongLong(c,removed);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */
/* 处理将对应的元素插入到对应的List列表对象中 */
void rpoplpushHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
	//检测在redis中是否存在对应的值对象
    if (!dstobj) {
		//创建对应的List列表值对象
        dstobj = createQuicklistObject();
		//设置对应的参数
        quicklistSetOptions(dstobj->ptr, server.list_max_ziplist_size, server.list_compress_depth);
		//在redis中添加对应的键值对
        dbAdd(c->db,dstkey,dstobj);
    }
	//发送改变对应键空间的信号
    signalModifiedKey(c->db,dstkey);
	//将对应的元素插入到目的List列表中
    listTypePush(dstobj,value,LIST_HEAD);
	//发送执行对应命令的事件通知
    notifyKeyspaceEvent(NOTIFY_LIST,"lpush",dstkey,c->db->id);
    /* Always send the pushed value to the client. */
	//同时向对应的客户端返回获取到的对应的元素
    addReplyBulk(c,value);
}

/*
 * 用于移除列表的最后一个元素，并将该元素添加到另一个列表并返回
 * 命令格式
 *      RPOPLPUSH SOURCE_KEY_NAME DESTINATION_KEY_NAME
 * 返回值
 *     被弹出的元素
 */
void rpoplpushCommand(client *c) {
    robj *sobj, *value;
	//检测源键所对应的值对象是否存在,且对应的类型是否是List列表类型
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL || checkType(c,sobj,OBJ_LIST))
		return;

	//检测获取到的List列表值对象是否元素个数为0
    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent versions of Redis delete keys of empty lists. */
		//向客户端返回没有对应元素的响应
		addReply(c,shared.nullbulk);
    } else {
    	//获取目的键所对应的值对象
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
		//记录对应的源键对象
        robj *touchedkey = c->argv[1];
		
		//检测目的值对象是否存在,且是否是List列表类型
        if (dobj && checkType(c,dobj,OBJ_LIST)) 
			return;
		
		//在源值对象中进行数据弹出操作处理
        value = listTypePop(sobj,LIST_TAIL);
		
        /* We saved touched key, and protect it, since rpoplpushHandlePush may change the client command argument vector (it does not currently). */
		incrRefCount(touchedkey);
		
		//执行将对应的元素插入到目的值对象中
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
		//减少对应的引用计数---->即进行空间的释放操作处理
        decrRefCount(value);

        /* Delete the source list when it is empty */
		//发送执行对应命令的事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,"rpop",touchedkey,c->db->id);
		//检测弹出一个元素后List列表中的元素是否为0
        if (listTypeLength(sobj) == 0) {
			//删除对应的键值对
            dbDelete(c->db,touchedkey);
			//发送执行对应命令的事件通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",touchedkey,c->db->id);
        }
		//发送改变对应键空间的信号
        signalModifiedKey(c->db,touchedkey);
		//减少对应的引用计数
        decrRefCount(touchedkey);
		//增加脏数据计数值
        server.dirty++;
		
		//???????????????????????//
        if (c->cmd->proc == brpoplpushCommand) {
            rewriteClientCommandVector(c,3,shared.rpoplpush,c->argv[1],c->argv[2]);
        }
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is a helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped from the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns C_OK if we are able to serve the client, otherwise
 * C_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. */
int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where) {
    robj *argv[3];

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        argv[0] = (where == LIST_HEAD) ? shared.lpop : shared.rpop;
        argv[1] = key;
        propagate((where == LIST_HEAD) ? server.lpopCommand : server.rpopCommand, db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        addReplyMultiBulkLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);
        
        /* Notify event. */
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
        notifyKeyspaceEvent(NOTIFY_LIST,event,key,receiver->db->id);
    } else {
        /* BRPOPLPUSH */
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            /* Propagate the RPOP operation. */
            argv[0] = shared.rpop;
            argv[1] = key;
            propagate(server.rpopCommand,
                db->id,argv,2,
                PROPAGATE_AOF|
                PROPAGATE_REPL);
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the LPUSH operation. */
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            propagate(server.lpushCommand,
                db->id,argv,3,
                PROPAGATE_AOF|
                PROPAGATE_REPL);

            /* Notify event ("lpush" was notified by rpoplpushHandlePush). */
            notifyKeyspaceEvent(NOTIFY_LIST,"rpop",key,receiver->db->id);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return C_ERR;
        }
    }
    return C_OK;
}

/* Blocking RPOP/LPOP */
void blockingPopGenericCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;

    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != OBJ_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a non normal [LR]POP. */
                    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
                    robj *value = listTypePop(o,where);
                    serverAssert(value != NULL);

                    addReplyMultiBulkLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
                    decrRefCount(value);
                    notifyKeyspaceEvent(NOTIFY_LIST,event,
                                        c->argv[j],c->db->id);
                    if (listTypeLength(o) == 0) {
                        dbDelete(c->db,c->argv[j]);
                        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                            c->argv[j],c->db->id);
                    }
                    signalModifiedKey(c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    rewriteClientCommandVector(c, 2, (where == LIST_HEAD) ? shared.lpop : shared.rpop, c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & CLIENT_MULTI) {
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* If the list is empty or the key does not exists we must block */
    blockForKeys(c,BLOCKED_LIST,c->argv + 1,c->argc - 2,timeout,NULL,NULL);
}

void blpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_HEAD);
}

void brpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_TAIL);
}

void brpoplpushCommand(client *c) {
    mstime_t timeout;

    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;

    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    if (key == NULL) {
        if (c->flags & CLIENT_MULTI) {
            /* Blocking against an empty list in a multi state returns immediately. */
            addReply(c, shared.nullbulk);
        } else {
            /* The list is empty and the client blocks. */
            blockForKeys(c,BLOCKED_LIST,c->argv + 1,1,timeout,c->argv[2],NULL);
        }
    } else {
        if (key->type != OBJ_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so the regular rpoplpushCommand is executed. */
            serverAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}




