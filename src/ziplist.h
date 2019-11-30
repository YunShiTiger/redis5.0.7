/*
 * 压缩列表结构
 */

#ifndef _ZIPLIST_H
#define _ZIPLIST_H

#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

unsigned char *ziplistNew(void);//创建一个空结构的压缩列表
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second);//合并给定的两个压缩列表的数据
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where);//在压缩列表的头部或者尾部插入元素
unsigned char *ziplistIndex(unsigned char *zl, int index);//获取执行索引位置的元素指向
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);//获取压缩列表中指定元素的后一个元素位置指向
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);//获取压缩列表中指定元素的前一个元素位置指向
unsigned int ziplistGet(unsigned char *p, unsigned char **sval, unsigned int *slen, long long *lval);//获取给定节点元素位置处对应的存储的数据
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen);//在指定的节点处插入对应新的节点元素
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);//在压缩列表中删除对应的节点元素
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num);//在压缩列表的指定索引处删除指定数量的元素节点
unsigned int ziplistCompare(unsigned char *p, unsigned char *s, unsigned int slen);//比较给定的元素节点的内容和给定的内容是否相同
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip);//从指定元素位置开始每跳过指定数量的元素检测是否与给定的内容是否相同
unsigned int ziplistLen(unsigned char *zl);//获取压缩列表的元素个数
size_t ziplistBlobLen(unsigned char *zl);//获取压缩列表占据的总的字节数量
void ziplistRepr(unsigned char *zl);//打印输出压缩列表的信息

#ifdef REDIS_TEST
int ziplistTest(int argc, char *argv[]);
#endif

#endif /* _ZIPLIST_H */




