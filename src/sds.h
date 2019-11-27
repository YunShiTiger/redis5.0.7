/* SDSLib 2.0 -- A C dynamic strings library
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __SDS_H
#define __SDS_H

//进行扩展字符串时最大的
#define SDS_MAX_PREALLOC (1024*1024)
const char *SDS_NOINIT;

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

typedef char *sds;

/* Note: sdshdr5 is never used, we just access the flags byte directly. However is here to document the layout of type 5 SDS strings. */
/* sdshdr有好几个类别，它们分别是：sdshdr5，sdshdr8，sdshdr16，sdshdr32，sdshdr64，其中sdshdr5是不使用的 */
/* 这五个结构体中，len表示字符串的长度，alloc表示buf指针分配空间的大小，flags表示该字符串的类型(sdshdr5，sdshdr8，sdshdr16，sdshdr32，sdshdr64),是由flags的低三位表示 */
/* 注意一个小细节：attribute ((packed))，这一段代码的作用是取消编译阶段的内存优化对齐功能。
	例如：struct aa {char a; int b;}; sizeof(aa) == 8;
	但是struct attribute ((packed)) aa {char a; int b;}; sizeof(aa) == 5;
	这个很重要，redis源码中不是直接对sdshdr某一个类型操作，往往参数都是sds，而sds就是结构体中的buf，
	在后面的源码分析中，你可能会经常看见s[-1]这种魔法一般的操作，而按照sdshdr内存分布s[-1]就是sdshdr中flags变量，由此可以获取到该sds指向的字符串的类型 
*/
struct __attribute__ ((__packed__)) sdshdr5 {
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; /* used */
    uint8_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

/* 字符串类型编码 字符串类型有5中类型 同时占据低3位 */
#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4

/* 获取对应字符串类型的掩码值 */
#define SDS_TYPE_MASK 7

/* 字符串类型占据的位数 */
#define SDS_TYPE_BITS 3

/* C 语言中的宏定义##操作符 */
/* 实例 SDS_HDR_VAR(8,s); 对应宏定义翻译的产物  struct sdshdr8 *sh = (void*)((s)-(sizeof(struct sdshdr##T))); */
/* 上述实例就可以根据指向buf的sds变量s得到sdshdr8的指针          即创建对应的一个sdshdr8类型变量指针 */
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));
/* 同上方的函数类似，根据指向buf的sds变量s得到sdshdr的指针 */
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T))))

//获取超短字符串对应的长度(即获取对应的字符串长度)
#define SDS_TYPE_5_LEN(f) ((f)>>SDS_TYPE_BITS)

/* 内联函数 */

/* 获取sds的长度 */
static inline size_t sdslen(const sds s) {
	//获取对应的字符串类型标记字节
    unsigned char flags = s[-1];
	//根据对应的掩码来区分字符串类型，同时获取对应的字符串长度
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->len;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->len;
    }
    return 0;
}

/* 获取sds的可用长度 */
static inline size_t sdsavail(const sds s) {
	//获取对应的字符串类型标记字节
    unsigned char flags = s[-1];
	//根据对应的掩码来区分字符串类型，同时获取对应的可以使用的空间
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            return 0;
        }
        case SDS_TYPE_8: {
			//此处使用对应的宏函数来创建一个对应的变量
            SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}

/* 设置sds的长度值 */
static inline void sdssetlen(sds s, size_t newlen) {
	//获取对应的字符串类型标记字节
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
            	//获取对应的字符串类型对应的空间指向
                unsigned char *fp = ((unsigned char*)s)-1;
				//设置新的字符串长度值
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len = newlen;
            break;
    }
}

/* 增加sds的长度 */
static inline void sdsinclen(sds s, size_t inc) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                unsigned char newlen = SDS_TYPE_5_LEN(flags)+inc;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len += inc;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len += inc;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len += inc;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len += inc;
            break;
    }
}

/* sdsalloc() = sdsavail() + sdslen() */
/* 获取sds已分配空间的大小 */
static inline size_t sdsalloc(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->alloc;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->alloc;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->alloc;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->alloc;
    }
    return 0;
}

/* 重新设置sds已分配空间的大小 */
static inline void sdssetalloc(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->alloc = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->alloc = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->alloc = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->alloc = newlen;
            break;
    }
}

/* SDS提供的相关函数 大部分函数都很简单，只是对zmalloc文件里面的函数，sds中inline函数，或者是sdsnewlen函数的一层简单调用 */
sds sdsnewlen(const void *init, size_t initlen);//
sds sdsnew(const char *init);//
sds sdsempty(void); //获取一个空的sds对象
sds sdsdup(const sds s);//复制字符串
void sdsfree(sds s);//sds释放函数
sds sdsgrowzero(sds s, size_t len); //扩展字符串到指定长度
sds sdscatlen(sds s, const void *t, size_t len);//字符串的复制
sds sdscat(sds s, const char *t);
sds sdscatsds(sds s, const sds t);
sds sdscpylen(sds s, const char *t, size_t len);
sds sdscpy(sds s, const char *t);

sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

sds sdscatfmt(sds s, char const *fmt, ...);//字符串格式化输出
sds sdstrim(sds s, const char *cset);//字符串缩减
void sdsrange(sds s, ssize_t start, ssize_t end);//字符串截取函数
void sdsupdatelen(sds s);//重置给定sds的长度值
void sdsclear(sds s);//字符串清空操作
int sdscmp(const sds s1, const sds s2);
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count);
void sdsfreesplitres(sds *tokens, int count);
void sdstolower(sds s);//sds字符串转小写
void sdstoupper(sds s);//sds字符串转大写
sds sdsfromlonglong(long long value);
sds sdscatrepr(sds s, const char *p, size_t len);
sds *sdssplitargs(const char *line, int *argc);
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);
sds sdsjoin(char **argv, int argc, char *sep); //以分隔符连接字符串子数组构成新的字符串
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);
void sdsIncrLen(sds s, ssize_t incr);
sds sdsRemoveFreeSpace(sds s);
size_t sdsAllocSize(sds s);
void *sdsAllocPtr(sds s);

/* Export the allocator used by SDS to the program using SDS.
 * Sometimes the program SDS is linked to, may use a different set of
 * allocators, but may want to allocate or free things that SDS will respectively free or allocate. */
void *sds_malloc(size_t size);
void *sds_realloc(void *ptr, size_t size);
void sds_free(void *ptr);

#ifdef REDIS_TEST
int sdsTest(int argc, char *argv[]);
#endif

#endif


