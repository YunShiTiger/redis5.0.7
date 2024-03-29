/* blocked.c - generic support for blocking operations like BLPOP & WAIT.
 * API:
 *
 * getTimeoutFromObjectOrReply() is just an utility function to parse a
 * timeout argument since blocking operations usually require a timeout.
 *
 * blockClient() set the CLIENT_BLOCKED flag in the client, and set the
 * specified block type 'btype' filed to one of BLOCKED_* macros.
 *
 * unblockClient() unblocks the client doing the following:
 * 1) It calls the btype-specific function to cleanup the state.
 * 2) It unblocks the client by unsetting the CLIENT_BLOCKED flag.
 * 3) It puts the client into a list of just unblocked clients that are
 *    processed ASAP in the beforeSleep() event loop callback, so that
 *    if there is some query buffer to process, we do it. This is also
 *    required because otherwise there is no 'readable' event fired, we
 *    already read the pending commands. We also set the CLIENT_UNBLOCKED
 *    flag to remember the client is in the unblocked_clients list.
 *
 * processUnblockedClients() is called inside the beforeSleep() function
 * to process the query buffer from unblocked clients and remove the clients
 * from the blocked_clients queue.
 *
 * replyToBlockedClientTimedOut() is called by the cron function when
 * a client blocked reaches the specified timeout (if the timeout is set
 * to 0, no timeout is processed).
 * It usually just needs to send a reply to the client.
 *
 * When implementing a new type of blocking opeation, the implementation
 * should modify unblockClient() and replyToBlockedClientTimedOut() in order
 * to handle the btype-specific behavior of this two functions.
 * If the blocking operation waits for certain keys to change state, the
 * clusterRedirectBlockedClientIfNeeded() function should also be updated.
 */

#include "server.h"

int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where);

/* Get a timeout value from an object and store it into 'timeout'.
 * The final timeout is always stored as milliseconds as a time where the
 * timeout will expire, however the parsing is performed according to
 * the 'unit' that can be seconds or milliseconds.
 *
 * Note that if the timeout is zero (usually from the point of view of
 * commands API this means no timeout) the value stored into 'timeout'
 * is zero. */
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit) {
    long long tval;

    if (getLongLongFromObjectOrReply(c,object,&tval,
        "timeout is not an integer or out of range") != C_OK)
        return C_ERR;

    if (tval < 0) {
        addReplyError(c,"timeout is negative");
        return C_ERR;
    }

    if (tval > 0) {
        if (unit == UNIT_SECONDS) tval *= 1000;
        tval += mstime();
    }
    *timeout = tval;

    return C_OK;
}

/* Block a client for the specific operation type. Once the CLIENT_BLOCKED flag is set client query buffer is not longer processed, but accumulated, and will be processed when the client is unblocked. */
void blockClient(client *c, int btype) {
	//设置客户端处于堵塞状态
    c->flags |= CLIENT_BLOCKED;
	//设置客户端堵塞的类型
    c->btype = btype;
	//增加服务器的堵塞客户端的数量值
    server.blocked_clients++;
	//增加客户端在某种类型上堵塞的数量值
    server.blocked_clients_by_type[btype]++;
}

/* This function is called in the beforeSleep() function of the event loop
 * in order to process the pending input buffer of clients that were
 * unblocked after a blocking operation. */
void processUnblockedClients(void) {
    listNode *ln;
    client *c;

    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        serverAssert(ln != NULL);
        c = ln->value;
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~CLIENT_UNBLOCKED;

        /* Process remaining data in the input buffer, unless the client
         * is blocked again. Actually processInputBuffer() checks that the
         * client is not blocked before to proceed, but things may change and
         * the code is conceptually more correct this way. */
        if (!(c->flags & CLIENT_BLOCKED)) {
            if (c->querybuf && sdslen(c->querybuf) > 0) {
                processInputBufferAndReplicate(c);
            }
        }
    }
}

/* This function will schedule the client for reprocessing at a safe time.
 *
 * This is useful when a client was blocked for some reason (blocking opeation,
 * CLIENT PAUSE, or whatever), because it may end with some accumulated query
 * buffer that needs to be processed ASAP:
 *
 * 1. When a client is blocked, its readable handler is still active.
 * 2. However in this case it only gets data into the query buffer, but the
 *    query is not parsed or executed once there is enough to proceed as
 *    usually (because the client is blocked... so we can't execute commands).
 * 3. When the client is unblocked, without this function, the client would
 *    have to write some query in order for the readable handler to finally
 *    call processQueryBuffer*() on it.
 * 4. With this function instead we can put the client in a queue that will
 *    process it for queries ready to be executed at a safe time.
 */
void queueClientForReprocessing(client *c) {
    /* The client may already be into the unblocked list because of a previous
     * blocking operation, don't add back it into the list multiple times. */
    if (!(c->flags & CLIENT_UNBLOCKED)) {
        c->flags |= CLIENT_UNBLOCKED;
        listAddNodeTail(server.unblocked_clients,c);
    }
}

/* Unblock a client calling the right function depending on the kind
 * of operation the client is blocking for. */
void unblockClient(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        unblockClientWaitingData(c);
    } else if (c->btype == BLOCKED_WAIT) {
        unblockClientWaitingReplicas(c);
    } else if (c->btype == BLOCKED_MODULE) {
        unblockClientFromModule(c);
    } else {
        serverPanic("Unknown btype in unblockClient().");
    }
    /* Clear the flags, and put the client in the unblocked list so that
     * we'll process new commands in its query buffer ASAP. */
    server.blocked_clients--;
    server.blocked_clients_by_type[c->btype]--;
    c->flags &= ~CLIENT_BLOCKED;
    c->btype = BLOCKED_NONE;
    queueClientForReprocessing(c);
}

/* This function gets called when a blocked client timed out in order to
 * send it a reply of some kind. After this function is called,
 * unblockClient() will be called with the same client as argument. */
void replyToBlockedClientTimedOut(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        addReply(c,shared.nullmultibulk);
    } else if (c->btype == BLOCKED_WAIT) {
        addReplyLongLong(c,replicationCountAcksByOffset(c->bpop.reploffset));
    } else if (c->btype == BLOCKED_MODULE) {
        moduleBlockedClientTimedOut(c);
    } else {
        serverPanic("Unknown btype in replyToBlockedClientTimedOut().");
    }
}

/* Mass-unblock clients because something changed in the instance that makes
 * blocking no longer safe. For example clients blocked in list operations
 * in an instance which turns from master to slave is unsafe, so this function
 * is called when a master turns into a slave.
 *
 * The semantics is to send an -UNBLOCKED error to the client, disconnecting
 * it at the same time. */
void disconnectAllBlockedClients(void) {
    listNode *ln;
    listIter li;

    listRewind(server.clients,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_BLOCKED) {
            addReplySds(c,sdsnew(
                "-UNBLOCKED force unblock from blocking operation, "
                "instance state changed (master -> replica?)\r\n"));
            unblockClient(c);
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        }
    }
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client. It handles serving clients blocked in
 * lists, streams, and sorted sets, via a blocking commands.
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some write operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BRPOPLPUSH we can have new blocking clients
 * to serve because of the PUSH side of BRPOPLPUSH.
 *
 * This function is normally "fair", that is, it will server clients
 * using a FIFO behavior. However this fairness is violated in certain
 * edge cases, that is, when we have clients blocked at the same time
 * in a sorted set and in a list, for the same key (a very odd thing to
 * do client side, indeed!). Because mismatching clients (blocking for
 * a different type compared to the current key type) are moved in the
 * other side of the linked list. However as long as the key starts to
 * be used only for a single type, like virtually any Redis application will
 * do, the function is already fair. */
void handleClientsBlockedOnKeys(void) {
    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalKeyAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BRPOPLPUSH. */
        l = server.ready_keys;
        server.ready_keys = listCreate();

        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalKeyAsReady() against this key. */
            dictDelete(rl->db->ready_keys,rl->key);

            /* Even if we are not inside call(), increment the call depth
             * in order to make sure that keys are expired against a fixed
             * reference time, and not against the wallclock time. This
             * way we can lookup an object multiple times (BRPOPLPUSH does
             * that) without the risk of it being freed in the second
             * lookup, invalidating the first one.
             * See https://github.com/antirez/redis/pull/6554. */
            server.fixed_time_expire++;
            updateCachedTime(0);

            /* Serve clients blocked on list key. */
            robj *o = lookupKeyWrite(rl->db,rl->key);
            if (o != NULL && o->type == OBJ_LIST) {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);
                    int numclients = listLength(clients);

                    while(numclients--) {
                        listNode *clientnode = listFirst(clients);
                        client *receiver = clientnode->value;

                        if (receiver->btype != BLOCKED_LIST) {
                            /* Put at the tail, so that at the next call
                             * we'll not run into it again. */
                            listDelNode(clients,clientnode);
                            listAddNodeTail(clients,receiver);
                            continue;
                        }

                        robj *dstkey = receiver->bpop.target;
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == blpopCommand) ?
                                     LIST_HEAD : LIST_TAIL;
                        robj *value = listTypePop(o,where);

                        if (value) {
                            /* Protect receiver->bpop.target, that will be
                             * freed by the next unblockClient()
                             * call. */
                            if (dstkey) incrRefCount(dstkey);
                            unblockClient(receiver);

                            if (serveClientBlockedOnList(receiver,
                                rl->key,dstkey,rl->db,value,
                                where) == C_ERR)
                            {
                                /* If we failed serving the client we need
                                 * to also undo the POP operation. */
                                listTypePush(o,value,where);
                            }

                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        } else {
                            break;
                        }
                    }
                }

                if (listTypeLength(o) == 0) {
                    dbDelete(rl->db,rl->key);
                    notifyKeyspaceEvent(NOTIFY_GENERIC,"del",rl->key,rl->db->id);
                }
                /* We don't call signalModifiedKey() as it was already called
                 * when an element was pushed on the list. */
            }

            /* Serve clients blocked on sorted set key. */
            else if (o != NULL && o->type == OBJ_ZSET) {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);
                    int numclients = listLength(clients);
                    unsigned long zcard = zsetLength(o);

                    while(numclients-- && zcard) {
                        listNode *clientnode = listFirst(clients);
                        client *receiver = clientnode->value;

                        if (receiver->btype != BLOCKED_ZSET) {
                            /* Put at the tail, so that at the next call
                             * we'll not run into it again. */
                            listDelNode(clients,clientnode);
                            listAddNodeTail(clients,receiver);
                            continue;
                        }

                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == bzpopminCommand)
                                     ? ZSET_MIN : ZSET_MAX;
                        unblockClient(receiver);
                        genericZpopCommand(receiver,&rl->key,1,where,1,NULL);
                        zcard--;

                        /* Replicate the command. */
                        robj *argv[2];
                        struct redisCommand *cmd = where == ZSET_MIN ?
                                                   server.zpopminCommand :
                                                   server.zpopmaxCommand;
                        argv[0] = createStringObject(cmd->name,strlen(cmd->name));
                        argv[1] = rl->key;
                        incrRefCount(rl->key);
                        propagate(cmd,receiver->db->id,
                                  argv,2,PROPAGATE_AOF|PROPAGATE_REPL);
                        decrRefCount(argv[0]);
                        decrRefCount(argv[1]);
                    }
                }
            }

            /* Serve clients blocked on stream key. */
            else if (o != NULL && o->type == OBJ_STREAM) {
                dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
                stream *s = o->ptr;

                /* We need to provide the new data arrived on the stream
                 * to all the clients that are waiting for an offset smaller
                 * than the current top item. */
                if (de) {
                    list *clients = dictGetVal(de);
                    listNode *ln;
                    listIter li;
                    listRewind(clients,&li);

                    while((ln = listNext(&li))) {
                        client *receiver = listNodeValue(ln);
                        if (receiver->btype != BLOCKED_STREAM) continue;
                        streamID *gt = dictFetchValue(receiver->bpop.keys,
                                                      rl->key);

                        /* If we blocked in the context of a consumer
                         * group, we need to resolve the group and update the
                         * last ID the client is blocked for: this is needed
                         * because serving other clients in the same consumer
                         * group will alter the "last ID" of the consumer
                         * group, and clients blocked in a consumer group are
                         * always blocked for the ">" ID: we need to deliver
                         * only new messages and avoid unblocking the client
                         * otherwise. */
                        streamCG *group = NULL;
                        if (receiver->bpop.xread_group) {
                            group = streamLookupCG(s,
                                    receiver->bpop.xread_group->ptr);
                            /* If the group was not found, send an error
                             * to the consumer. */
                            if (!group) {
                                addReplyError(receiver,
                                    "-NOGROUP the consumer group this client "
                                    "was blocked on no longer exists");
                                unblockClient(receiver);
                                continue;
                            } else {
                                *gt = group->last_id;
                            }
                        }

                        if (streamCompareID(&s->last_id, gt) > 0) {
                            streamID start = *gt;
                            start.seq++; /* Can't overflow, it's an uint64_t */

                            /* Lookup the consumer for the group, if any. */
                            streamConsumer *consumer = NULL;
                            int noack = 0;

                            if (group) {
                                consumer = streamLookupConsumer(group,
                                           receiver->bpop.xread_consumer->ptr,
                                           1);
                                noack = receiver->bpop.xread_group_noack;
                            }

                            /* Emit the two elements sub-array consisting of
                             * the name of the stream and the data we
                             * extracted from it. Wrapped in a single-item
                             * array, since we have just one key. */
                            addReplyMultiBulkLen(receiver,1);
                            addReplyMultiBulkLen(receiver,2);
                            addReplyBulk(receiver,rl->key);

                            streamPropInfo pi = {
                                rl->key,
                                receiver->bpop.xread_group
                            };
                            streamReplyWithRange(receiver,s,&start,NULL,
                                                 receiver->bpop.xread_count,
                                                 0, group, consumer, noack, &pi);

                            /* Note that after we unblock the client, 'gt'
                             * and other receiver->bpop stuff are no longer
                             * valid, so we must do the setup above before
                             * this call. */
                            unblockClient(receiver);
                        }
                    }
                }
            }
            server.fixed_time_expire--;

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }
}

/* This is how the current blocking lists/sorted sets/streams work, we use
 * BLPOP as example, but the concept is the same for other list ops, sorted
 * sets and XREAD.
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 */

/* Set a client in blocking mode for the specified key (list, zset or stream),
 * with the specified timeout. The 'type' argument is BLOCKED_LIST,
 * BLOCKED_ZSET or BLOCKED_STREAM depending on the kind of operation we are
 * waiting for an empty key in order to awake the client. The client is blocked
 * for all the 'numkeys' keys as in the 'keys' argument. When we block for
 * stream keys, we also provide an array of streamID structures: clients will
 * be unblocked only when items with an ID greater or equal to the specified one is appended to the stream. */
/* 处理对给定客户端在对应的键对象进行堵塞操作处理 */
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout, robj *target, streamID *ids) {
    dictEntry *de;
    list *l;
    int j;

	//在客户端上记录堵塞的超时时间值
    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (target != NULL) 
		incrRefCount(target);

	//循环处理堵塞的多个键对象
    for (j = 0; j < numkeys; j++) {
        /* The value associated with the key name in the bpop.keys dictionary is NULL for lists and sorted sets, or the stream ID for streams. */
        void *key_data = NULL;

		//检测是否是堵塞的流对象类型
        if (btype == BLOCKED_STREAM) {
            key_data = zmalloc(sizeof(streamID));
            memcpy(key_data,ids+j,sizeof(streamID));
        }

        /* If the key already exists in the dictionary ignore it. */
		//检测对应的客户端堵塞字典中是否已经存在对应的堵塞的键对象  防止多次插入相同的堵塞键对象
        if (dictAdd(c->bpop.keys,keys[j],key_data) != DICT_OK) {
            zfree(key_data);
            continue;
        }
		//增加对应的引用计数值
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
		//获取堵塞在对应键对象上的值对象
        de = dictFind(c->db->blocking_keys,keys[j]);
		//检测对应的列表值对象是否存在
        if (de == NULL) {
            int retval;
            /* For every key we take a list of clients blocked for it */
			//创建对应的堵塞在同一个键上的列表值对象
            l = listCreate();
			//将对应的键值对添加到堵塞字典上
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
			//增加对应的引用计数  这里增加引用计数是 在上述的字典中添加了对应的键值对 防止其他地方也引用 导致释放
            incrRefCount(keys[j]);
            serverAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
			//列表对象存储 获取对应的堵塞列表对象
            l = dictGetVal(de);
        }
		//将对应的需要添加到堵塞列表的客户端添加到堵塞列表上
        listAddNodeTail(l,c);
    }
	//触发堵塞客户端的处理
    blockClient(c,btype);
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP.
 * You should never call this function directly, but unblockClient() instead. */
void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    serverAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);
    di = dictGetIterator(c->bpop.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);

        /* Remove this client from the list of clients waiting for this key. */
        l = dictFetchValue(c->db->blocking_keys,key);
        serverAssertWithInfo(c,key,l != NULL);
        listDelNode(l,listSearchKey(l,c));
        /* If the list is empty we need to remove it to avoid wasting memory */
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    dictEmpty(c->bpop.keys,NULL);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
    if (c->bpop.xread_group) {
        decrRefCount(c->bpop.xread_group);
        decrRefCount(c->bpop.xread_consumer);
        c->bpop.xread_group = NULL;
        c->bpop.xread_consumer = NULL;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnLists() */
void signalKeyAsReady(redisDb *db, robj *key) {
    readyList *rl;

    /* No clients blocking for this key? No need to queue it. */
    if (dictFind(db->blocking_keys,key) == NULL) 
		return;

    /* Key was already signaled? No need to queue it again. */
    if (dictFind(db->ready_keys,key) != NULL) 
		return;

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1) check. */
    incrRefCount(key);
    serverAssert(dictAdd(db->ready_keys,key,NULL) == DICT_OK);
}




