/*
 * 
 */

#ifndef __RDB_H
#define __RDB_H

#include <stdio.h>
#include "rio.h"

/* TBD: include only necessary headers. */
#include "server.h"

/* The current RDB version. When the format changes in a way that is no longer backward compatible this number gets incremented. */
#define RDB_VERSION 9

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|XXXXXX => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|XXXXXX XXXXXXXX =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => A full 32 bit len in net byte order will follow
 * 10|000001 [64 bit integer] => A full 64 bit len in net byte order will follow
 * 11|OBKIND this means: specially encoded object will follow. The six bits number specify the kind of object that follows. See the RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may values, will fit inside. */
#define RDB_6BITLEN 0            
#define RDB_14BITLEN 1
#define RDB_32BITLEN 0x80
#define RDB_64BITLEN 0x81
#define RDB_ENCVAL 3              //11 标识是整数类型或者压缩类型的字符串数据标识
#define RDB_LENERR UINT64_MAX

/* When a length of a string object stored on disk has the first two bits set, the remaining six bits specify a special encoding for the object accordingly to the following defines: */
#define RDB_ENC_INT8 0        /* 8 bit signed integer */
#define RDB_ENC_INT16 1       /* 16 bit signed integer */
#define RDB_ENC_INT32 2       /* 32 bit signed integer */
#define RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* Map object types to RDB object types. Macros starting with OBJ_ are for memory storage and may change. Instead RDB types must be fixed because we store them on disk. */
/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW */
/* 用于存储于rdb文件中的对象类型标识 */
#define RDB_TYPE_STRING 0
#define RDB_TYPE_LIST   1
#define RDB_TYPE_SET    2
#define RDB_TYPE_ZSET   3
#define RDB_TYPE_HASH   4
#define RDB_TYPE_ZSET_2 5 /* ZSET version 2 with doubles stored in binary. */
#define RDB_TYPE_MODULE 6
#define RDB_TYPE_MODULE_2 7 /* Module value with annotations for parsing without the generating module being loaded. */

/* Object types for encoded objects. */
/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW */
/* 用于存储于rdb文件中对象的编码实现标识*/
#define RDB_TYPE_HASH_ZIPMAP    9
#define RDB_TYPE_LIST_ZIPLIST  10
#define RDB_TYPE_SET_INTSET    11
#define RDB_TYPE_ZSET_ZIPLIST  12
#define RDB_TYPE_HASH_ZIPLIST  13
#define RDB_TYPE_LIST_QUICKLIST 14
#define RDB_TYPE_STREAM_LISTPACKS 15

/* Test if a type is an object type. */
/* 用于检测给定的标识是否是值对象的对象类型或者编码类型访问的宏 */
#define rdbIsObjectType(t) ((t >= 0 && t <= 7) || (t >= 9 && t <= 15))

/* Special RDB opcodes (saved/loaded with rdbSaveType/rdbLoadType). */
#define RDB_OPCODE_MODULE_AUX 247   /* Module auxiliary data. */
#define RDB_OPCODE_IDLE       248   /* LRU idle time. */
#define RDB_OPCODE_FREQ       249   /* LFU frequency. */
#define RDB_OPCODE_AUX        250   /* RDB aux field. */
//数据库字典元素个数值类型 后续要写入 字典元素数量值 和 过期字典元素数量值
#define RDB_OPCODE_RESIZEDB   251   /* Hash table resize hint. */
#define RDB_OPCODE_EXPIRETIME_MS 252    /* Expire time in milliseconds. */
#define RDB_OPCODE_EXPIRETIME 253       /* Old expire time in seconds. */
//选择数据库索引类型
#define RDB_OPCODE_SELECTDB   254   /* DB number of the following keys. */
//数据库中的数据写入完成后 需要向rdb中写入的结束符
#define RDB_OPCODE_EOF        255   /* End of the RDB file. */

/* Module serialized values sub opcodes */
#define RDB_MODULE_OPCODE_EOF   0   /* End of module value. */
#define RDB_MODULE_OPCODE_SINT  1   /* Signed integer. */
#define RDB_MODULE_OPCODE_UINT  2   /* Unsigned integer. */
#define RDB_MODULE_OPCODE_FLOAT 3   /* Float. */
#define RDB_MODULE_OPCODE_DOUBLE 4  /* Double. */
#define RDB_MODULE_OPCODE_STRING 5  /* String. */

/* rdbLoad...() functions flags. */
/* 加载rdb文件中的字符串数据 将其转换成对应标识的数据 */
#define RDB_LOAD_NONE   0                 //默认的字符串对象形式
#define RDB_LOAD_ENC    (1<<0)            //进行编码处理的字符串格式
#define RDB_LOAD_PLAIN  (1<<1)            //原始的字符串格式
#define RDB_LOAD_SDS    (1<<2)            //sds格式的字符串

//用于标识只是rdb模式的标识
#define RDB_SAVE_NONE 0
//用于表示rdb和aof的混合模式标识
#define RDB_SAVE_AOF_PREAMBLE (1<<0)

int rdbSaveType(rio *rdb, unsigned char type);
int rdbLoadType(rio *rdb);
int rdbSaveTime(rio *rdb, time_t t);
time_t rdbLoadTime(rio *rdb);
int rdbSaveLen(rio *rdb, uint64_t len);
int rdbSaveMillisecondTime(rio *rdb, long long t);
long long rdbLoadMillisecondTime(rio *rdb, int rdbver);
uint64_t rdbLoadLen(rio *rdb, int *isencoded);
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr);
int rdbSaveObjectType(rio *rdb, robj *o);
int rdbLoadObjectType(rio *rdb);
int rdbLoad(char *filename, rdbSaveInfo *rsi);
int rdbSaveBackground(char *filename, rdbSaveInfo *rsi);
int rdbSaveToSlavesSockets(rdbSaveInfo *rsi);
void rdbRemoveTempFile(pid_t childpid);
int rdbSave(char *filename, rdbSaveInfo *rsi);
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key);
size_t rdbSavedObjectLen(robj *o);
robj *rdbLoadObject(int type, rio *rdb, robj *key);
void backgroundSaveDoneHandler(int exitcode, int bysignal);
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime);
ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt);
robj *rdbLoadStringObject(rio *rdb);
ssize_t rdbSaveStringObject(rio *rdb, robj *obj);
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len);
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr);
int rdbSaveBinaryDoubleValue(rio *rdb, double val);
int rdbLoadBinaryDoubleValue(rio *rdb, double *val);
int rdbSaveBinaryFloatValue(rio *rdb, float val);
int rdbLoadBinaryFloatValue(rio *rdb, float *val);
int rdbLoadRio(rio *rdb, rdbSaveInfo *rsi, int loading_aof);
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi);

#endif







