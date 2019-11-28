/* 
 * SDSLib 2.0 -- A C dynamic strings library
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
    https://www.2cto.com/kf/201710/692138.html

    学习点1: 柔性数组空间占据问题
    https://www.cnblogs.com/veis/p/7073076.html
    https://blog.csdn.net/u014303647/article/details/88740109
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

/* 字符串类型编码 字符串类型有5中类型 同时占据低3位 这样可以表示8种类型 已经够用了 */
#define SDS_TYPE_5  0  //注意本种类型其实没有使用 表示32长度的短字符串
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4

/* 获取对应字符串类型的掩码值 */
#define SDS_TYPE_MASK 7

/* 字符串类型占据的位数 */
#define SDS_TYPE_BITS 3

/* C 语言中的宏定义##操作符
 * https://www.cnblogs.com/muzinian/archive/2012/11/25/2787929.html
 * https://www.jianshu.com/p/e9f00097904a
 */
/* 实例 SDS_HDR_VAR(8,s); 对应宏定义翻译的产物  struct sdshdr8 *sh = (void*)((s)-(sizeof(struct sdshdr##T))); */
/* 上述实例就可以根据指向buf的sds变量s得到sdshdr8的指针          即创建对应的一个sdshdr8类型变量指针 */
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));
/* 同上方的函数类似，根据指向buf的sds变量s得到sdshdr的指针 */
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T))))

//获取超短字符串对应的长度(即获取对应的字符串长度)
#define SDS_TYPE_5_LEN(f) ((f)>>SDS_TYPE_BITS)

/* 相关内联函数 */

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

/* 获取sds的剩余可用长度 */
static inline size_t sdsavail(const sds s) {
	//获取对应的字符串类型标记字节
    unsigned char flags = s[-1];
	//根据对应的掩码来区分字符串类型，同时获取对应的可以使用的空间
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            return 0;
        }
        case SDS_TYPE_8: {
			//此处使用对应的宏函数来创建一个对应的变量 sh
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

/* 设置sds新的长度值 */
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

/* 增加指定系数sds的使用长度 */
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
sds sdsnewlen(const void *init, size_t initlen);//根据给定的字符串和对应的字符串长度创建sds对象结构
sds sdsnew(const char *init);//根据给定的字符串创建对应的sds结构
sds sdsempty(void); //创建一个空的sds对象结构
sds sdsdup(const sds s);//复制给定的sds字符串数据
void sdsfree(sds s);//sds释放函数
sds sdsgrowzero(sds s, size_t len); //扩展字符串到指定长度，扩展中的空间初始化为对应的字符串结束符
sds sdscatlen(sds s, const void *t, size_t len);//在sds结构后追加字符串内容
sds sdscat(sds s, const char *t);//将指定的字符串添加到sds结构中
sds sdscatsds(sds s, const sds t);//将指定的sds结构追加到给定的sds结构中
sds sdscpylen(sds s, const char *t, size_t len);//将给定长度的字符串重置到sds结构中
sds sdscpy(sds s, const char *t);//将给定的字符串重置到sds结构中

sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

sds sdscatfmt(sds s, char const *fmt, ...);//字符串格式化输出
sds sdstrim(sds s, const char *cset);//前后端移除给定的字符串中的字符
void sdsrange(sds s, ssize_t start, ssize_t end);//字符串截取函数
void sdsupdatelen(sds s);//重置给定sds的长度值
void sdsclear(sds s);//字符串清空操作
int sdscmp(const sds s1, const sds s2);//比较给定的两个sds大小
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count);
void sdsfreesplitres(sds *tokens, int count);
void sdstolower(sds s);//sds字符串转小写
void sdstoupper(sds s);//sds字符串转大写
sds sdsfromlonglong(long long value);//将给定的长整型转换成对应的sds格式
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


