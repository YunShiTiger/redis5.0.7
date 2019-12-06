/*
 * 用于存储有序整数集合的一种实现方式
 */

#ifndef __INTSET_H
#define __INTSET_H
#include <stdint.h>

/* 整数集合的存储表示结构 */
typedef struct intset {
	//表示当前整数集合的编码方式   即使用几个字节来表示一个整数
    uint32_t encoding;
	//集合中元素的个数
    uint32_t length;
	//用于真正存储整数集合的数组
    int8_t contents[];
} intset;

/* 有序整数集合对外提供的处理函数 */
intset *intsetNew(void);//创建一个空有序整数集合存储结构
intset *intsetAdd(intset *is, int64_t value, uint8_t *success);//
intset *intsetRemove(intset *is, int64_t value, int *success);//在整数集合中删除给定的整数
uint8_t intsetFind(intset *is, int64_t value);//检测整数集合中是否有对应的整数
int64_t intsetRandom(intset *is);//获取整数集合中随机位置的一个整数的值
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value);//获取整数集合中给定位置的整数值
uint32_t intsetLen(const intset *is);//获取给定整数集合中元素的个数
size_t intsetBlobLen(intset *is);//获取给定整数集合占据的总的字节个数 

#ifdef REDIS_TEST
int intsetTest(int argc, char *argv[]);
#endif

#endif // __INTSET_H




