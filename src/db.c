/*
 * 直接进行redis数据库键操作的相关命令实现
 */

#include "server.h"
#include "cluster.h"
#include "atomicvar.h"

#include <signal.h>
#include <ctype.h>

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

int keyIsExpired(redisDb *db, robj *key);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
/* 更新对象访问的lfu时间值 */
void updateLFU(robj *val) {
	//获取访问次数值
    unsigned long counter = LFUDecrAndReturn(val);
	//增加对象的访问次数值
    counter = LFULogIncr(counter);
	//重新给对象设置lru值
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Low level key lookup API, not actually called directly from commands
 * implementations that should instead rely on lookupKeyRead(), lookupKeyWrite() and lookupKeyReadWithFlags(). */
/* 该函数被lookupKeyRead()和lookupKeyWrite()和lookupKeyReadWithFlags()调用 */
/* 从redis字典中取出key对象所对应的值对象，如果存在返回值对象，否则返回NULL */
robj *lookupKey(redisDb *db, robj *key, int flags) {
	//在数据库中查找key对象，返回保存该key的节点结构指向
    dictEntry *de = dictFind(db->dict,key->ptr);
	//检测对应的键值对结构是否存在
    if (de) {
		//获取对应的键所对应的值对象
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm. Don't do it if we have a saving child, as this will trigger a copy on write madness. */
		//此处充分考虑到了内存中的模型  --->即主从复制过程中 进程对内存空间数据的共享性
		//此处是一个比较重要的地方,保持值对象不变,那么在进行备份时就会防止内存空间的增加操作处理
		//检测是否处于备份数据过程中,即是否触发改变键值对中值对象中的内存属性值
		if (server.rdb_child_pid == -1 && server.aof_child_pid == -1 && !(flags & LOOKUP_NOTOUCH)) {
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
            	//更新对象的访问时间
                val->lru = LRU_CLOCK();
            }
        }
		//返回对应的值对象
        return val;
    } else {
		//返回对应的空对象
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL if the key is logically expired
 * but still existing, in case this is a slave, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging expiring our key via DELs in the replication link. */
/* 在redis数据库字典中查询对应键所对应的值对象 同时收集命中信息 */
/*
 * 以读操作取出key的值对象，没找到返回NULL
 *	调用该函数的副作用如下：
 *  	1.如果一个键的到达过期时间TTL，该键被设置为过期的
 *		2.键的使用时间信息被更新
 *		3.全局键 hits/misses 状态被更新
 * 注意：如果键在逻辑上已经过期但是仍然存在，函数返回NULL
 */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;
	//检测对应的键对象是否已经过期(有可能触发了删除操作处理 即在主节点上进行了删除处理)
    if (expireIfNeeded(db,key) == 1) {
        /* Key expired. If we are in the context of a master, expireIfNeeded()
         * returns 0 only when the key does not exist at all, so it's safe to return NULL ASAP. */
        //键已过期，如果是主节点环境，表示key已经绝对被删除 因此返回对应的NULL
        if (server.masterhost == NULL) {
			//增加对应的未命中统计值
            server.stat_keyspace_misses++;
			//返回对应的NULL值
            return NULL;
        }

        /* However if we are in the context of a slave, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessign expired values in a read-only fashion, that
         * will say the key as non existing.
         *
         * Notably this covers GETs when slaves are used to scale reads. */
        //如果我们在从节点环境， expireIfNeeded()函数不会删除过期的键，它返回的仅仅是键是否被删除的逻辑值
        //过期的键由主节点负责，目的是为了保证主从节点数据的一致性
        if (server.current_client && server.current_client != server.master &&
            server.current_client->cmd && server.current_client->cmd->flags & CMD_READONLY) {
            //增加对应的未命中统计值
            server.stat_keyspace_misses++;
			//返回对应的NULL值
            return NULL;
        }
    }
	//键没有对应的过期(没有设置过期时间或者过期时间未到)，进一步查询对应的键对象所对应的值对象
    val = lookupKey(db,key,flags);
	//检测值对象是否存在
    if (val == NULL)
		//增加对应的未命中统计值
        server.stat_keyspace_misses++;
    else
		//增加对应的命中统计值
        server.stat_keyspace_hits++;
	//返回对应的值对象
    return val;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the common case. */
/* 以读操作取出key的值对象，会更新是否命中的信息 */
robj *lookupKeyRead(redisDb *db, robj *key) {
	//在下面的函数处理过程中 也触发了expireIfNeeded的处理过程 
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached.
 *
 * Returns the linked value object if the key exists or NULL if the key does not exist in the specified DB. */
/* 以写操作取出key对应的值对象，不更新是否命中的信息 */
robj *lookupKeyWrite(redisDb *db, robj *key) {
	//触发检测是否有必要进行过期键进行删除操作处理
    expireIfNeeded(db,key);
	//进行查询键对象所对应的值对象
    return lookupKey(db,key,LOOKUP_NONE);
}

/* 以读操作取出key的值对象，如果值对象不存在，则发送reply信息，并返回NULL */
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
	//在redis中以只读取的方式查询键对象对应的值对象
    robj *o = lookupKeyRead(c->db, key);
	//检测查询到的值对象是否存在
    if (!o) 
		//直接向对应的客户端返回没有找到对应键对象对应值对象的响应
		addReply(c,reply);
	//返回对应的值对象的指向(这个地方的值对象可以为null,如果为null的话,其实已经给对应的客户端响应结果了)
    return o;
}

/* 以写操作取出key的值对象，如果值对象不存在，则发送reply信息，并返回NULL */
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
	//在redis中以可写操作的方式查询键对象对应的值对象
    robj *o = lookupKeyWrite(c->db, key);
	//检测查询到的值对象是否存在
    if (!o) 
		//直接向对应的客户端返回没有找到对应键对象对应值对象的响应
		addReply(c,reply);
	//返回对应的值对象的指向(这个地方的值对象可以为null,如果为null的话,其实已经给对应的客户端响应结果了)
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference counter of the value if needed. The program is aborted if the key already exists. */
/* 将对应的键值对添加到redis中,该函数的调用者负责增加key-val的引用计数 */
void dbAdd(redisDb *db, robj *key, robj *val) {
	//复制对应的键对象的字符串数据----->注意对于键对象已经进行了对应的深拷贝处理了
    sds copy = sdsdup(key->ptr);
	//将对应的键值对对象放置于redis中-->此处是核心部分--->对于键对象是取自参数对象中的字符串,对于值对象是取自于参数部分的值对象
    int retval = dictAdd(db->dict, copy, val);
	//检测是否插入对应的键值对成功
    serverAssertWithInfo(NULL,key,retval == DICT_OK);
	//特殊检查当前插入的值对象是否是List类型或者Zset类型-------->这个地方可能引发去堵塞操作处理,即堵塞在特定键上的命令客户端
    if (val->type == OBJ_LIST || val->type == OBJ_ZSET)
		//尝试触发解除堵塞键上的命令
		signalKeyAsReady(db, key);
	//检查是否开启了集群模式
    if (server.cluster_enabled) 
		//将对应的键添加到对应的槽位中
		slotToKeyAdd(key);
}

/* Overwrite an existing key with a new value. Incrementing the reference count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 * The program is aborted if the key was not already present. */
/* 在redis中进行复写对应的键值对,该函数的调用者负责增加key-val的引用计数 */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
	//获取键对象对应的键值对结构指向
    dictEntry *de = dictFind(db->dict,key->ptr);
	//检测对应的键值对结构是否存在
    serverAssertWithInfo(NULL,key,de != NULL);
	//获取对应的键值对结构对象 主要是记录老的键值对 方面进行释放值对象的处理
    dictEntry auxentry = *de;
	//获取来的值对象
    robj *old = dictGetVal(de);
	//查询当前redis中配置的内存清楚策略 是否是LFU模式
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
		//记录老对象的lru值
        val->lru = old->lru;
    }
	//设置新的值对象
    dictSetVal(db->dict, de, val);

	//检测服务器是否配置了延迟释放对应的对象
    if (server.lazyfree_lazy_server_del) {
		//将对应的值对象添加到延迟释放的任务中
        freeObjAsync(old);
		//同时清空老的键值对中值对象的引用关系 即后续不进行释放对应值对象的处理了
        dictSetVal(db->dict, &auxentry, NULL);
    }
	//触发释放值对象的处理
    dictFreeVal(db->dict, &auxentry);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent).
 *
 * All the new keys in the database should be created via this interface. */
/* 进行添加或者覆盖键值对的通用接口操作 */
void setKey(redisDb *db, robj *key, robj *val) {
	//首先检测对应的键是否已经存在于redis中
    if (lookupKeyWrite(db,key) == NULL) {
		//进行添加对应的键值对对象操作处理
        dbAdd(db,key,val);
    } else {
    	//进行覆盖重写对应的键值对操作处理
        dbOverwrite(db,key,val);
    }
	//增加对值对象的引用计数-------->这个地方需要明确两个问题
	//1. 为什么不进行增加键的引用计数呢------->可以在dbAdd函数内部发现 对键对象进行了拷贝了内部字符串内容的操作处理
	//2. 为什么要对引用计数进行增加处理------->可以在dbAdd函数内部发现 对值对象是直接进行赋值操作处理,增加引用计数说明在redis键值对中引用了它,后期再网络那块进行引用计数减少时,不会引发释放redis中引用此对象的空间释放操作处理
    incrRefCount(val);
	//主动触发对过期键的删除操作处理-------->键删除策略中的主动行为
    removeExpire(db,key);
	//发送键值对空间变化信号
    signalModifiedKey(db,key);
}

/* 检测对应的键对象是否在redis数据库字典中 返回1表示存在 0表示不存在 */
int dbExists(redisDb *db, robj *key) {
	//在字典结构中查询对应的键对象
    return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object. If there are no keys, NULL is returned.
 * The function makes sure to return keys not already expired. */
/* 在redis中随机返回一个键对象( 随机返回一个键的字符串类型的对象，且保证返回的键没有过期) */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
	//初始化尝试的最大次数
    int maxtries = 100;
	//获取所有键对象是否都过期的标记
    int allvolatile = dictSize(db->dict) == dictSize(db->expires);

	//循环处理,获取可以使用的随机键对象
    while(1) {
        sds key;
        robj *keyobj;
	
		//获取一个随机键对应实体
        de = dictGetRandomKey(db->dict);
		//检测是否有对应的实体对象
        if (de == NULL) 
			return NULL;

		//获取对应的键对象
        key = dictGetKey(de);
		//创建键对象对应的字符串对象----->方便返回此字符串对象 即返回的不是原始的键对象对应的字符串对象 而是新创建的一个字符串对象 因为在网络那块可能引起释放操作处理
        keyobj = createStringObject(key,sdslen(key));
		//检测对应的键对象是否设置了过期时间
		if (dictFind(db->expires,key)) {
			//
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                 * it could happen that all the keys are already logically
                 * expired in the slave, so the function cannot stop because
                 * expireIfNeeded() is false, nor it can stop because
                 * dictGetRandomKey() returns NULL (there are keys to return).
                 * To prevent the infinite loop we do some tries, but if there
                 * are the conditions for an infinite loop, eventually we
                 * return a key name that may be already expired. */
                return keyobj;
            }
			//检测对应的键是否已经处于过期状态
            if (expireIfNeeded(db,keyobj)) {
				//减少对应的引用计数 即释放对应的值对象的空间
                decrRefCount(keyobj);
				//重新触发随机获取键的处理
                continue; /* search for another key. This expired. */
            }
        }
		//返回对应的键字符串对象 
        return keyobj;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
/* 进行同步删除一个对应的键值对,并释放对应的空间 问题是当值对象数据特别多时 可能占据主进程的时间 */
int dbSyncDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of the key, because it is shared with the main dictionary. */
	//检测是否有对应的过期键值对
	if (dictSize(db->expires) > 0) 
		//首先在对应的过期键值对中删除对应的本键值对
		dictDelete(db->expires,key->ptr);
	//检测在redis数据库字典中删除对应的键值对是否成功
    if (dictDelete(db->dict,key->ptr) == DICT_OK) {
		//检测是否开启了集群模式
        if (server.cluster_enabled) 
			//在对应的槽位中删除对应的键
			slotToKeyDel(key);
		//返回删除对应键值对成功标识
        return 1;
    } else {
		//返回删除对应键失败标识----->即没有找到对应的键值对
        return 0;
    }
}

/* This is a wrapper whose behavior depends on the Redis lazy free configuration. Deletes the key synchronously or asynchronously. */
/* 统一进行删除对应的键对象的处理方法 */
int dbDelete(redisDb *db, robj *key) {
	//根据服务器配置的延迟参数来进一步确认使用那种策略进行删除过期的值对象
    return server.lazyfree_lazy_server_del ? dbAsyncDelete(db,key) : dbSyncDelete(db,key);
}

/* Prepare the string object stored at 'key' to be modified destructively to implement commands like SETBIT or APPEND.
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key' in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example using an sdscat() call to append some data, or anything else.
 */
/* 获取给定对象所对应的非共享的对象形式 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
	//检测对应的类型是否是字符串类型
    serverAssert(o->type == OBJ_STRING);
	//检测对应的字符串对象是否被共享过或者编码方式不是RAW格式
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
		// 获取给定字符串类型对象的解码字符串对象
        robj *decoded = getDecodedObject(o);
		//根据解码字符串对象重新创建对应的字符串对象
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
		//减少对原始字符串对象的引用计数
        decrRefCount(decoded);
		//将此键值对对象进行一次重新插入到redis中的处理
        dbOverwrite(db,key,o);
    }
	//返回新创建的字符串对象
    return o;
}

/* Remove all keys from all the databases in a Redis server.
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP.
 *
 * On success the fuction returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
/* 进行删除对应库中所有数据操作的处理 */
long long emptyDb(int dbnum, int flags, void(callback)(void*)) {
	//配置是否异步删除操作标识
    int async = (flags & EMPTYDB_ASYNC);
    long long removed = 0;

	//检测当前给定的库索引是否合法
    if (dbnum < -1 || dbnum >= server.dbnum) {
		//设置错误信息
        errno = EINVAL;
		//返回删除数据失败的错误标识
        return -1;
    }

	//根据参数设定删除库的起始和终止索引位置
    int startdb, enddb;
	//注意特殊值-1 是进行全库删除
    if (dbnum == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbnum;
    }

	//循环删除对应索引库的数据处理
    for (int j = startdb; j <= enddb; j++) {
		//获取当前索引库对应的元素个数
        removed += dictSize(server.db[j].dict);
		//检测是否是异步删除操作处理
        if (async) {
			//启动异步删除操作处理
            emptyDbAsync(&server.db[j]);
        } else {
        	//清空所有的键值对
            dictEmpty(server.db[j].dict,callback);
			//清空所有的过期的键值对
            dictEmpty(server.db[j].expires,callback);
        }
    }
	//检测是否是开启了集群模式------>此处是进行集群模式的处理
    if (server.cluster_enabled) {
        if (async) {
            slotToKeyFlushAsync();
        } else {
            slotToKeyFlush();
        }
    }
    if (dbnum == -1) 
		flushSlaveKeysWithExpireList();
	
	//返回删除键值对的数量
    return removed;
}

/* 根据提供的索引选择操作的对应的库对象 */
int selectDb(client *c, int id) {
	//检测对应的库索引是否合法
    if (id < 0 || id >= server.dbnum)
		//返回切换对应索引库失败的标识
        return C_ERR;
	//获取库索引对应的库
    c->db = &server.db[id];
	//返回切换对应索引库成功的标识
    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 * Every time a key in the database is modified the function signalModifiedKey() is called.
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
}

void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyDb() call for FLUSHALL and FLUSHDB commands.
 * Currently the command just attempts to parse the "ASYNC" option. It also checks if the command arity is wrong.
 * On success C_OK is returned and the flags are stored in *flags, otherwise C_ERR is returned and the function sends an error to the client. */
/* 主要用于检测客户端设置的Flush命令参数是否合法 */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
	//检测参数个数是否大于1
    if (c->argc > 1) {
		//检测参数个数和对应参数位置是否是async的合法性检测
        if (c->argc > 2 || strcasecmp(c->argv[1]->ptr,"async")) {
			//向客户端返回参数配置错误的响应
            addReply(c,shared.syntaxerr);
			//返回解析参数失败的错误标识
            return C_ERR;
        }
		//设置配置异步删除参数标识
        *flags = EMPTYDB_ASYNC;
    } else {
		//设置默认的配置参数标识
        *flags = EMPTYDB_NO_FLAGS;
    }
	//返回解析参数成功标识
    return C_OK;
}

/* FLUSHDB [ASYNC]
 * Flushes the currently SELECTed Redis DB. */
/* 
 * 用于清空当前数据库中的所有 key
 * 命令格式
 *	   FLUSHDB [ASYNC]
 * 返回值
 *	   成功返回 OK 参数配置错误返回参数错误
*/
void flushdbCommand(client *c) {
    int flags;
	
	//用于检测客户端传入参数是否合法
    if (getFlushCommandFlags(c,&flags) == C_ERR) 
		return;
	
	//发送清除当前索引所对应的库数据
    signalFlushedDb(c->db->id);
	//增加对应的脏计数值
    server.dirty += emptyDb(c->db->id,flags,NULL);
	//向客户端返回操作成功标识
    addReply(c,shared.ok);
}

/* FLUSHALL [ASYNC]
 * Flushes the whole server data set. */
/* 
 * 用于清空当前数据库中的所有 key
 * 命令格式
 *     FLUSHDB [ASYNC]
 * 返回值
 *     成功返回 OK 参数配置错误返回参数错误 
 */
void flushallCommand(client *c) {
    int flags;
	
	//用于检测客户端传入参数是否合法
    if (getFlushCommandFlags(c,&flags) == C_ERR) 
		return;
	
	//发送清除所有库对应的数据
    signalFlushedDb(-1);
	//进行真正的删除数据操作处理
    server.dirty += emptyDb(-1,flags,NULL);
	//向客户端返回操作成功响应
    addReply(c,shared.ok);
	//检测当前是否处于rdb备份操作
    if (server.rdb_child_pid != -1) {
		//发送信号取消对应的rdb备份操作处理
        kill(server.rdb_child_pid,SIGUSR1);
		//删除对应的备份临时文件
        rdbRemoveTempFile(server.rdb_child_pid);
    }
	
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        int saved_dirty = server.dirty;
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(server.rdb_filename,rsiptr);
        server.dirty = saved_dirty;
    }
	
	//增加对应的脏计数值
    server.dirty++;
}

/* This command implements DEL and LAZYDEL. */
/* 对于删除指定键值对的通用操作处理函数 */
/* 这个地方有个问题 在主上触发 expireIfNeeded 那么后续 dbSyncDelete 就不会成功了 这样是不是有影响 */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;
	//循环删除给定的多个键值对
    for (j = 1; j < c->argc; j++) {
		//???????????????????//
		//检测是否有必要删除过期的键值对 expireIfNeeded 内部也触发了 dbSyncDelete 或者 dbAsyncDelete
        expireIfNeeded(c->db,c->argv[j]);
		//根据传入的是否懒处理标识来进行删除操作处理
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) : dbSyncDelete(c->db,c->argv[j]);
		//检测是否触发删除成功
        if (deleted) {
			//发送键值对空间变化信号
            signalModifiedKey(c->db,c->argv[j]);
			//发送触发对应删除命令的通知
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[j], c->db->id);
			//增加脏计数值
            server.dirty++;
			//记录删除键值对的数量
            numdel++;
        }
    }
	//向客户端返回删除键值对的数量
    addReplyLongLong(c,numdel);
}

/*
 * 用于删除已存在的键(同步进行删除处理)。不存在的 key 会被忽略
 * 命令格式
 *     DEL KEY_NAME ...
 * 返回值
 *     被删除 key 的数量
 */
void delCommand(client *c) {
    delGenericCommand(c,0);
}

/*
 * 用于删除已存在的键(进行异步懒删除操作处理)。不存在的 key 会被忽略
 * 命令格式
 *     UNLINK KEY_NAME
 * 返回值
 *     被删除 key 的数量
 */
void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N. Return value is the number of keys existing. */
/* 
 * 用于检查给定 key 是否存在
 * 命令格式
 *     EXISTS key1 key2 ... key_N.
 * 返回值
 *     对应存在的键的数量
 */
void existsCommand(client *c) {
	//用于记录存在的键值对的数量
    long long count = 0;
    int j;
	
	//循环处理传入的键参数
    for (j = 1; j < c->argc; j++) {
		//检测对应的键所对应的值对象是否存在
        if (lookupKeyRead(c->db,c->argv[j])) 
			//记录存在的键的数量
			count++;
    }
	//返回存在的键值对的数量
    addReplyLongLong(c,count);
}

/*
 * 用于切换到指定的数据库，数据库索引号 index 用数字值指定，以 0 作为起始索引值
 * 命令格式
 *     SELECT index
 * 返回值
 *     总是返回 OK
 */
void selectCommand(client *c) {
    long id;
	
	//获取选择对应库的索引值
    if (getLongFromObjectOrReply(c, c->argv[1], &id, "invalid DB index") != C_OK)
        return;
	
	//检测是否开启了集群模式---->在集群模式下不能进行选择对应索引库的处理
    if (server.cluster_enabled && id != 0) {
		//向客户端返回当前处于集群模式的错误信息
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
	
	//进行选择对应索引库操作处理
    if (selectDb(c,id) == C_ERR) {
		//向客户端返回索引值越界问题
        addReplyError(c,"DB index is out of range");
    } else {
    	//向客户端返回成功操作标识
        addReply(c,shared.ok);
    }
}

/*
 * 从当前数据库中随机返回一个 key
 * 命令格式
 *     RANDOMKEY 
 * 返回值
 *     当数据库不为空时，返回一个 key 。 当数据库为空时，返回 nil （windows 系统返回 null）
 */
void randomkeyCommand(client *c) {
    robj *key;
	
	//获取对应的随机键
    if ((key = dbRandomKey(c->db)) == NULL) {
		//返回对应的空对象
        addReply(c,shared.nullbulk);
        return;
    }
	
	//向客户端返回对应的键
    addReplyBulk(c,key);
	//减少对应的引用计数------>即释放对应的空间
    decrRefCount(key);
}

void keysCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addDeferredMultiBulkLength(c);

    di = dictGetSafeIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;

        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (!keyIsExpired(c->db,keyobj)) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredMultiBulkLength(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];
    robj *o = pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = dictGetKey(de);
        sds sdsval = dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) 
		listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the client. */
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE) {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value of every element on the Hash. */
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH || o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* Step 1: Parse options. */
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL) != C_OK) {
                goto cleanup;
            }

            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is equivalent to disabling it. */
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /* Handle the case of a hash table. */
    ht = NULL;
    if (o == NULL) {
        ht = c->db->dict;
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. */
    }

    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        privdata[0] = keys;
        privdata[1] = o;
        do {
            cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
        } while (cursor && maxiterations-- && listLength(keys) < (unsigned long)count);
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys, (vstr != NULL) ? createStringObject((char*)vstr,vlen) : createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. */
    node = listFirst(keys);
    while (node) {
        robj *kobj = listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. */
        if (!filter && use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter element if it is an expired key. */
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associted value if needed. */
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
            if (filter) {
                kobj = listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. */
    addReplyMultiBulkLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyMultiBulkLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);
    listRelease(keys);
}

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(client *c) {
    unsigned long cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) 
		return;
    scanGenericCommand(c,NULL,cursor);
}

/*
 * 用于返回当前数据库的 key 的数量
 * 命令格式
 *     DBSIZE
 * 返回值
 *     当前数据库的 key 的数量
 */
void dbsizeCommand(client *c) {
	//向客户端返回当前库中键值对的数量
    addReplyLongLong(c,dictSize(c->db->dict));
}

/*
 * 返回最近一次 Redis 成功将数据保存到磁盘上的时间，以 UNIX 时间戳格式表示
 * 命令格式
 *     LASTSAVE
 * 返回值
 *     字符串，文本行的集合
 */
void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

/*
 * 用于返回 key 所储存的值的类型
 * 命令格式
 *     TYPE KEY_NAME
 * 返回值
 *     返回 key 的数据类型，数据类型有 
 *     none (key不存在) string (字符串) list (列表) set (集合) zset (有序集) hash (哈希表)
 */
void typeCommand(client *c) {
    robj *o;
    char *type;
	
	//获取键所对应的值对象
    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
	//检测对应的值对象是否存在
    if (o == NULL) {
        type = "none";
    } else {
    	//解析值对象的类型
        switch(o->type) {
        case OBJ_STRING: 
			type = "string"; 
		break;
        case OBJ_LIST: 
			type = "list"; 
			break;
        case OBJ_SET: 
			type = "set"; 
			break;
        case OBJ_ZSET: 
			type = "zset"; 
			break;
        case OBJ_HASH: 
			type = "hash"; 
			break;
        case OBJ_STREAM: 
			type = "stream"; 
			break;
        case OBJ_MODULE: {
            moduleValue *mv = o->ptr;
            type = mv->type->name;
        }; break;
        default: 
			type = "unknown"; 
			break;
        }
    }
	//向客户端返回对应值对象的类型
    addReplyStatus(c,type);
}

/*
 * Shutdown 命令执行以下操作
 *     停止所有客户端
 *     如果有至少一个保存点在等待，执行 SAVE 命令
 *     如果 AOF 选项被打开，更新 AOF 文件
 *     关闭 redis 服务器(server)
 * 执行 SHUTDOWN SAVE 会强制让数据库执行保存操作，即使没有设定(configure)保存点
 * 执行 SHUTDOWN NOSAVE 会阻止数据库执行保存操作，即使已经设定有一个或多个保存点(你可以将这一用法看作是强制停止服务器的一个假想的 ABORT 命令)
 * 命令格式
 *     SHUTDOWN [NOSAVE] [SAVE]
 * 返回值
 *     执行失败时返回错误。 执行成功时不返回任何信息，服务器和客户端的连接断开，客户端自动退出。 
 */
void shutdownCommand(client *c) {
    int flags = 0;
	
	//检测对应的参数个数是否合法
    if (c->argc > 2) {
		//向客户端返回参数配置错误
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
			//配置不进行保存操作处理
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
        	//配置进行保存操作处理
            flags |= SHUTDOWN_SAVE;
        } else {
			//配置属性信息不正确的响应
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB with half-read data).
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    //如果服务器正在载入数据集或者是正在处于集群模式
    if (server.loading || server.sentinel_mode)
		//清除SHUTDOWN_SAVE标志，强制设置为SHUTDOWN_NOSAVE
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

	//准备停机，处理停机前的操作，例如杀死子进程，刷新缓冲区，关闭socket等
    if (prepareForShutdown(flags) == C_OK) 
		//触发关闭对应服务的处理
		exit(0);
	//向客户端响应进行停止服务失败的提示信息
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

/* 通用的进行键值对的键改名操作处理 */
void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed, if the key exists, however we still return an error on unexisting key. */
	//检测给定的两个键的名称是否相同
	if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) 
		//设置新老键名相同的标识
		samekey = 1;

	//获取对应的老键所对应的对象是否存在
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

	//检测进行改名改成相同的特殊处理
    if (samekey) {
		//根据标识返回对应的响应结果
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

	//提前进行值对象引用计数的增加处理  ---->因为后面 牵扯到 添加键值对 和移除 老的键值对
    incrRefCount(o);
	//获取对应的值对象的过期时间
    expire = getExpire(c->db,c->argv[1]);
	//检测改名后的键值对是否存在
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
		//检测是否是不存在才进行改名操作处理的标识
        if (nx) {
			//减少对应的引用计数
            decrRefCount(o);
			//向客户端返回对应的标识
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one with the same name. */
		//删除改名后对应的键值对
        dbDelete(c->db,c->argv[2]);
    }
	//将新的键值对添加到redis数据库字典中
    dbAdd(c->db,c->argv[2],o);
	//检测是否还有剩余的过期时间值
    if (expire != -1) 
		//给修改后的键值对设置剩余的过期时间
		setExpire(c,c->db,c->argv[2],expire);

	//删除对应的老键值对
    dbDelete(c->db,c->argv[1]);
	//发送对应键值对空间变化的信号
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
	//发送触发相关命令的通知
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from", c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to", c->argv[2],c->db->id);
	//增加脏计数值
    server.dirty++;
	//向客户端返回对应的响应标识
    addReply(c,nx ? shared.cone : shared.ok);
}

/*
 * 用于修改 key 的名称 不管修改后的键对象是否存在
 * 命令格式
 *     RENAME OLD_KEY_NAME NEW_KEY_NAME
 * 返回值
 *     改名成功时提示 OK ，失败时候返回一个错误
 *     当 OLD_KEY_NAME 和 NEW_KEY_NAME 相同，或者 OLD_KEY_NAME 不存在时，返回一个错误。 当 NEW_KEY_NAME 已经存在时， RENAME 命令将覆盖旧值。
 */
void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

/*
 * 用于在新的 key 不存在时修改 key 的名称
 * 命令格式
 *     RENAMENX OLD_KEY_NAME NEW_KEY_NAME
 * 返回值
 *     修改成功时，返回 1 。 如果 NEW_KEY_NAME 已经存在，返回 0 
 */
void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

/*
 * 用于将当前数据库的 key 移动到给定的数据库 db 当中
 * 命令格式
 *     MOVE KEY_NAME DESTINATION_DATABASE
 * 返回值
 *     移动成功返回 1 ，失败则返回 0
 */
void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    long long dbid, expire;

	//检测当前是否开启集群模式
    if (server.cluster_enabled) {
		//向客户端返回集群模式下不能进行移动元素的处理
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
	//获得源数据库和源数据库的id
    src = c->db;
    srcid = c->db->id;

	//获取对应的目的库索引值 索引值合法 进行切换到对应的索引库
    if (getLongLongFromObject(c->argv[2],&dbid) == C_ERR || dbid < INT_MIN || dbid > INT_MAX || selectDb(c,dbid) == C_ERR) {
		//向客户端返回目标库索引越界标识
		addReply(c,shared.outofrangeerr);
        return;
    }
	//获取目标库
    dst = c->db;
	//切换回源数据库
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same DB as the source DB it is probably an error. */
	//检测前后切换的数据库相同
    if (src == dst) {
		//返回相同库不能进行移动操作处理
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
	//以写操作方式取出源数据库的值对象
    o = lookupKeyWrite(c->db,c->argv[1]);
	//检测对应的值对象是否存在
    if (!o) {
		//向客户端返回移动键值对失败标识
        addReply(c,shared.czero);
        return;
    }
	//获取配置的过期时间
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB */
	//判断当前key是否存在于目标数据库，存在直接返回不能移动键值对的处理
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
		//向客户端返回移动键值对失败标识
        addReply(c,shared.czero);
        return;
    }
	//将key-value对象添加到目标数据库中
    dbAdd(dst,c->argv[1],o);
	//检测是否需要设置过期时间
    if (expire != -1) 
		//设置移动后key的过期时间
		setExpire(c,dst,c->argv[1],expire);
	//增加引用计数值 因为后面有对应的删除键值对的处理
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
	//从源数据库中将key和关联的值对象删除
    dbDelete(src,c->argv[1]);
	//更新脏计数值
    server.dirty++;
	//向客户端返回移动键值对成功标识
    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment where the function is used for more info. */
/* 检测堵塞字典中的对象是否有准备好的键值对存在 */
void scanDatabaseForReadyLists(redisDb *db) {
    dictEntry *de;
	//获取对应的堵塞字典结构的安全迭代器
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
	//循环检测对应的处于堵塞中的键对象元素是否可以解除堵塞
    while((de = dictNext(di)) != NULL) {
		//获取处于堵塞的键对象
        robj *key = dictGetKey(de);
		//在数据字典中尝试获取对应的值对象
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
		//检测对应的值对象是否存在且类型是否是规定的类型
        if (value && (value->type == OBJ_LIST || value->type == OBJ_STREAM || value->type == OBJ_ZSET))
			//发送对应的键对象已经准备成功的处理
            signalKeyAsReady(db, key);
    }
	//释放对应的安全迭代器空间
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise C_OK is returned. */
/* 进行切换两个索引对应的数据库数据 */
int dbSwapDatabases(int id1, int id2) {
	//检测对应的索引值是否合法
    if (id1 < 0 || id1 >= server.dbnum || id2 < 0 || id2 >= server.dbnum) 
        return C_ERR;
	//检测两个索引值是否相同
    if (id1 == id2) 
		return C_OK;
	//中间过渡值
    redisDb aux = server.db[id1];
	//获取对应库索引的库数据
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swap hash tables. Note that we don't swap blocking_keys, ready_keys and watched_keys, since we want clients to remain in the same DB they were. */
    //将第二个索引的数据迁移到第一个索引上 注意这个地方只是迁移了 数据字典数据 和 过期字典数据
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;

	//将第一个索引的数据迁移到第二个索引上
    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready if needed. */
    //在第一个库上触发监听的List堵塞是否可以开启
    scanDatabaseForReadyLists(db1);
	//在第二个库上触发监听的List堵塞是否可以开启
    scanDatabaseForReadyLists(db2);
	//返回切换数据库操作成功标识
    return C_OK;
}

/* SWAPDB db1 db2 */
/*
 * 将给定的两个索引值对应的库数据进行交换操作处理
 * 命令格式
 *     SWAPDB db1 db2
 * 返回值
 *     交换成功返回成功标识
 */
void swapdbCommand(client *c) {
    long id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
	//检测是否开启了集群模式
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. */
	//获取第一个库的索引值
    if (getLongFromObjectOrReply(c, c->argv[1], &id1, "invalid first DB index") != C_OK)
        return;
	
	//获取第二个库的索引值
    if (getLongFromObjectOrReply(c, c->argv[2], &id2, "invalid second DB index") != C_OK)
        return;

    /* Swap... */
	//尝试进行切换操作处理
    if (dbSwapDatabases(id1,id2) == C_ERR) {
		//尝试切换失败 向客户端发送失败原因
        addReplyError(c,"DB index is out of range");
        return;
    } else {
		//增加对应的脏计数值
        server.dirty++;
		//向客户端回复操作成功标识
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 * 下面是键过期相关的操作API
 *----------------------------------------------------------------------------*/

/* 将含有过期时间的键对象进行移除处理，成功返回1 */
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the main dict. Otherwise, the key will never be freed. */
	//首先确认配置的键对象在redis的字典结构中
	serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
	//将带有过期时间的键在过期字典中进行删除处理
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/* Set an expire to the specified key. If the expire is set in the context of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds after which the key will no longer be considered valid. */
/* 设置键值对的过期时间 */
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
	//在对应的键值对字典结构中获取对应键所对应的节点
    kde = dictFind(db->dict,key->ptr);
	//检测对应的节点是否存在
    serverAssertWithInfo(NULL,key,kde != NULL);
	//在对应的过期字典结构中找到或者添加对应的键所对应的结构  ---->即有可能以前设置过过期时间
    de = dictAddOrFind(db->expires,dictGetKey(kde));
	//给对应的键对象设置过期时间值
    dictSetSignedIntegerVal(de,when);


    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire is associated with this key (i.e. the key is non volatile) */
/* 获取给定键对象设置的过期时间 */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
	//检测对应的过期键是否存在元素,同时获取对应的过期键对应的实体对象
    if (dictSize(db->expires) == 0 || (de = dictFind(db->expires,key->ptr)) == NULL) 
		//返回没有设置对应的过期时间值的标识
		return -1;

    /* The entry was found in the expire dict, this means it should also be present in the main dict (safety check). */
	//进一步进行安全检测 即在对应的数据存储的内存数据库中也需要存在对应的键值对对象
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
	//获取设置的过期时间值--------->需要注意,在过期字典中存储的键值对的内容是 键对象 和 值对象(对应的过期时间)
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring keys. */
/* 将键对象过期事件传播到从节点和AOF文件中 当一个键在主节点中过期时，主节点会发送del命令给从节点和AOF文件 */
void propagateExpire(redisDb *db, robj *key, int lazy) {
	//构造一个参数列表
    robj *argv[2];
	//配置对应的命令
    argv[0] = lazy ? shared.unlink : shared.del;
	//配置对应的键
    argv[1] = key;
	
	//增加对应的引用计数
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

	//如果AOF状态为开启或可写的状态
    if (server.aof_state != AOF_OFF)
		//将del命令追加到AOF文件中
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
	//将配置的删除过期键的命令发送给所有的从节点
    replicationFeedSlaves(server.slaves,db->id,argv,2);

	//减少对应的引用计数 进而触发释放对象空间处理
    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. */
/* 获取给定的键对象对应的剩余过期时间值 */
int keyIsExpired(redisDb *db, robj *key) {
	//获取对应键对象的过期时间
    mstime_t when = getExpire(db,key);
    mstime_t now;

	//检测对应的键是否设置了过期时间 -1 标识是没有设置过期键操作
    if (when < 0) 
		//没有设置过期时间值 返回没有过期的标识
		return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
	//检测当前服务器是否处于正在加载中的状态
    if (server.loading) 
		//返回没有过期的标识 
		return 0;

    //配置对应的时间值
    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent. See issue #1525 on Github for more information. */
    if (server.lua_caller) {
        now = server.lua_time_start;
    }
    /* If we are in the middle of a command execution, we still want to use
     * a reference time that does not change: in that case we just use the
     * cached time, that we update before each call in the call() function.
     * This way we avoid that commands such as RPOPLPUSH or similar, that
     * may re-open the same key multiple times, can invalidate an already
     * open object in a next call, if the next call will see the key expired,
     * while the first did not. */
    else if (server.fixed_time_expire > 0) {
        now = server.mstime;
    }
    /* For the other cases, we want to use the most fresh time we have. */
    else {
        now = mstime();
    }

    /* The key expired if the current (virtual or real) time is greater than the expire time of the key. */
	//计算键是否处于过期
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
/* 检测是否有必要删除过期的键值对,没有过期返回对应的0标识 */
int expireIfNeeded(redisDb *db, robj *key) {
	//检测对应的键对象是否已经过期
    if (!keyIsExpired(db,key)) 
		//没有过期直接返回0标识
		return 0;

    /* If we are running in the context of a slave, instead of
     * evicting the expired key from the database, we return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if we think the key is expired at this time. */
    //检测当前是否是从服务器 即从服务器不做对应的删除操作处理
    //如果服务器正在进行主从节点的复制，从节点的过期键应该被 主节点发送同步删除的操作进行删除，而自己不主动删除
    if (server.masterhost != NULL) 
		//从节点只是返回过期的标识,但是不会触发后续的删除过期键的处理
		return 1;

    /* Delete the key */
	//增加对应过期键的统计信息
    server.stat_expiredkeys++;
	//将过期键key传播给AOF文件和从节点
    propagateExpire(db,key,server.lazyfree_lazy_expire);
	//发送对应的命令通知
    notifyKeyspaceEvent(NOTIFY_EXPIRED,"expired",key,db->id);
	//根据服务器配置来确定是同步删除过期键还是异步删除
    return server.lazyfree_lazy_expire ? dbAsyncDelete(db,key) : dbSyncDelete(db,key);
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* The base case is to use the keys position as given in the command table (firstkey, lastkey, step). */
int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }

    last = cmd->lastkey;
    if (last < 0) last = argc+last;
    keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        if (j >= argc) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                zfree(keys);
                *numkeys = 0;
                return NULL;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is an heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,numkeys);
    } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,numkeys);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

/* Free the result of getKeysFromCommand. */
void getKeysFreeResult(int *result) {
    zfree(result);
}

/* Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> */
int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num < 1 || num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect */
    keys = zmalloc(sizeof(int)*(num+1));

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). */
    keys[num] = 1;
    *numkeys = num+1;  /* Total keys = {union,inter} keys + storage key */
    return keys;
}

/* Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] */
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num <= 0 || num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    keys = zmalloc(sizeof(int)*num);
    *numkeys = num;

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return keys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);

    num = 0;
    keys = zmalloc(sizeof(int)*2); /* Alloc 2 places for the worst case. */

    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    *numkeys = num + found_store;
    return keys;
}

int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, first, *keys;
    UNUSED(cmd);

    /* Assume the obvious form. */
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. */
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;
                num = argc-first;
                break;
            }
        }
    }

    keys = zmalloc(sizeof(int)*num);
    for (i = 0; i < num; i++) keys[i] = first+i;
    *numkeys = num;
    return keys;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC] [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ... */
int *georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = zmalloc(sizeof(int) * num);

    /* Add all key positions to keys[] */
    keys[0] = 1;
    if(num > 1) {
         keys[1] = stored_key;
    }
    *numkeys = num;
    return keys;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N */
int *xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num = 0, *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the name of the stream key. */
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        *numkeys = 0;
        return NULL;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key. */

    keys = zmalloc(sizeof(int) * num);
    for (i = streams_pos+1; i < argc-num; i++) keys[i-streams_pos-1] = i;
    *numkeys = num;
    return keys;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot. */
void slotToKeyUpdateKey(robj *key, int add) {
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));
    unsigned char buf[64];
    unsigned char *indexed = buf;
    size_t keylen = sdslen(key->ptr);

    server.cluster->slots_keys_count[hashslot] += add ? 1 : -1;
    if (keylen+2 > 64) indexed = zmalloc(keylen+2);
    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    memcpy(indexed+2,key->ptr,keylen);
    if (add) {
        raxInsert(server.cluster->slots_to_keys,indexed,keylen+2,NULL,NULL);
    } else {
        raxRemove(server.cluster->slots_to_keys,indexed,keylen+2,NULL);
    }
    if (indexed != buf) zfree(indexed);
}

void slotToKeyAdd(robj *key) {
    slotToKeyUpdateKey(key,1);
}

void slotToKeyDel(robj *key) {
    slotToKeyUpdateKey(key,0);
}

void slotToKeyFlush(void) {
    raxFree(server.cluster->slots_to_keys);
    server.cluster->slots_to_keys = raxNew();
    memset(server.cluster->slots_keys_count,0,sizeof(server.cluster->slots_keys_count));
}

/* Pupulate the specified array of objects with keys in the specified slot.
 * New objects are returned to represent keys, it's up to the caller to
 * decrement the reference count to release the keys names. */
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    raxSeek(&iter,">=",indexed,2);
    while(count-- && raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        keys[j++] = createStringObject((char*)iter.key+2,iter.key_len-2);
    }
    raxStop(&iter);
    return j;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
unsigned int delKeysInSlot(unsigned int hashslot) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    while(server.cluster->slots_keys_count[hashslot]) {
        raxSeek(&iter,">=",indexed,2);
        raxNext(&iter);

        robj *key = createStringObject((char*)iter.key+2,iter.key_len-2);
        dbDelete(&server.db[0],key);
        decrRefCount(key);
        j++;
    }
    raxStop(&iter);
    return j;
}

unsigned int countKeysInSlot(unsigned int hashslot) {
    return server.cluster->slots_keys_count[hashslot];
}







