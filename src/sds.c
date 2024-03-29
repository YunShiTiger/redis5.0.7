/* 
 * SDSLib 2.0 -- A C dynamic strings library
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include "sds.h"
#include "sdsalloc.h"

//特殊sds字符串   No Init
const char *SDS_NOINIT = "SDS_NOINIT";

/* 根据提供的sds类型来获取该sds类型对应的结构体大小 */
static inline int sdsHdrSize(char type) {
    switch(type&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return sizeof(struct sdshdr5);
        case SDS_TYPE_8:
            return sizeof(struct sdshdr8);
        case SDS_TYPE_16:
            return sizeof(struct sdshdr16);
        case SDS_TYPE_32:
            return sizeof(struct sdshdr32);
        case SDS_TYPE_64:
            return sizeof(struct sdshdr64);
    }
    return 0;
}

/**
 根据提供的字符串长度来获取对应的类型编码
 */
static inline char sdsReqType(size_t string_size) {
    if (string_size < 1<<5)
        return SDS_TYPE_5;
    if (string_size < 1<<8)
        return SDS_TYPE_8;
    if (string_size < 1<<16)
        return SDS_TYPE_16;
#if (LONG_MAX == LLONG_MAX)
    if (string_size < 1ll<<32)
        return SDS_TYPE_32;
    return SDS_TYPE_64;
#else
    return SDS_TYPE_32;
#endif
}

/* Create a new sds string with the content specified by the 'init' pointer and 'initlen'.
 * If NULL is used for 'init' the string is initialized with zero bytes.
 * If SDS_NOINIT is used, the buffer is left uninitialized;
 *
 * The string is always null-termined (all the sds strings are, always) so
 * even if you create an sds string with:
 *
 * mystring = sdsnewlen("abc",3);
 *
 * You can print the string with printf() as there is an implicit \0 at the
 * end of the string. However the string is binary safe and can contain
 * \0 characters in the middle, as the length is stored in the sds header. */
/* 根据提供的字符串和字符串长度创建对应的sds */
sds sdsnewlen(const void *init, size_t initlen) {
    void *sh;
    sds s;
	//根据提供的字符串长度初始化对应的sds类型
    char type = sdsReqType(initlen);
    /* Empty strings are usually created in order to append. Use type 8 since type 5 is not good at this. */
	//特殊处理获取到的5类型的类型转化
    if (type == SDS_TYPE_5 && initlen == 0) 
		type = SDS_TYPE_8;
	//根据sds类型来获取该sds类型对应的结构体大小
    int hdrlen = sdsHdrSize(type);
    unsigned char *fp; /* flags pointer. */
	
    //此处的s_malloc其实就是zmalloc函数,只是一个别名,注意这里，会给sds多增加一个字节的空间，由后面的s[initlen] = '\0';可知，作者是为了兼容C语言的字符串类型，这样就可以直接使用printf来输出sds了，这样非常的方便
    sh = s_malloc(hdrlen+initlen+1);
	//检测是否是特殊的初始化字符串
	if (init==SDS_NOINIT)
        init = NULL;
    else if (!init)
		//memset 用来对一段内存空间全部设置为某个字符，一般用在对定义的字符串进行初始化为‘ ’或‘/0’
        memset(sh, 0, hdrlen+initlen+1);
	//检测创建对应的空间是否成功
    if (sh == NULL) 
		return NULL;
	//获取sds中字符串存储的起始位置
    s = (char*)sh+hdrlen;
	//记录对应的sds类型空间的指向
    fp = ((unsigned char*)s)-1;
	//根据sds类型来初始化sds的内容
    switch(type) {
        case SDS_TYPE_5: {
            *fp = type | (initlen << SDS_TYPE_BITS);
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
    }
	//在初始化完成后，将init的内容拷贝进sds对象中，但是init如果原来等于SDS_NOINIT，就会被置为NULL，所以sds还是一串未知的字符串
    if (initlen && init)
        memcpy(s, init, initlen);
	//设置字符串结尾标识
    s[initlen] = '\0';
	//返回创建后的sds对象
    return s;
}

/* Create an empty (zero length) sds string. Even in this case the string always has an implicit null term. */
/* 创建一个空的sds对象 */
sds sdsempty(void) {
    return sdsnewlen("",0);
}

/* Create a new sds string starting from a null terminated C string. */
/* 根据给定的字符串创建对应的sds对象 */
sds sdsnew(const char *init) {
	//获取给定字符串对应的长度
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/* Duplicate an sds string. */
/* 复制字符串操作 */
sds sdsdup(const sds s) {
	//其实s指定的位置就是字符串真实的位置指向
    return sdsnewlen(s, sdslen(s));
}

/* Free an sds string. No operation is performed if 's' is NULL. */
/* sds释放函数 */
void sdsfree(sds s) {
    if (s == NULL) 
		return;
	//获取字符串对应的其实指向，进行释放空间处理
    s_free((char*)s-sdsHdrSize(s[-1]));
}

/* Set the sds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the sds string is hacked manually in some
 * way, like in the following example:
 *
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length remains 6 bytes. */
/* 重置给定sds的长度值 */
void sdsupdatelen(sds s) {
	//获取给定的sds的长度值
    size_t reallen = strlen(s);
	//重新设置sds的长度值
    sdssetlen(s, reallen);
}

/* Modify an sds string in-place to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
/* 字符串清空操作 */
void sdsclear(sds s) {
	//首先重置给定sds的长度为0
    sdssetlen(s, 0);
	//然后设置字符串结束符到第一个空间中
    s[0] = '\0';
}

/* Enlarge the free space at the end of the sds string so that the caller
 * is sure that after calling this function can overwrite up to addlen
 * bytes after the end of the string, plus one more byte for nul term.
 *
 * Note: this does not change the *length* of the sds string as returned
 * by sdslen(), but only the free buffer space we have. */
/* 对给定的sds结构进行空间扩充处理 */
sds sdsMakeRoomFor(sds s, size_t addlen) {
    void *sh, *newsh;
	//记录对应的可用空间数量
    size_t avail = sdsavail(s);
    size_t len, newlen;
	//创建新类型和获取老类型
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen;

    /* Return ASAP if there is enough space left. */
	//检测是否真的需要进行空间扩充处理
    if (avail >= addlen) 
		return s;
	
	//获取老字符串的长度值
    len = sdslen(s);
	//获取sds真实的起始位置
    sh = (char*)s-sdsHdrSize(oldtype);
	//计算新的sds长度值
    newlen = (len+addlen);
	//检测新的长度值是否超过了指定的值
    if (newlen < SDS_MAX_PREALLOC)
        newlen *= 2;
    else
        newlen += SDS_MAX_PREALLOC;
	//获取新的长度值对应的字符串类型
    type = sdsReqType(newlen);

    /* Don't use type 5: the user is appending to the string and type 5 is
     * not able to remember empty space, so sdsMakeRoomFor() must be called at every appending operation. */
    //特殊处理对应的类型
    if (type == SDS_TYPE_5) 
		type = SDS_TYPE_8;
	//获取对应类型占据的头结构占据的空间值
    hdrlen = sdsHdrSize(type);
	//检测字符串的类型是否变化来确定是否进行重新分配空间
    if (oldtype==type) {
		//类型相同 在原有空间上进行空间的重新分配处理
        newsh = s_realloc(sh, hdrlen+newlen+1);
		//检测重新分配空间是否正常
        if (newsh == NULL) 
			return NULL;
		//获取重新分配空间对应的字符串指向位置
        s = (char*)newsh+hdrlen;
    } else {
        /* Since the header size changes, need to move the string forward, and can't use realloc */
        //重新分配对应的空间值
		newsh = s_malloc(hdrlen+newlen+1);
		//检测空间分配是否正常
        if (newsh == NULL) 
			return NULL;
		//进行元素移动操作处理
        memcpy((char*)newsh+hdrlen, s, len+1);
		//释放原有的sds空间
        s_free(sh);
		//获取对应的字符串空间指向
        s = (char*)newsh+hdrlen;
		//设置对应的字符串类型值
        s[-1] = type;
		//设置新的sds的长度值
        sdssetlen(s, len);
    }
	//设置sds的空间分配值
    sdssetalloc(s, newlen);
	//返回扩充空间后的sds结构
    return s;
}

/* Reallocate the sds string so that it has no free space at the end. The
 * contained string remains not altered, but next concatenation operations
 * will require a reallocation.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/* 对给定的sds结构进行缩容操作处理 */
sds sdsRemoveFreeSpace(sds s) {
    void *sh, *newsh;
	//记录新老sds的字符串类型
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
	//记录新老sds的头的长度值
    int hdrlen, oldhdrlen = sdsHdrSize(oldtype);
	//记录对应的字符串长度值
    size_t len = sdslen(s);
	//记录对应的sds可用空间值
    size_t avail = sdsavail(s);
	//记录sds真实的位置指向
    sh = (char*)s-oldhdrlen;

    /* Return ASAP if there is no space left. */
	//检测是否有对应需要进行释放
    if (avail == 0) 
		return s;

    /* Check what would be the minimum SDS header that is just good enough to fit this string. */
	//获取指定长度对应的字符串类型
    type = sdsReqType(len);
	//获取指定类型对应的头的空间大小
    hdrlen = sdsHdrSize(type);

    /* If the type is the same, or at least a large enough type is still
     * required, we just realloc(), letting the allocator to do the copy
     * only if really needed. Otherwise if the change is huge, we manually
     * reallocate the string to use the different header type. */
    //检测类型是否需要进行变动操作处理
    if (oldtype==type || type > SDS_TYPE_8) {
		//进行空间位置的重新分配操作处理
        newsh = s_realloc(sh, oldhdrlen+len+1);
		//检测重新分配是否成功
        if (newsh == NULL) 
			return NULL;
		//获取新的sds中的字符串位置指向
        s = (char*)newsh+oldhdrlen;
    } else {
		//进行重新分配空间处理
        newsh = s_malloc(hdrlen+len+1);
		//检测重新分配空间是否成功
        if (newsh == NULL) 
			return NULL;
		//进行元素移动操作处理
        memcpy((char*)newsh+hdrlen, s, len+1);
		//释放老的sds的空间
        s_free(sh);
		//获取对应的字符串位置指向
        s = (char*)newsh+hdrlen;
		//设置新的类型值
        s[-1] = type;
		//设置对应的长度值
        sdssetlen(s, len);
    }
	//重新设置对应的sds的空间分配值
    sdssetalloc(s, len);
	//返回缩容后的sds结构
    return s;
}

/* Return the total size of the allocation of the specified sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term. */
/* 获取sds结构占据的总的空间大小值 */
size_t sdsAllocSize(sds s) {
	//获取字符串占据的空间大小值
    size_t alloc = sdsalloc(s);
	//计算总的空间占据值
    return sdsHdrSize(s[-1])+alloc+1;
}

/* Return the pointer of the actual SDS allocation (normally SDS strings are referenced by the start of the string buffer). */
/* 获取sds真是分配空间的起始位置指向 */
void *sdsAllocPtr(sds s) {
    return (void*) (s-sdsHdrSize(s[-1]));
}

/* Increment the sds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * This function is used in order to fix the string length after the
 * user calls sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * Usage example:
 *
 * Using sdsIncrLen() and sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * sds string without copying into an intermediate buffer:
 *
 * 应用实例
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * sdsIncrLen(s, nread);
 */
/* 给指定的sds增加或者减少指向长度修复处理 */
void sdsIncrLen(sds s, ssize_t incr) {
	//获取对应的字符串类型标识
    unsigned char flags = s[-1];
    size_t len;
	//根据对应的类型进行相关操作
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            unsigned char *fp = ((unsigned char*)s)-1;
            unsigned char oldlen = SDS_TYPE_5_LEN(flags);
            assert((incr > 0 && oldlen+incr < 32) || (incr < 0 && oldlen >= (unsigned int)(-incr)));
            *fp = SDS_TYPE_5 | ((oldlen+incr) << SDS_TYPE_BITS);
            len = oldlen+incr;
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
			//计算新的字符串长度值
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
			//计算新的字符串长度值
			len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (unsigned int)incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
			//计算新的字符串长度值
			len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (uint64_t)incr) || (incr < 0 && sh->len >= (uint64_t)(-incr)));
			//计算新的字符串长度值
			len = (sh->len += incr);
            break;
        }
        default: len = 0; /* Just to avoid compilation warnings. */
    }
	//设置字符串结束标识
    s[len] = '\0';
}

/* Grow the sds to have the specified length. Bytes that were not part of the original length of the sds will be set to zero.
 * if the specified length is smaller than the current length, no operation is performed. */
/* 将给定的sds扩展到指定长度 并且填充对应的结束符 */
sds sdsgrowzero(sds s, size_t len) {
	//获取sds当前的长度值
    size_t curlen = sdslen(s);
	//检测需要扩展的长度值是否小于对应的当前长度值
    if (len <= curlen) 
		//返回原始的sds
		return s;
	//将sds扩展空间处理
    s = sdsMakeRoomFor(s,len-curlen);
	//检测扩展空间是否成功
    if (s == NULL) 
		return NULL;

    /* Make sure added region doesn't contain garbage */
	//将扩展后的空间设置为结束符
    memset(s+curlen,0,(len-curlen+1)); /* also set trailing \0 byte */
	//设置sds新的长度值
    sdssetlen(s, len);
	//返回新的sds结构
    return s;
}

/* Append the specified binary-safe string pointed by 't' of 'len' bytes to the
 * end of the specified sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/* 将指定的字符串添加到sds结构中 */
sds sdscatlen(sds s, const void *t, size_t len) {
	//获取sds当前的长度
    size_t curlen = sdslen(s);
	//尝试进行扩容操作处理
    s = sdsMakeRoomFor(s,len);
	//检测扩容操作是否成功
    if (s == NULL) 
		return NULL;
	//将对应的字符串内容填充到sds结构中
    memcpy(s+curlen, t, len);
	//设置sds新的长度值
    sdssetlen(s, curlen+len);
	//设置字符串的结束标识
    s[curlen+len] = '\0';
	//返回新的sds结构
    return s;
}

/* Append the specified null termianted C string to the sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/* 将指定的字符串添加到sds结构中 */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/* Append the specified sds 't' to the existing sds 's'.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/* 将指定的sds结构添加到sds结构中 */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* Destructively modify the sds string 's' to hold the specified binary safe string pointed by 't' of length 'len' bytes. */
/* 将指定的字符串内容设置给sds结构中 */
sds sdscpylen(sds s, const char *t, size_t len) {
	//首先检测是否有足够的空间来存放新的字符串数据
    if (sdsalloc(s) < len) {
		//进行sds空间扩充操作处理
        s = sdsMakeRoomFor(s,len-sdslen(s));
		//检测扩展空间是否成功
        if (s == NULL) 
			return NULL;
    }
	//将对应的字符串迁移到对应的sds中
    memcpy(s, t, len);
	//设置字符串结束位置
    s[len] = '\0';
	//设置sds的长度值
    sdssetlen(s, len);
	//返回对应的sds结构
    return s;
}

/* Like sdscpylen() but 't' must be a null-termined string so that the length of the string is obtained with strlen(). */
/* 将指定的字符串内容设置给sds结构中 */
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* Helper for sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least SDS_LLSTR_SIZE bytes.
 *
 * The function returns the length of the null-terminated string representation stored at 's'. */
/* 将对应的长整数转换成对应的字符串格式 */
#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces an reversed string. */
	//将对应的负数转换成正数处理
    v = (value < 0) ? -value : value;
    p = s;
	//循环向空间中写入对应的数字位数
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
	//特殊处理负数的情况,添加负号标识
    if (value < 0) 
		*p++ = '-';

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
	//循环进行数字位置颠倒处理
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
/* 将对应的无符号长整数转换成对应的字符串格式 */
int sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces an reversed string. */
	//使用p指向对应的存储数字的空间
    p = s;
	//循环向空间中写入对应的数字位数
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    /* Compute length and add null term. */
	//记录对应的无符号长整数转换成字符串类型的长度值
    l = p-s;
	//设置字符串结束标识
    *p = '\0';

    /* Reverse the string. */
	//循环进行数字位置颠倒处理
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
	//返回对应的长度值
    return l;
}

/* Create an sds string from a long long value. It is much faster than:
 * sdscatprintf(sdsempty(),"%lld\n", value); */
/* 将给定的长整型转换成对应的字符串格式 */
sds sdsfromlonglong(long long value) {
    //定义存储空间
    char buf[SDS_LLSTR_SIZE];
	//进行转换操作处理
    int len = sdsll2str(buf,value);
	//生成对应的sds格式
    return sdsnewlen(buf,len);
}

/* Like sdscatprintf() but gets va_list instead of being variadic. */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = s_malloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    while(1) {
        buf[buflen-2] = '\0';
        va_copy(cpy,ap);
        vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (buf[buflen-2] != '\0') {
            if (buf != staticbuf) s_free(buf);
            buflen *= 2;
            buf = s_malloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = sdscat(s, buf);
    if (buf != staticbuf) s_free(buf);
    return t;
}

/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    size_t initlen = sdslen(s);
    const char *f = fmt;
    long i;
    va_list ap;

    va_start(ap,fmt);
    f = fmt;    /* Next format specifier byte to process. */
    i = initlen; /* Position of the next byte to write to dest str. */
    while(*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (sdsavail(s)==0) {
            s = sdsMakeRoomFor(s,1);
        }

        switch(*f) {
        case '%':
            next = *(f+1);
            f++;
            switch(next) {
            case 's':
            case 'S':
                str = va_arg(ap,char*);
                l = (next == 's') ? strlen(str) : sdslen(str);
                if (sdsavail(s) < l) {
                    s = sdsMakeRoomFor(s,l);
                }
                memcpy(s+i,str,l);
                sdsinclen(s,l);
                i += l;
                break;
            case 'i':
            case 'I':
                if (next == 'i')
                    num = va_arg(ap,int);
                else
                    num = va_arg(ap,long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsll2str(buf,num);
                    if (sdsavail(s) < l) {
                        s = sdsMakeRoomFor(s,l);
                    }
                    memcpy(s+i,buf,l);
                    sdsinclen(s,l);
                    i += l;
                }
                break;
            case 'u':
            case 'U':
                if (next == 'u')
                    unum = va_arg(ap,unsigned int);
                else
                    unum = va_arg(ap,unsigned long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsull2str(buf,unum);
                    if (sdsavail(s) < l) {
                        s = sdsMakeRoomFor(s,l);
                    }
                    memcpy(s+i,buf,l);
                    sdsinclen(s,l);
                    i += l;
                }
                break;
            default: /* Handle %% and generally %<unknown>. */
                s[i++] = next;
                sdsinclen(s,1);
                break;
            }
            break;
        default:
            s[i++] = *f;
            sdsinclen(s,1);
            break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;
}

/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminted C string.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "HelloWorld". */
/* 在给定的字符串前后截取对应的字符*/
sds sdstrim(sds s, const char *cset) {
    char *start, *end, *sp, *ep;
    size_t len;
	//初始化起始位置
    sp = start = s;
	//初始化结束位置
    ep = end = s+sdslen(s)-1;
	//从开始位置循环检测字符串中的字符是否在给定的字符串中
    while(sp <= end && strchr(cset, *sp)) 
		//向后移动位置
		sp++;
	//从结束位置循环检测字符串中的字符是否在给定的字符串中
    while(ep > sp && strchr(cset, *ep)) 
		//向前移动位置
		ep--;
	//根据两个指针的位置计算新字符串的长度值
    len = (sp > ep) ? 0 : ((ep-sp)+1);
	//根据起始指针的指向位置检测是否需要进行移动字符串元素处理
    if (s != sp) 
		//移动字符串中的相关元素
		memmove(s, sp, len);
	//设置字符串新的结束位置值
    s[len] = '\0';
	//设置sds新的长度值
    sdssetlen(s,len);
	//返回对应的sds对象
    return s;
}

/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * Example:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World" */
 /* 截取指定范围内的字符串 本方法的核心在与计算截取的起始位置点和截取的长度值，然后利用memmove函数进行移动元素 */
void sdsrange(sds s, ssize_t start, ssize_t end) {
    size_t newlen, len = sdslen(s);
	//首先检测给定的字符串是否长度为0
    if (len == 0) 
		return;
	//检测并重置起始位置值
    if (start < 0) {
        start = len+start;
        if (start < 0) 
			start = 0;
    }
	//检测并重置结束位置值
    if (end < 0) {
        end = len+end;
        if (end < 0) 
			end = 0;
    }
	//根据重置的起始和结束位置计算新的字符串长度值
    newlen = (start > end) ? 0 : (end-start)+1;

	//根据计算的新的长度值进一步进行修正长度值的处理
    if (newlen != 0) {
		//检测起始位置是否超过了字符串的长度值
        if (start >= (ssize_t)len) {
            newlen = 0;
        } else if (end >= (ssize_t)len) {
        	//重置结束位置为字符串的结束位置
            end = len-1;
			//重新计算对应的截取的字符串的长度值
            newlen = (start > end) ? 0 : (end-start)+1;
        }
    } else {
    	//截取长度值为0 设置起始位置为0      感觉这个地方不设置为0 也是可以的
        start = 0;
    }
	//在起始位置和新的字符串长度不为0的情况下进行字符串数据移动操作处理
    if (start && newlen) 
		//将指定位置后的字符串移动到起始位置上
		memmove(s, s+start, newlen);
	//设置新的字符串的结束标识
    s[newlen] = 0;
	//设置新的sds的长度值
    sdssetlen(s,newlen);
}

/* Apply tolower() to every character of the sds string 's'. */
/* sds字符串转小写 */
void sdstolower(sds s) {
    size_t len = sdslen(s), j;
	//循环进行字符转换处理
    for (j = 0; j < len; j++) 
		s[j] = tolower(s[j]);
}

/* Apply toupper() to every character of the sds string 's'. */
/* sds字符串转大写 */
void sdstoupper(sds s) {
    size_t len = sdslen(s), j;
	//循环进行字符转换处理
    for (j = 0; j < len; j++) 
		s[j] = toupper(s[j]);
}

/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than the smaller one. */
 /* 比较给定的两个sds的大小 */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;
	//首先获取两个给定的sds的长度值
    l1 = sdslen(s1);
    l2 = sdslen(s2);
	//获取最小的长度值
    minlen = (l1 < l2) ? l1 : l2;
	//在最小长度内比较两个sds的大小
    cmp = memcmp(s1,s2,minlen);
	//检测比较的结果是否相同
    if (cmp == 0) 
		//在相同的情况下 比较sds长度的大小
		return l1>l2? 1: (l1<l2? -1: 0);
	//在不相同的情况下，返回比较结果
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the same function but for zero-terminated strings. */
/*  */
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5;
    long start = 0, j;
    sds *tokens;

    if (seplen < 1 || len < 0) 
		return NULL;

    tokens = s_malloc(sizeof(sds)*slots);
    if (tokens == NULL) 
		return NULL;

    if (len == 0) {
        *count = 0;
        return tokens;
    }
    for (j = 0; j < (len-(seplen-1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements+2) {
            sds *newtokens;

            slots *= 2;
            newtokens = s_realloc(tokens,sizeof(sds)*slots);
            if (newtokens == NULL) 
				goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {
            tokens[elements] = sdsnewlen(s+start,j-start);
            if (tokens[elements] == NULL) 
				goto cleanup;
            elements++;
            start = j+seplen;
            j = j+seplen-1; /* skip the separator */
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) 
		goto cleanup;
    elements++;
    *count = elements;
    return tokens;

cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);
        s_free(tokens);
        *count = 0;
        return NULL;
    }
}

/* Free the result returned by sdssplitlen(), or do nothing if 'tokens' is NULL. */
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]);
    s_free(tokens);
}

/* Append to the sds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatrepr(sds s, const char *p, size_t len) {
    s = sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': 
			s = sdscatlen(s,"\\n",2); 
			break;
        case '\r': 
			s = sdscatlen(s,"\\r",2); 
			break;
        case '\t': 
			s = sdscatlen(s,"\\t",2); 
			break;
        case '\a': 
			s = sdscatlen(s,"\\a",2); 
			break;
        case '\b': 
			s = sdscatlen(s,"\\b",2); 
			break;
        default:
            if (isprint(*p))
                s = sdscatprintf(s,"%c",*p);
            else
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
    return sdscatlen(s,"\"",1);
}

/* Helper function for sdssplitargs() that returns non zero if 'c' is a valid hex digit. */
/* 检测给定的字符是否是在16进制字符范围内 */
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Helper function for sdssplitargs() that converts a hex digit into an integer from 0 to 15 */
/* 将给定的字符转换成16进制数 */
int hex_digit_to_int(char c) {
    switch(c) {
    case '0': 
		return 0;
    case '1': 
		return 1;
    case '2': 
		return 2;
    case '3': 
		return 3;
    case '4': 
		return 4;
    case '5': 
		return 5;
    case '6': 
		return 6;
    case '7': 
		return 7;
    case '8': 
		return 8;
    case '9': 
		return 9;
    case 'a': 
	case 'A': 
		return 10;
    case 'b': 
	case 'B': 
		return 11;
    case 'c': 
	case 'C': 
		return 12;
    case 'd': 
	case 'D': 
		return 13;
    case 'e': 
	case 'E': 
		return 14;
    case 'f': 
	case 'F': 
		return 15;
    default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters as in: "foo"bar or "foo' */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        /* skip blanks */
        while(*p && isspace(*p)) 
			p++;
        if (*p) {
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                                             is_hex_digit(*(p+2)) &&
                                             is_hex_digit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p+2))*16)+
                                hex_digit_to_int(*(p+3));
                        current = sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':
                        inq=1;
                        break;
                    case '\'':
                        insq=1;
                        break;
                    default:
                        current = sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) 
					p++;
            }
            /* add the token to the vector */
            vector = s_realloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) 
				vector = s_malloc(sizeof(void*));
            return vector;
        }
    }

err:
    while((*argc)--)
        sdsfree(vector[*argc]);
    s_free(vector);
    if (current) 
		sdsfree(current);
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * The function returns the sds string pointer, that is always the same as the input pointer since no resize is needed. */
/* 将给定的sds中的字符串内容与from相同的字符替换成对应位置to中的字符     */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);
	//循环进行替换操作处理
    for (j = 0; j < l; j++) {
		//循环进行检测操作处理
        for (i = 0; i < setlen; i++) {
			//检测是否相同
            if (s[j] == from[i]) {
				//进行替换操作处理
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string). Returns the result as an sds string. */
/* 拼接对应的字符串到一个集合sds中 */
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty();
    int j;
    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc-1) 
			join = sdscat(join,sep);
    }
    return join;
}

/* Like sdsjoin, but joins an array of SDS strings. */
/* 拼接对应的sds到一个集合sds中 */
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen) {
	//创建一个空的sds结构
    sds join = sdsempty();
    int j;
	//循环进行拼接操作处理
    for (j = 0; j < argc; j++) {
		//将对应的sds添加到拼接集合中
        join = sdscatsds(join, argv[j]);
		//检测是否是最后一个需要添加的元素
        if (j != argc-1) 
			//将对应的分割符添加到sds结构中
			join = sdscatlen(join,sep,seplen);
    }
	//返回拼接后的sds结构
    return join;
}

/* Wrappers to the allocators used by SDS. Note that SDS will actually
 * just use the macros defined into sdsalloc.h in order to avoid to pay the overhead of function calls. Here we define these wrappers only for
 * the programs SDS is linked to, if they want to touch the SDS internals even if they use a different allocator. */
void *sds_malloc(size_t size) { return s_malloc(size); }
void *sds_realloc(void *ptr, size_t size) { return s_realloc(ptr,size); }
void sds_free(void *ptr) { s_free(ptr); }

#if defined(SDS_TEST_MAIN)
#include <stdio.h>
#include "testhelp.h"
#include "limits.h"

#define UNUSED(x) (void)(x)
int sdsTest(void) {
    {
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length", sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0)

        sdsfree(x);
        x = sdsnewlen("foo",2);
        test_cond("Create a string with specified length", sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0)

        x = sdscat(x,"bar");
        test_cond("Strings concatenation", sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        test_cond("sdscpy() against an originally longer string", sdslen(x) == 1 && memcmp(x,"a\0",2) == 0)

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
            sdslen(x) == 33 && memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0)

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        test_cond("sdscatprintf() seems working in the base case", sdslen(x) == 3 && memcmp(x,"123\0",4) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        test_cond("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808," "9223372036854775807--",60) == 0)
        printf("[%s]\n",x);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 && memcmp(x,"--4294967295,18446744073709551615--",35) == 0)

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," x");
        test_cond("sdstrim() works when all chars match", sdslen(x) == 0)

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," ");
        test_cond("sdstrim() works when a single char remains", sdslen(x) == 1 && x[0] == 'x')

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters", sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0)

        y = sdsdup(x);
        sdsrange(y,1,1);
        test_cond("sdsrange(...,1,1)", sdslen(y) == 1 && memcmp(y,"i\0",2) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        test_cond("sdsrange(...,1,-1)", sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        test_cond("sdsrange(...,-2,-1)", sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        test_cond("sdsrange(...,2,1)", sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        test_cond("sdsrange(...,1,100)", sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        test_cond("sdsrange(...,100,100)", sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x,y) > 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) < 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        test_cond("sdscatrepr(...data...)", memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0)

        {
            unsigned int oldfree;
            char *p;
            int step = 10, j, i;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            test_cond("sdsnew() free/len buffers", sdslen(x) == 1 && sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                int oldlen = sdslen(x);
                x = sdsMakeRoomFor(x,step);
                int type = x[-1]&SDS_TYPE_MASK;

                test_cond("sdsMakeRoomFor() len", sdslen(x) == oldlen);
                if (type != SDS_TYPE_5) {
                    test_cond("sdsMakeRoomFor() free", sdsavail(x) >= step);
                    oldfree = sdsavail(x);
                }
                p = x+oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A'+j;
                }
                sdsIncrLen(x,step);
            }
            test_cond("sdsMakeRoomFor() content",
                memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ",x,101) == 0);
            test_cond("sdsMakeRoomFor() final length",sdslen(x)==101);

            sdsfree(x);
        }
    }
    test_report()
    return 0;
}
#endif

#ifdef SDS_TEST_MAIN
int main(void) {
    return sdsTest();
}
#endif



