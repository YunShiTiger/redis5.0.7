/*
 * 
 */


#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"

struct _rio {
    /* Backend functions. Since this functions do not tolerate short writes or reads the return value is simplified to: zero on error, non zero on complete success. */
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
	
    int (*flush)(struct _rio *);
    /* The update_cksum method if not NULL is used to compute the checksum of all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf and len fields pointing to the new block of data to add to the checksum computation. */
    //更新对应校验码值的处理函数
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    /* The current checksum */
	//用于记录当前读取或者写入数据对应的校验码值
    uint64_t cksum;

    /* number of bytes read or written */
	//用于记录已经读取或者写入的字节数量
    size_t processed_bytes;

    /* maximum single read or write chunk size */
	//处理一片数据的字节门限值 即分配对多数据字节进行处理
    size_t max_processing_chunk;

    /* Backend-specific vars. */
    union {
        /* In-memory buffer target. */
        struct {
            sds ptr;
            off_t pos;
        } buffer;
        /* Stdio file pointer target. */
        struct {
        	//对应的文件句柄
            FILE *fp;
			//用于记录对于上次手动操作fsync到现在还有多少字节没有手动执行sync操作 即大体可能有多少在操作系统的缓存汇总没有刷入到文件中
            off_t buffered; /* Bytes written since last fsync. */
			//用于记录缓存的字节到达此值时触发手动sync操作的门限值
            off_t autosync; /* fsync after 'autosync' bytes written. */
        } file;
        /* Multiple FDs target (used to write to N sockets). */
        struct {
            int *fds;       /* File descriptors. */
            int *state;     /* Error state of each fd. 0 (if ok) or errno. */
            int numfds;
            off_t pos;
            sds buf;
        } fdset;
    } io;
};

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the actual implementation of read / write / tell, and will update the checksum if needed. */
/* 将对应字节数量的字符串写入到rdb文件中 特点是 1 分批写入大数据 2 能够统计校验码值*/
static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
	//循环分配进行写入数据处理
    while (len) {
		//计算本次需要写入的字节数量值
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
		//检测是否配置了计算校验码的处理函数
        if (r->update_cksum) 
			//统计本次写入后数据的校验码值
			r->update_cksum(r,buf,bytes_to_write);
		//真正进行数据写入操作处理
        if (r->write(r,buf,bytes_to_write) == 0)
            return 0;
		//根据下一次写入数据的buffer位置
        buf = (char*)buf + bytes_to_write;
		//减少可写入的字节数量
        len -= bytes_to_write;
		//统计已写入的字节数量
        r->processed_bytes += bytes_to_write;
    }
	//返回写入成功标识
    return 1;
}

/* 从对应的rdb文件中读取到对应的缓存buffer中 */
static inline size_t rioRead(rio *r, void *buf, size_t len) {
	//循环分配进行读取数据处理
    while (len) {
		//计算本次需要读取的字节数量值
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
		//真正进行数据读取操作处理
        if (r->read(r,buf,bytes_to_read) == 0)
            return 0;
		//检测是否配置了计算校验码的处理函数
        if (r->update_cksum) 
			//统计本次读取后数据的校验码值
			r->update_cksum(r,buf,bytes_to_read);
		//根据下一次读取数据的buffer位置
        buf = (char*)buf + bytes_to_read;
		//减少可读取的字节数量
        len -= bytes_to_read;
		//统计已读取的字节数量
        r->processed_bytes += bytes_to_read;
    }
	//返回读取成功标识
    return 1;
}

static inline off_t rioTell(rio *r) {
    return r->tell(r);
}

static inline int rioFlush(rio *r) {
    return r->flush(r);
}

void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);
void rioInitWithFdset(rio *r, int *fds, int numfds);

void rioFreeFdset(rio *r);

size_t rioWriteBulkCount(rio *r, char prefix, long count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

struct redisObject;
int rioWriteBulkObject(rio *r, struct redisObject *obj);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
void rioSetAutoSync(rio *r, off_t bytes);

#endif




