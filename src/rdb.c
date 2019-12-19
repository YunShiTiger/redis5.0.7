/*
 * redis中rdb备份的实现方式
 * rdb文件可以用于服务启动时加载的磁盘数据 或者 检测给定路径或者文件所对应的rdb文件是否完整
 * 相关介绍文档 https://blog.csdn.net/u012422440/article/details/94592513
 * rdb 文件的内容格式
 *   A   REDIS + RDB_VERSION  共9个字节的字符串数据
 *   B   SaveInfoAuxFields 存储辅助配置信息 是一对数据 即一对字符串数据
 *		   RDB_OPCODE_AUX 类型标识       250 后面是两个字符串数据 对应的两个字符串数据的存储处理
 *            对应存储的字符串进行分情况处理
 *              1 字符串长度小于等于 11 触发尝试编码整数操作处理  
 *                   11000000 2字节     后面一个空间存储整数值 
 * 					 11000001 3个字节 后面两个字节存储整数值 
 * 					 11000010 5字节     后面四个字节存储整数值 
 *              2 字符串长度大于20 同时服务器开启了进行压缩存储的标识 进行压缩存储字符串处理
 *                   压缩编码类型的标识 11000011
 *                   字符串压缩后占据的空间字节数
 *					 字符串压缩前占据的空间字节数
 *					 压缩后的字符串内容
 *              3 除去上述情况以外的字符串存储
 *                   首先写入字符串长度值
 *					 然后写入字符串数据
 *         此处需要区分编码整数值和对应的字符串长度值 以及对应的压缩字符串数据的存储区分标识
 *
 *         纯表示整数          11000000  11000001  11000010
 *		   标识压缩字符串 11000011
 *         标识整数长度         00******  01******  10000000    10000001  
 *   C	 SaveModulesAux 存储模块相关的数据                 ------------------->此处暂时不解析
 *   D   开始循环存入每个数据库中的数据
 *         1  RDB_OPCODE_SELECTDB    254  标识是选择对应数据库索引的标识
 *         2  写入对应的索引整数值
 *         3  RDB_OPCODE_RESIZEDB    251  标识当前索引库对应的需要写入数据键和过期键元素个数的标识
 *         4  写入字典键元素数量值
 *         5  写入过期字典键元素数量值
 *         6  开始核心操作 即循环存储当前索引对应的键值对的数据到rdb文件中
 *               1 检测是否需要写入过期时间值
 *					  RDB_OPCODE_EXPIRETIME_MS 252 标识键值对有对应的过期时间的标识
 *                    写入对应的过期时间值
 *               2 根据服务器配置的内存策略来确定存储的lru或者lfu值
 *					  lru格式
 *					  	 RDB_OPCODE_IDLE  248 标识是lru格式的空转时间值
 *					  	 写入空转时间值
 *					  lfu格式
 *					  	 RDB_OPCODE_FREQ  249 标识是lfu格式的次数值
 *					  	 写入对应的次数值
 *               3 写入对应的值对象类型标识
 *					  RDB_TYPE_STRING      		 0
 *					  RDB_TYPE_LIST        		 1
 *					  RDB_TYPE_SET         		 2
 *					  RDB_TYPE_ZSET         	 3
 *					  RDB_TYPE_HASH         	 4
 *					  RDB_TYPE_ZSET_2       	 5
 *				  	  RDB_TYPE_MODULE       	 6
 *					  RDB_TYPE_MODULE_2     	 7
 *					  RDB_TYPE_HASH_ZIPMAP       9
 *					  RDB_TYPE_LIST_ZIPLIST     10
 *					  RDB_TYPE_SET_INTSET       11
 *					  RDB_TYPE_ZSET_ZIPLIST     12
 *					  RDB_TYPE_HASH_ZIPLIST     13
 *					  RDB_TYPE_LIST_QUICKLIST   14
 *					  RDB_TYPE_STREAM_LISTPACKS 15
 *               4  存储对应的键对象字符串对应的数据到rdb文件中 注意这个地方下面的陈述有问题 对于键字符串对象来说 只有字符串格式类型的 没有 整数编码类型的 这不过处理函数中分类型进行处理了
 *						如果是整数编码方式的键对象    
 *							如果对应的整数能够进行编码整数操作处理 就以编码整数的方式进行存储
 *							如果对应的整数不能进行编码整数操作处理 就将整数转换成字符串形式 先存储字符串长度值 然后存储对应的字符串数据	
 *						字符串编码方式的键对象                   以写入字符串的方式写入键字符串对象到rdb文件中 
 *               5  核心操作处理 存储值对象的数据 其实在前面已经写入了对应的值对象的编码实现方式
 *						字符串对象 按照键字符串方式进行处理 只不过在值对象是字符串对象时触发检测是否是编码整数方式
 *						列表对象      OBJ_ENCODING_QUICKLIST  能够进行下面操作的本质原因是对应的ziplist占据的空间是连续空间
 *							首先存储列表对象总的元素值
 *							循环遍历所有的压缩列表节点 存储节点内指向的ziplist结构
 *			                    如果节点进行了压缩            以压缩字符串的方式进行存储  -->压缩编码类型-->字符串压缩后占据的空间字节数-->字符串压缩前占据的空间字节数-->压缩后的字符串内容
 *					            如果节点未进行压缩            以正常的字符串方式进行存储--> 走字符串存储的流程 
 *				        集合对象 根据编码方式进行不同的存储策略
 *							OBJ_ENCODING_HT
 *					        	首先写入当前字典结构中元素的数量值
 *								循环遍历所有的字典中的键值对 写入对应的字段字符串数据 因为字典结构实现的集合只使用了字典结构中的键部分
 *							OBJ_ENCODING_INTSET
 *								直接获取对应的整数集合的存储结构 进行存储处理 因为整数集合的空间是连续的
 *						Hash对象 根据编码方式进行不同的存储策略	
 *							OBJ_ENCODING_ZIPLIST
 *								直接将对应的ziplist结构中的数据以字符串格式进行写入
 *							OBJ_ENCODING_HT
 *								首先写入当前字典结构中元素的数量值
 *								循环遍历所有的字典中的键值对 写入对应的字段字符串数据 然后写入对应的值字符串数据
 *						有序集合对象 根据编码方式进行不同的存储策略
 *							OBJ_ENCODING_ZIPLIST
 *								直接将对应的ziplist结构中的数据以字符串格式进行写入
 *							OBJ_ENCODING_SKIPLIST
 *						流对象
 *						模块对象
 *						
 *					    在每处理一对键值对数据存储到rdb文件中 检测是否开启了rdb和aof混合模式 通过是否达到对应的门限值 来进一步确认是否进行将父进程中追加的命令 通过管道 输出到子进程中的aof缓存中 即aof重写阶段的处理追加命令的操作流程
 *				6	检测是否需要将脚本信息存储到rdb文件中	
 *						以辅助信息的方式写入脚本信息 SaveAuxField 其实对应的一个字符串是固定的 lua 另一个字符串是脚本本身
 *										
 *				7	写入与模块相关的数据 ？？？？？？？？？？ 后期进行进一步分析	
 *				8	写入结束符 RDB_OPCODE_EOF		255
 *				9	最后写入8字节的校验码数据	
 *								
 *						
 *							
 *						
 *					
 *						
 *					
 *						
 *					
 *					
 *						
 *						
 *						
 *						
 *							
 *					
 *						
 *						
 *					
 *						
 *						
 */

#include "server.h"
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"
#include "stream.h"

#include <math.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/param.h>

#define rdbExitReportCorruptRDB(...) rdbCheckThenExit(__LINE__,__VA_ARGS__)

extern int rdbCheckMode;
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

void rdbCheckThenExit(int linenum, char *reason, ...) {
    va_list ap;
    char msg[1024];
    int len;

    len = snprintf(msg,sizeof(msg), "Internal error in RDB reading function at rdb.c:%d -> ", linenum);
    va_start(ap,reason);
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    va_end(ap);

    if (!rdbCheckMode) {
        serverLog(LL_WARNING, "%s", msg);
        char *argv[2] = {"",server.rdb_filename};
        redis_check_rdb_main(2,argv,NULL);
    } else {
        rdbCheckError("%s",msg);
    }
	//退出redis服务
    exit(1);
}

/* 将对应字节数量的字符串数据通过rio结构写入到文件中 */
static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
	//触发写入操作处理,并检测写入是否成功
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
	//返回写入的字节数量
    return len;
}

/* This is just a wrapper for the low level function rioRead() that will automatically abort if it is not possible to read the specified amount of bytes. */
/* 从rio结构中读取指定数量的数据到对应的空间中 */
void rdbLoadRaw(rio *rdb, void *buf, uint64_t len) {
	//尝试读取指定数量的数据到对应的存储空间中
    if (rioRead(rdb,buf,len) == 0) {
		//读取失败设置错误信息
        rdbExitReportCorruptRDB("Impossible to read %llu bytes in rdbLoadRaw()",(unsigned long long) len);
        return; /* Not reached. */
    }
}

/* 向rio中写入类型标识数据 一个字节空间大小的标识*/
int rdbSaveType(rio *rdb, unsigned char type) {
	//写入一个字节的类型标识数据
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * This function is not only used to load object types, but also special "types" like the end-of-file type, the EXPIRE type, and so forth. */
/*在rio中读取一个字节大小的类型标识数据 */
int rdbLoadType(rio *rdb) {
    unsigned char type;
	//尝试读取一个字节的类型标识
    if (rioRead(rdb,&type,1) == 0) 
		//返回读取失败的标识
		return -1;
	//返回读取到的数据类型标识
    return type;
}

/* This is only used to load old databases stored with the RDB_OPCODE_EXPIRETIME opcode. New versions of Redis store using the RDB_OPCODE_EXPIRETIME_MS opcode. */
/* 从rdb文件中加载4字节的时间值 */
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
	//读取4字节的数据
    rdbLoadRaw(rdb,&t32,4);
    return (time_t)t32;
}

/* 向rdb文件中写入8字节的时间整数值 注意这个地方只是写入值 在这之前应该会写入一个类型标识 来标识下一次写入的是8字节的整数值 */
int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    memrev64ifbe(&t64); /* Store in little endian. */
	//写入8字节整数值
    return rdbWriteRaw(rdb,&t64,8);
}

/* This function loads a time from the RDB file. It gets the version of the
 * RDB because, unfortunately, before Redis 5 (RDB version 9), the function
 * failed to convert data to/from little endian, so RDB files with keys having
 * expires could not be shared between big endian and little endian systems
 * (because the expire time will be totally wrong). The fix for this is just
 * to call memrev64ifbe(), however if we fix this for all the RDB versions,
 * this call will introduce an incompatibility for big endian systems:
 * after upgrading to Redis version 5 they will no longer be able to load their
 * own old RDB files. Because of that, we instead fix the function only for new
 * RDB versions, and load older RDB versions as we used to do in the past, allowing big endian systems to load their own old RDB files. */
/* 从rdb文件中读取8字节的时间整数值 */
long long rdbLoadMillisecondTime(rio *rdb, int rdbver) {
    int64_t t64;
	//读取8字节的数据
    rdbLoadRaw(rdb,&t64,8);
	//检测当前版本是否是9版本的  从9版本的开始使用大端法进行存储了
    if (rdbver >= 9) /* Check the top comment of this function. */
        memrev64ifbe(&t64); /* Convert in big endian if the system is BE. */
    return (long long)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the RDB_* definitions for more information on the types of encoding. */
/* 下rdb文件中写入一个对应的长度数值 */
/* 00****** 使用一个字节空间就标识了对应的整数长度值 其中长度值存储在后6位中 
 * 01****** ******** 使用两个字节空间就标识了对应的整数长度值 其中长度值存储在后14位中 
 * 10000000 ******** ******** ******** ********  使用五个字节空间就标识了对应的整数长度值 其中长度值存储在后32位中 
 * 10000001 ******** ******** ******** ******** ******** ******** ******** ********  使用九个字节空间就标识了对应的整数长度值 其中长度值存储在后64位中 
 */
int rdbSaveLen(rio *rdb, uint64_t len) {
    unsigned char buf[2];
    size_t nwritten;
    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);
		//写入一字节空间大小的长度值
        if (rdbWriteRaw(rdb,buf,1) == -1) 
			return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
		//写入两字节空间大小的长度值
        if (rdbWriteRaw(rdb,buf,2) == -1) 
			return -1;
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
		//首先存储长度标识
        if (rdbWriteRaw(rdb,buf,1) == -1) 
			return -1;
        uint32_t len32 = htonl(len);
		//然后存入对应的长度值
        if (rdbWriteRaw(rdb,&len32,4) == -1) 
			return -1;
        nwritten = 1+4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
		//首先存储长度标识
        if (rdbWriteRaw(rdb,buf,1) == -1) 
			return -1;
        len = htonu64(len);
		//然后存入对应的长度值
        if (rdbWriteRaw(rdb,&len,8) == -1)
			return -1;
        nwritten = 1+8;
    }
	//返回写入rdb文件中的字节数量
    return nwritten;
}


/* Load an encoded length. If the loaded length is a normal length as stored with rdbSaveLen(), the read length is set to '*lenptr'. If instead the
 * loaded length describes a special encoding that follows, then '*isencoded' is set to 1 and the encoding format is stored at '*lenptr'.
 * See the RDB_ENC_* definitions in rdb.h for more information on specialencodings. The function returns -1 on error, 0 on success. */
/* 解析并读取对应的长度值 */
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) 
		*isencoded = 0;
	//首先读取一个字节来解析对应的长度值使用的空间数量情况
    if (rioRead(rdb,buf,1) == 0) 
		return -1;
	//获取对应的标识位
    type = (buf[0]&0xC0)>>6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) 
			//标识是编码类型的数据 即数据是整数数据或者压缩类型的字符串数据 长度标识中以11开头的
			*isencoded = 1;
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
		//再次读取1个字节的整数值
        if (rioRead(rdb,buf+1,1) == 0) 
			return -1;
		//拼接14位的整数值
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
		//读取4字节长度值
        if (rioRead(rdb,&len,4) == 0) 
			return -1;
		//存储对应的长度值
        *lenptr = ntohl(len);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
		//读取8字节长度值
        if (rioRead(rdb,&len,8) == 0) 
			return -1;
		//存储对应的长度值
        *lenptr = ntohu64(len);
    } else {
		//不属于长度范围内的标识 触发错误处理
        rdbExitReportCorruptRDB("Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
	//返回读取数据长度成功的标识
    return 0;
}

/* This is like rdbLoadLenByRef() but directly returns the value read
 * from the RDB stream, signaling an error by returning RDB_LENERR (since it is a too large count to be applicable in any Redis data structure). */
/* 在rdb文件中读取对应的长度值 */
uint64_t rdbLoadLen(rio *rdb, int *isencoded) {
    uint64_t len;
	//读取对应的长度值
    if (rdbLoadLenByRef(rdb,isencoded,&len) == -1) 
		return RDB_LENERR;
	//返回读取到的长度值
    return len;
}

/* Encodes the "value" argument as integer when it fits in the supported ranges for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string length is returned. Otherwise 0 is returned. */
/* 编码整数值 前8位空间表示占据的空间数量 
 * 11000000 2字节 后面一个空间标识数据大小 
 * 11000001 3个字节 后面两个字节存储数据 
 * 11000010 5字节 后面四个字节空间标识数据大小 
 */
int rdbEncodeInteger(long long value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype". The returned value changes according to the flags, see rdbGenerincLoadStringObject() for more info. */
/* 根据给定的解析的整数编码标识来加载对应的整数值 并根据对应的配置参数生成对应的对象 */
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags, size_t *lenptr) {
	//首先分析给定的参数标识
	//将对应的整数转换成对应字符串数据
    int plain = flags & RDB_LOAD_PLAIN;
	//将对应的整数转换成对应的sds字符串格式
    int sds = flags & RDB_LOAD_SDS;
	//将对应的整数转换成对应的压缩编码的整数字符串对象
    int encode = flags & RDB_LOAD_ENC;
    unsigned char enc[4];
	//用于存储获取的整数值
    long long val;

    if (enctype == RDB_ENC_INT8) {
		//读取一字节的整数值
        if (rioRead(rdb,enc,1) == 0) 
			return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
		//读取两字节的整数值
        if (rioRead(rdb,enc,2) == 0) 
			return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
		//读取四字节的整数值
        if (rioRead(rdb,enc,4) == 0) 
			return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
		//给定的整数编码标识不合法的错误处理
        val = 0; /* anti-warning */
        rdbExitReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
    }
	//根据类型来创建对应的结果数据
    if (plain || sds) {
        char buf[LONG_STR_SIZE], *p;
		//将对应的整数值转换成对应的字符串形式
        int len = ll2string(buf,sizeof(buf),val);
        if (lenptr) 
			*lenptr = len;
		//根据对应的参数来进行开辟空间操作处理
        p = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
		//将对应的转换的字符串数据存储到空间中
        memcpy(p,buf,len);
		//返回元素字符串格式数据的指向
        return p;
    } else if (encode) {
		//将整数转换成压缩格式的字符串对象
        return createStringObjectFromLongLongForValue(val);
    } else {
		//默认是 将对应的整数转换成对应的字符串对象 并返回对应的对象
        return createObject(OBJ_STRING,sdsfromlonglong(val));
    }
}

/* String objects in the form "2391" "-100" without any space and with a range of values that can fit in an 8, 16 or 32 bit signed value can be encoded as integers to save space */
/* 尝试进行编码整数值的处理操作 */
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
	//尝试将对应的字符串转换成对应的整数值
    value = strtoll(s, &endptr, 10);
	//检测转换是否成功
    if (endptr[0] != '\0') 
		return 0;
	//将对应的转换后的整数值转换成对应的字符串数据
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical then it's not possible to encode the string as integer */
	//进一步比较转换后和前的字符串数据是否匹配
    if (strlen(buf) != len || memcmp(buf,s,len)) 
		return 0;
	//进行编码整数操作处理
    return rdbEncodeInteger(value,enc);
}

/* 对给定的压缩字符串进行存储到rdb文件中的处理 */
ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len, size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk */
	//拼接压缩编码类型的标识 11000011
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF;
	//写入压缩编码数据类型
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) 
		goto writeerr;
    nwritten += n;
	//写入压缩后的字节空间大小
    if ((n = rdbSaveLen(rdb,compress_len)) == -1) 
		goto writeerr;
    nwritten += n;
	//写入压缩前的字节空间大小
    if ((n = rdbSaveLen(rdb,original_len)) == -1) 
		goto writeerr;
    nwritten += n;
	//存储具体的压缩后的字符串数据
    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) 
		goto writeerr;
    nwritten += n;
	//返回写入的字节数量
    return nwritten;

writeerr:
    return -1;
}

/* 尝试对给定的字符串数据进行压缩编码存储处理 */
ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
	//检测对应的需要压缩编码的字符串长度是否符合要求
    if (len <= 4) 
		return 0;
	//临时计算压缩需要的空间值
    outlen = len-4;
	//开辟对应的空间
    if ((out = zmalloc(outlen+1)) == NULL) 
		return 0;
	//进行尝试压缩操作处理
    comprlen = lzf_compress(s, len, out, outlen);
	//检测是否压缩操作成功
    if (comprlen == 0) {
		//释放对应的空间
        zfree(out);
        return 0;
    }
	//尝试将压缩后的字符串数据存入到rdb文件中
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len);
	//释放对应的空间
    zfree(out);
	//返回写入rdb文件的字节数量
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value changes according to 'flags'. For more info check the rdbGenericLoadStringObject() function. */
/* 读取压缩类型的字符串数据的操作处理 */
void *rdbLoadLzfStringObject(rio *rdb, int flags, size_t *lenptr) {
	//将对应的压缩字符串数据解码后 存储为原始字符串格式的数据
    int plain = flags & RDB_LOAD_PLAIN;
	//将对应的压缩字符串数据解码后 存储为sds格式的数据
    int sds = flags & RDB_LOAD_SDS;
    uint64_t len, clen;
    unsigned char *c = NULL;
    char *val = NULL;

	//读取字符串压缩后占据的字节长度值
    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
		return NULL;
	//读取字符串压缩前占据的字节长度值
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
		return NULL;
	//分配对应的空间,用于存储对应的字符串数据
    if ((c = zmalloc(clen)) == NULL) 
		goto err;

    /* Allocate our target according to the uncompressed size. */
	//根据标识来创建对应的空间 用于存储解压后的字符串数据
    if (plain) {
        val = zmalloc(len);
    } else {
        val = sdsnewlen(SDS_NOINIT,len);
    }
	//检测是否需要配置解压后的长度值
    if (lenptr) 
		*lenptr = len;

    /* Load the compressed representation and uncompress it to target. */
	//读取对应长度的压缩字符串数据
    if (rioRead(rdb,c,clen) == 0) 
		goto err;
	//尝试进行解压缩操作处理
    if (lzf_decompress(c,clen,val,len) == 0) {
        if (rdbCheckMode) 
			rdbCheckSetError("Invalid LZF compressed string");
        goto err;
    }
	//释放对应的压缩数据占用空间
    zfree(c);
	//根据标识来返回对应的数据
    if (plain || sds) {
        return val;
    } else {
    	//默认返回的是字符串对象格式
        return createObject(OBJ_STRING,val);
    }
err:
    zfree(c);
    if (plain)
        zfree(val);
    else
        sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string representation of an integer value we try to save it in a special form */
/* 写入对应数量的字符串数据到rdb文件中 */
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding */
	//字符串长度小于等于11 触发尝试进行编码整数操作处理
    if (len <= 11) {
		//定义对应的存储编码整数的空间 最大占用5字节空间 此处开辟空间 内部函数进行存储对应的值
        unsigned char buf[5];
		//尝试进行编码整数操作处理
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
			//将编码后的整数值写入到rdb文件中  注意编码的整数值的第一个字节能够标识整数占据的空间数量
            if (rdbWriteRaw(rdb,buf,enclen) == -1) 
				return -1;
			//返回写入的字节数量
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even aaaaaaaaaaaaaaaaaa so skip it */
	//检测是否配置了进行压缩存储rdb文件中过长字符串的标识 同时字符串数据长度超过了 20 个字节的门限值
    if (server.rdb_compression && len > 20) {
		/* Return value of 0 means data can't be compressed, save the old way */
		//尝试进行压缩数据处理 并获取压缩后的字节数量
        n = rdbSaveLzfStringObject(rdb,s,len);
		//检测是否存入成功
        if (n == -1) 
			return -1;
		//检测是否写入成功
        if (n > 0) 
			//返回写入的字节数量
			return n;
    }

    /* Store verbatim */
	//首先写入字符串的长度值
    if ((n = rdbSaveLen(rdb,len)) == -1) 
		return -1;
	//增加对应的写入字节数
    nwritten += n;
	//检测是否有对应的字符串数据需要写入---->此处一般不会是0值
    if (len > 0) {
		//然后写入对应的字符串数据
        if (rdbWriteRaw(rdb,s,len) == -1) 
			return -1;
		//计算写入数据的字节数量值
        nwritten += len;
    }
	//返回对应的写入字节数量
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. */
/* 将对应的整数以其编码后的字符串方式存储到rdb文件中 */
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
	//对给定的字符串进行编码操作处理
    int enclen = rdbEncodeInteger(value,buf);
	//检测编码操作是否成功
    if (enclen > 0) {
		//将对应的编码后的整数方式存储到rdb文件中
        return rdbWriteRaw(rdb,buf,enclen);
    } else {
        /* Encode as string */
		//将对应的整数转换成对应的字符串格式
        enclen = ll2string((char*)buf,32,value);
		//检测转换是否成功   32位长度足够了
        serverAssert(enclen < 32);
		//写入字符串长度值
        if ((n = rdbSaveLen(rdb,enclen)) == -1) 
			return -1;
        nwritten += n;
		//写入转换后的字符串数据
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) 
			return -1;
        nwritten += n;
    }
	//返回写入rdb文件的字节数
    return nwritten;
}

/* Like rdbSaveRawString() gets a Redis object instead. */
/* 存储对应的键对象字符串对应的数据到rdb文件中 注意键对象也是字符串对象类型 只不过键对象没有进行进行*/
ssize_t rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the object is already integer encoded. */
	//检测键对象是否是整数编码方式的对象
    if (obj->encoding == OBJ_ENCODING_INT) {
		//对整数类型的键对象进行编码操作处理 存储到rdb文件中
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);
    } else {
    	//检测对应的键对象是否是字符串类型的数据
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
		//以写入字符串的方式写入键字符串对象到rdb文件中
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to encode it in a special way to be more memory efficient. When this flag is passed the function no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc() instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 * On I/O error NULL is returned.
 */
/* 从rdb文件中读取对应的字符串 并将字符串转换成对应格式的数据 */
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr) {
	//解析状态位来确定最终生成的数据格式
    int encode = flags & RDB_LOAD_ENC;
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int isencoded;
    uint64_t len;

	//首先读取对应的字符串长度值 同时获取本字符串是否是整数或者压缩类型的字符串数据
    len = rdbLoadLen(rdb,&isencoded);
	//检测是否是编码类型的字符串数据
    if (isencoded) {
		//根据对应的长度来加载对应长度的字符串数据
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
			//加载并生成对应的字符串数据
            return rdbLoadIntegerObject(rdb,len,flags,lenptr);
        case RDB_ENC_LZF:
			//加载压缩数据并生成对应的字符串数据
            return rdbLoadLzfStringObject(rdb,flags,lenptr);
        default:
            rdbExitReportCorruptRDB("Unknown RDB string encoding type %d",len);
        }
    }

	//检测对应的长度值 是否越界了
    if (len == RDB_LENERR) 
		return NULL;
	//根据需要转换的格式来进行区分操作处理
    if (plain || sds) {
		//根据格式来分配对应大小的空间
        void *buf = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
		//记录对应的字符串长度值
        if (lenptr) 
			*lenptr = len;
		//从rdb文件中读取指定长度的字符串数据
        if (len && rioRead(rdb,buf,len) == 0) {
            if (plain)
                zfree(buf);
            else
                sdsfree(buf);
            return NULL;
        }
		//返回需要的指定格式的数据
        return buf;
    } else {
		//根据编码参数来创建对应的字符串对象
        robj *o = encode ? createStringObject(SDS_NOINIT,len) : createRawStringObject(SDS_NOINIT,len);
		//读取指定长度的字符串到指定的空间中
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
		//返回对应的字符串对象
        return o;
    }
}

/* 读取对应的字符串数据 使用默认的创建对应的字符串对象的方式 Raw格式 */
robj *rdbLoadStringObject(rio *rdb) {
	//读取rdb文件中的字符串数据 转换成对应的字符串对象形式
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
}

/* 读取对应的字符串数据 并开启当字符串数据比较短时 使用Embedded格式来创建字符串对象 否则使用Raw格式来创建字符对象 */
robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC,NULL);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf)-1,(long long)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue() */
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    if (rioRead(rdb,&len,1) == 0) 
		return -1;
    switch(len) {
    case 255: 
		*val = R_NegInf; 
		return 0;
    case 254: 
		*val = R_PosInf; 
		return 0;
    case 253: 
		*val = R_Nan; 
		return 0;
    default:
        if (rioRead(rdb,buf,len) == 0) 
			return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return 0;
    }
}

/* Saves a double for RDB 8 or greater, where IE754 binary64 format is assumed.
 * We just make sure the integer is always stored in little endian, otherwise the value is copied verbatim from memory to disk.
 *
 * Return -1 on error, the size of the serialized value on success. */
int rdbSaveBinaryDoubleValue(rio *rdb, double val) {
    memrev64ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Loads a double from RDB 8 or greater. See rdbSaveBinaryDoubleValue() for more info. On error -1 is returned, otherwise 0. */
int rdbLoadBinaryDoubleValue(rio *rdb, double *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) 
		return -1;
    memrev64ifbe(val);
    return 0;
}

/* Like rdbSaveBinaryDoubleValue() but single precision. */
int rdbSaveBinaryFloatValue(rio *rdb, float val) {
    memrev32ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Like rdbLoadBinaryDoubleValue() but single precision. */
int rdbLoadBinaryFloatValue(rio *rdb, float *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) 
		return -1;
    memrev32ifbe(val);
    return 0;
}

/* Save the object type of object "o". */
/* 向rdb文件中写入值对象的类型或者对应的编码实现类型 只占一个字节空间大小 */
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    case OBJ_STRING:
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    case OBJ_LIST:
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return rdbSaveType(rdb,RDB_TYPE_LIST_QUICKLIST);
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_2);
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_HASH);
        else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return rdbSaveType(rdb,RDB_TYPE_STREAM_LISTPACKS);
    case OBJ_MODULE:
        return rdbSaveType(rdb,RDB_TYPE_MODULE_2);
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the type is not specifically a valid Object Type. */
/* 从rdb文件中读取一个字节空间大小的对象类型标识 */
int rdbLoadObjectType(rio *rdb) {
    int type;
	//读取一个空间的对象类型标识
    if ((type = rdbLoadType(rdb)) == -1) 
		return -1;
	//检测获取的类型标识是否在满足的范围内
    if (!rdbIsObjectType(type)) 
		return -1;
	//返回对应的值对象的类型标识
    return type;
}

/* This helper function serializes a consumer group Pending Entries List (PEL)
 * into the RDB file. The 'nacks' argument tells the function if also persist
 * the informations about the not acknowledged message, or if to persist
 * just the IDs: this is useful because for the global consumer group PEL
 * we serialized the NACKs as well, but when serializing the local consumer
 * PELs we just add the ID, that will be resolved inside the global PEL to put a reference to the same structure. */
ssize_t rdbSaveStreamPEL(rio *rdb, rax *pel, int nacks) {
    ssize_t n, nwritten = 0;

    /* Number of entries in the PEL. */
    if ((n = rdbSaveLen(rdb,raxSize(pel))) == -1) 
		return -1;
    nwritten += n;

    /* Save each entry. */
    raxIterator ri;
    raxStart(&ri,pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        /* We store IDs in raw form as 128 big big endian numbers, like
         * they are inside the radix tree key. */
        if ((n = rdbWriteRaw(rdb,ri.key,sizeof(streamID))) == -1) 
			return -1;
        nwritten += n;

        if (nacks) {
            streamNACK *nack = ri.data;
            if ((n = rdbSaveMillisecondTime(rdb,nack->delivery_time)) == -1)
                return -1;
            nwritten += n;
            if ((n = rdbSaveLen(rdb,nack->delivery_count)) == -1) 
				return -1;
            nwritten += n;
            /* We don't save the consumer name: we'll save the pending IDs
             * for each consumer in the consumer PEL, and resolve the consumer
             * at loading time. */
        }
    }
    raxStop(&ri);
    return nwritten;
}

/* Serialize the consumers of a stream consumer group into the RDB. Helper
 * function for the stream data type serialization. What we do here is to
 * persist the consumer metadata, and it's PEL, for each consumer. */
size_t rdbSaveStreamConsumers(rio *rdb, streamCG *cg) {
    ssize_t n, nwritten = 0;

    /* Number of consumers in this consumer group. */
    if ((n = rdbSaveLen(rdb,raxSize(cg->consumers))) == -1) 
		return -1;
    nwritten += n;

    /* Save each consumer. */
    raxIterator ri;
    raxStart(&ri,cg->consumers);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamConsumer *consumer = ri.data;

        /* Consumer name. */
        if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) 
			return -1;
        nwritten += n;

        /* Last seen time. */
        if ((n = rdbSaveMillisecondTime(rdb,consumer->seen_time)) == -1)
            return -1;
        nwritten += n;

        /* Consumer PEL, without the ACKs (see last parameter of the function
         * passed with value of 0), at loading time we'll lookup the ID
         * in the consumer group global PEL and will put a reference in the
         * consumer local PEL. */
        if ((n = rdbSaveStreamPEL(rdb,consumer->pel,0)) == -1)
            return -1;
        nwritten += n;
    }
    raxStop(&ri);
    return nwritten;
}

/* Save a Redis object. Returns -1 on error, number of bytes written on success. */
/* 核心处理操作 主要是分析值对象的类型 然后根据值对象编码将对应的数据到rdb文件中的操作处理 */
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key) {
    ssize_t n = 0, nwritten = 0;

    if (o->type == OBJ_STRING) {
        /* Save a string value */
		//存储字符串对象的操作处理
		//触发存储字符串对象操作处理
        if ((n = rdbSaveStringObject(rdb,o)) == -1) 
			return -1;
		//统计写入的字节数量
        nwritten += n;
    } else if (o->type == OBJ_LIST) {
        /* Save a list value */
		//存储列表对象的操作 
		//首先检测是否是快速列表形式
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
			//获取值对象的quicklist结构实现
            quicklist *ql = o->ptr;
			//获取对应的quicklist结构的头结点链表
            quicklistNode *node = ql->head;
			//首先写入总的列表元素数量值
            if ((n = rdbSaveLen(rdb,ql->len)) == -1) 
				return -1;
            nwritten += n;
			//循环处理各个链表节点
            while(node) {
				//检测当前节点的数据是否是压缩结构
                if (quicklistNodeIsCompressed(node)) {
                    void *data;
					//获取压缩数据的位置指向 同时获取未压缩前总的字节数
                    size_t compress_len = quicklistGetLzf(node, &data);
					//尝试对压缩数据进行存储处理
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) 
						return -1;
                    nwritten += n;
                } else {
					//对未压缩的节点数据进行存储               能够这样处理的原因是 ziplist开辟的空间是连续的
                    if ((n = rdbSaveRawString(rdb,node->zl,node->sz)) == -1) 
						return -1;
                    nwritten += n;
                }
				//处理下一个链表节点
                node = node->next;
            }
        } else {
        	//编码类型不正确
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        /* Save a set value */
		//存储集合对象的操作 
        if (o->encoding == OBJ_ENCODING_HT) {
			//获取对应的字典结构
            dict *set = o->ptr;
			//获取对应的迭代器
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;
			//写入字典元素的数量值
            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;
			//循环字典结构中的所有节点
            while((de = dictNext(di)) != NULL) {
                sds ele = dictGetKey(de);
				//此处只写入了对应的字段字符串数据
                if ((n = rdbSaveRawString(rdb,(unsigned char*)ele,sdslen(ele)))  == -1) {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
			//写入完成释放对应的迭代器
            dictReleaseIterator(di);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
			//获取对应的整数集合占据的空间字节数
            size_t l = intsetBlobLen((intset*)o->ptr);
			//写入整数集合中的数据到rdb文件 此处整数集合的空间也是连续空间
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) 
				return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value */
		//存储有序集合对象的操作 
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
			//获取压缩列表的总的字节数量
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);
			//以字符串的方式写入对应的压缩列表数据
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) 
				return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            zskiplist *zsl = zs->zsl;
            if ((n = rdbSaveLen(rdb,zsl->length)) == -1) 
				return -1;
            nwritten += n;

            /* We save the skiplist elements from the greatest to the smallest
             * (that's trivial since the elements are already ordered in the
             * skiplist): this improves the load process, since the next loaded
             * element will always be the smaller, so adding to the skiplist
             * will always immediately stop at the head, making the insertion
             * O(1) instead of O(log(N)). */
            zskiplistNode *zn = zsl->tail;
            while (zn != NULL) {
                if ((n = rdbSaveRawString(rdb, (unsigned char*)zn->ele,sdslen(zn->ele))) == -1) {
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                    return -1;
                nwritten += n;
                zn = zn->backward;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value */
		//存储Hash对象的操作 
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
			//获取压缩列表的总的字节数量
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);
			//以字符串的方式写入对应的压缩列表数据
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1)
				return -1;
            nwritten += n;

        } else if (o->encoding == OBJ_ENCODING_HT) {
			//获取对应的迭代器
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;
			//写入对应的元素数量值
            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;
			//循环遍历所有的字段和值
            while((de = dictNext(di)) != NULL) {
				//获取对应的字段
                sds field = dictGetKey(de);
				//获取对应的值
                sds value = dictGetVal(de);
				//存储对应的字段字符串数据
                if ((n = rdbSaveRawString(rdb,(unsigned char*)field, sdslen(field))) == -1) {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
				//存储对应的值字符串数据
                if ((n = rdbSaveRawString(rdb,(unsigned char*)value, sdslen(value))) == -1) {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
			//释放对应的迭代器
            dictReleaseIterator(di);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        /* Store how many listpacks we have inside the radix tree. */
		//存储流对象的操作 
        stream *s = o->ptr;
        rax *rax = s->rax;
        if ((n = rdbSaveLen(rdb,raxSize(rax))) == -1) 
			return -1;
        nwritten += n;

        /* Serialize all the listpacks inside the radix tree as they are, when loading back, we'll use the first entry of each listpack to insert it back into the radix tree. */
        raxIterator ri;
        raxStart(&ri,rax);
        raxSeek(&ri,"^",NULL,0);
        while (raxNext(&ri)) {
            unsigned char *lp = ri.data;
            size_t lp_bytes = lpBytes(lp);
            if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) 
				return -1;
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lp_bytes)) == -1) 
				return -1;
            nwritten += n;
        }
        raxStop(&ri);

        /* Save the number of elements inside the stream. We cannot obtain
         * this easily later, since our macro nodes should be checked for number of items: not a great CPU / space tradeoff. */
        if ((n = rdbSaveLen(rdb,s->length)) == -1) 
			return -1;
        nwritten += n;
        /* Save the last entry ID. */
        if ((n = rdbSaveLen(rdb,s->last_id.ms)) == -1) 
			return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->last_id.seq)) == -1) 
			return -1;
        nwritten += n;

        /* The consumer groups and their clients are part of the stream type, so serialize every consumer group. */

        /* Save the number of groups. */
        size_t num_cgroups = s->cgroups ? raxSize(s->cgroups) : 0;
        if ((n = rdbSaveLen(rdb,num_cgroups)) == -1) 
			return -1;
        nwritten += n;

        if (num_cgroups) {
            /* Serialize each consumer group. */
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;

                /* Save the group name. */
                if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1)
                    return -1;
                nwritten += n;

                /* Last ID. */
                if ((n = rdbSaveLen(rdb,cg->last_id.ms)) == -1) 
					return -1;
                nwritten += n;
                if ((n = rdbSaveLen(rdb,cg->last_id.seq)) == -1) 
					return -1;
                nwritten += n;

                /* Save the global PEL. */
                if ((n = rdbSaveStreamPEL(rdb,cg->pel,1)) == -1) 
					return -1;
                nwritten += n;

                /* Save the consumers of this group. */
                if ((n = rdbSaveStreamConsumers(rdb,cg)) == -1) 
					return -1;
                nwritten += n;
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        /* Save a module-specific value. */
		//存储模块对象的操作 
        RedisModuleIO io;
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;

        /* Write the "module" identifier as prefix, so that we'll be able to call the right module during loading. */
        int retval = rdbSaveLen(rdb,mt->id);
        if (retval == -1) 
			return -1;
        io.bytes += retval;

        /* Then write the module-specific representation + EOF marker. */
        moduleInitIOContext(io,mt,rdb,key);
        mt->rdb_save(&io,mv->value);
        retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
        if (retval == -1)
            io.error = 1;
        else
            io.bytes += retval;

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }
        return io.error ? -1 : (ssize_t)io.bytes;
    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future we could switch to a faster solution. */
size_t rdbSavedObjectLen(robj *o) {
    ssize_t len = rdbSaveObject(NULL,o,NULL);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0 is returned (the key was already expired). */
/* 核心处理 将对应的键值对写入到rdb文件中的处理函数 */
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime) {
	//解析当前redis服务配置的内存策略 lru或者lfu
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* Save the expire time */
	//检测是否需要写入过期时间值
    if (expiretime != -1) {
		//写入过期时间标识
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) 
			return -1;
		//写入对应的过期时间值
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) 
			return -1;
    }

    /* Save the LRU info. */
	//存储对应的lru信息
    if (savelru) {
		//获取对应的空转时间值
        uint64_t idletime = estimateObjectIdleTime(val);
		//换算成对应的秒值
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
		//写入空转时间值标识
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) 
			return -1;
		//写入对应的空转时间值
        if (rdbSaveLen(rdb,idletime) == -1) 
			return -1;
    }

    /* Save the LFU info. */
	//存储对应的lfu信息
    if (savelfu) {
        uint8_t buf[1];
		//获取对应的次数值
        buf[0] = LFUDecrAndReturn(val);
        /* We can encode this in exactly two bytes: the opcode and an 8 bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it a single time when loading does not affect the frequency much. */
		//写入对应的lfu类型数据标识
		if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) 
			return -1;
		//写入次数值
        if (rdbWriteRaw(rdb,buf,1) == -1) 
			return -1;
    }

    /* Save type, key, value */
	//写入值对象类型标识
    if (rdbSaveObjectType(rdb,val) == -1) 
		return -1;
	//写入键对象所对应的数据
    if (rdbSaveStringObject(rdb,key) == -1) 
		return -1;
	//最为核心的操作 存储对应的值对象的操作处理
    if (rdbSaveObject(rdb,val,key) == -1) 
		return -1;
    return 1;
}

/* Save an AUX field. */
/* 触发向rdb文件中写入两个字符串数据 即键值对 */
ssize_t rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    ssize_t ret, len = 0;
	//写入数据类型是辅助信息类型
    if ((ret = rdbSaveType(rdb,RDB_OPCODE_AUX)) == -1) 
		return -1;
    len += ret;
	//写入对应的键字符串数据
    if ((ret = rdbSaveRawString(rdb,key,keylen)) == -1) 
		return -1;
    len += ret;
	//写入对应的值字符串数据
    if ((ret = rdbSaveRawString(rdb,val,vallen)) == -1) 
		return -1;
    len += ret;
	//返回写入的总的字节数量
    return len;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained with strlen(). */
/* 向rdb文件中写入一对字符串数据 */
ssize_t rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). */
/* 向rdb文件中写入一个字符串数据和整数数据 其实整数转换成对应的字符串数据 其实内部存储的还是一对字符串数据 */
ssize_t rdbSaveAuxFieldStrInt(rio *rdb, char *key, long long val) {
	//定义空间用于存储整数对应的字符串数据
    char buf[LONG_STR_SIZE];
	//获取整数对应的字符串数据
    int vlen = ll2string(buf,sizeof(buf),val);
	//将转换后的两个字符串数据写入rdb文件中
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. */
/* 在rdb文件中写入额外辅助信息 ( aux ) 辅助信息中包含了 Redis 的版本，内存占用和复制库( repl-id )和偏移量( repl-offset )等*/
int rdbSaveInfoAuxFields(rio *rdb, int flags, rdbSaveInfo *rsi) {
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;
    int aof_preamble = (flags & RDB_SAVE_AOF_PREAMBLE) != 0;

    /* Add a few fields about the state when the RDB was created. */
	//写入redis的版本值信息
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) 
		return -1;
	//写入当前系统对应的void* 占据的字节数量
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) 
		return -1;
	//写入当前的时间值
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) 
		return -1;
	//写入当前使用的内存值
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) 
		return -1;

    /* Handle saving options that generate aux fields. */
    if (rsi) {
		//
        if (rdbSaveAuxFieldStrInt(rdb,"repl-stream-db",rsi->repl_stream_db) == -1) 
			return -1;
		//
        if (rdbSaveAuxFieldStrStr(rdb,"repl-id",server.replid) == -1) 
			return -1;
		//
        if (rdbSaveAuxFieldStrInt(rdb,"repl-offset",server.master_repl_offset) == -1) 
			return -1;
    }
	//
    if (rdbSaveAuxFieldStrInt(rdb,"aof-preamble",aof_preamble) == -1) 
		return -1;
	
	//写入相关数据之后,返回写入成功标识
    return 1;
}

ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt) {
    /* Save a module-specific aux value. */
    RedisModuleIO io;
    int retval = rdbSaveType(rdb, RDB_OPCODE_MODULE_AUX);

    /* Write the "module" identifier as prefix, so that we'll be able to call the right module during loading. */
    retval = rdbSaveLen(rdb,mt->id);
    if (retval == -1) 
		return -1;
    io.bytes += retval;

    /* write the 'when' so that we can provide it on loading. add a UINT opcode for backwards compatibility, everything after the MT needs to be prefixed by an opcode. */
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_UINT);
    if (retval == -1) 
		return -1;
    io.bytes += retval;
    retval = rdbSaveLen(rdb,when);
    if (retval == -1) 
		return -1;
    io.bytes += retval;

    /* Then write the module-specific representation + EOF marker. */
    moduleInitIOContext(io,mt,rdb,NULL);
    mt->aux_save(&io,when);
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
    if (retval == -1)
        io.error = 1;
    else
        io.bytes += retval;

    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    if (io.error)
        return -1;
    return io.bytes;
}

/* Produces a dump of the database in RDB format sending it to the specified Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the integer pointed by 'error' is set to the value of errno just after the I/O error. */
/* rdb文件备份的核心处理流程 */
int rdbSaveRio(rio *rdb, int *error, int flags, rdbSaveInfo *rsi) {
    dictIterator *di = NULL;
    dictEntry *de;
    char magic[10];
    int j;
    uint64_t cksum;
    size_t processed = 0;

	//检测是否开启了校验和选项
    if (server.rdb_checksum)
		//设置校验和的函数
        rdb->update_cksum = rioGenericUpdateChecksum;
	//将Redis版本信息保存到magic中
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);

	//在rdb文件中先写入 REDIS 魔法值，然后是 RDB 文件的版本( rdb_version )
    if (rdbWriteRaw(rdb,magic,9) == -1) 
		goto werr;

	//再次写入额外辅助信息 ( aux ) 辅助信息中包含了 Redis 的版本，内存占用和复制库( repl-id )和偏移量( repl-offset )等
    if (rdbSaveInfoAuxFields(rdb,flags,rsi) == -1) 
		goto werr;
	
    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_BEFORE_RDB) == -1) 
		goto werr;
	
	//遍历redis中所有库中的数据,进行数据备份操作处理
    for (j = 0; j < server.dbnum; j++) {
		//获取当前索引对应的库
        redisDb *db = server.db+j;
		//获取当前库对应的数据集
        dict *d = db->dict;
		//检测当前库中是否有数据需要存储处理
        if (dictSize(d) == 0) 
			continue;
		//获取对应的安全迭代器对象
        di = dictGetSafeIterator(d);

        /* Write the SELECT DB opcode */
		//写入选择索引数据库标识类型
        if (rdbSaveType(rdb,RDB_OPCODE_SELECTDB) == -1) 
			goto werr;
		
		//写入对应的数据库索引值
        if (rdbSaveLen(rdb,j) == -1) 
			goto werr;

        /* Write the RESIZE DB opcode. We trim the size to UINT32_MAX, which is currently the largest type we are able to represent in RDB sizes.
         * However this does not limit the actual size of the DB to load since these sizes are just hints to resize the hash tables. */
        uint64_t db_size, expires_size;

		//获取字典元素数量
        db_size = dictSize(db->dict);
		//获取配置过期时间的字典元素数量
        expires_size = dictSize(db->expires);

		//写入字典和过期字典元素数量类型标识
        if (rdbSaveType(rdb,RDB_OPCODE_RESIZEDB) == -1) 
			goto werr;

		//写入字典元素数量值
        if (rdbSaveLen(rdb,db_size) == -1) 
			goto werr;

		//写入过期字典元素数量值
        if (rdbSaveLen(rdb,expires_size) == -1) 
			goto werr;

        /* Iterate this DB writing every entry */
		//循环检测对应的字典中的数据
        while((de = dictNext(di)) != NULL) {
			//获取对应的键结构sds
            sds keystr = dictGetKey(de);
			//获取对应的值对象
            robj key, *o = dictGetVal(de);
            long long expire;

			//重新初始化一个对应的键对象
            initStaticStringObject(key,keystr);
			
			//获取键对应的过期时间值
            expire = getExpire(db,&key);
			//触发将对应的键值对的数据写入到rdb文件中
            if (rdbSaveKeyValuePair(rdb,&key,o,expire) == -1) 
				goto werr;

            /* When this RDB is produced as part of an AOF rewrite, move accumulated diff from parent to child while rewriting in order to have a smaller final write. */
			//检测当前是否处于rdb和aof混合模式 同时检测是否满足对应的处理差值
            if (flags & RDB_SAVE_AOF_PREAMBLE && rdb->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES) {
				//重新记录对应的处理字节数
                processed = rdb->processed_bytes;
				//将对应的父进程中的数据追加处理的操作命令通过对应的管道接收到子进程中进行存储
                aofReadDiffFromParent();
            }
        }
		//循环完成释放对应的迭代器空间
        dictReleaseIterator(di);
		//置空迭代器指向
        di = NULL; /* So that we don't release it again on error. */
    }

    /* If we are storing the replication information on disk, persist the script cache as well: on successful PSYNC after a restart, we need
     * to be able to process any EVALSHA inside the replication backlog the master will send us. */
    //
    if (rsi && dictSize(server.lua_scripts)) {
		//获取对应的脚本字典迭代器
        di = dictGetIterator(server.lua_scripts);
		//循环遍历当前所有的脚本
        while((de = dictNext(di)) != NULL) {
			//获取对应的脚本值对象
            robj *body = dictGetVal(de);
			//以辅助信息的格式写入脚本值对象
            if (rdbSaveAuxField(rdb,"lua",3,body->ptr,sdslen(body->ptr)) == -1)
                goto werr;
        }
		//释放对应的迭代器
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. */
    }
	//
    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_AFTER_RDB) == -1) 
		goto werr;

    /* EOF opcode */
	//数据库数据设置完成之后,写入一个结束标记符
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) 
		goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the loading code skips the check in this case. */
	//通过rio对象获取对应的校验码
	cksum = rdb->cksum;
	/进行校验码值处理
    memrev64ifbe(&cksum);
	//将8字节校验码写入到文件的最后
    if (rioWrite(rdb,&cksum,8) == 0) 
		goto werr;
	//执行完成rdb备份操作处理成功标识
    return C_OK;

werr:
    if (error) 
		*error = errno;
    if (di) 
		dictReleaseIterator(di);
    return C_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends without doing any processing of the content. */
int rdbSaveRioWithEOFMark(rio *rdb, int *error, rdbSaveInfo *rsi) {
    char eofmark[RDB_EOF_MARK_SIZE];

    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) 
		*error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) 
		goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) 
		goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) 
		goto werr;
    if (rdbSaveRio(rdb,error,RDB_SAVE_NONE,rsi) == C_ERR) 
		goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) 
		goto werr;
    return C_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) 
		*error = errno;
    return C_ERR;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
/* 将数据库数据保存在磁盘上，返回C_OK成功，否则返回C_ERR */
int rdbSave(char *filename, rdbSaveInfo *rsi) {
	//定义临时存储数据的文件名称
    char tmpfile[256];
	//定义用于存储当前工作路径的buffer空间
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    FILE *fp;
	//创建于本次文件操作相关联的rio结构对象
    rio rdb;
    int error = 0;
	
	//拼接获取对应的备份临时文件的名称
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
	//以写方式打开临时文件
    fp = fopen(tmpfile,"w");
	//检测打开文件是否成功
    if (!fp) {
		//获取对应的文件目录
        char *cwdp = getcwd(cwd,MAXPATHLEN);
		//写入对应的错误信息日志
        serverLog(LL_WARNING,
            "Failed opening the RDB file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
		//返回执行rdb操作失败的错误标识
        return C_ERR;
    }

	//初始化一个rio对象，该对象是一个文件对象IO 即通过对应的文件句柄来完成rio结构的初始化
    rioInitWithFile(&rdb,fp);
	
	//检测服务配置参数是否启动了触发sync的操作处理
    if (server.rdb_save_incremental_fsync)
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES);

	//核心处理 将redis数据库中的数据写入rio中 即导入对应的临时存储文件中
    if (rdbSaveRio(&rdb,&error,RDB_SAVE_NONE,rsi) == C_ERR) {
        errno = error;
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
	//冲洗缓冲区，确保所有的数据都写入磁盘
    if (fflush(fp) == EOF) 
		goto werr;
	//将fp指向的文件同步到磁盘中
    if (fsync(fileno(fp)) == -1) 
		goto werr;
	//关闭文件
    if (fclose(fp) == EOF) 
		goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only if the generate DB file is ok. */
	//原子性改变rdb文件的名字
    if (rename(tmpfile,filename) == -1) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
		//删除对应的临时文件
        unlink(tmpfile);
		//返回rdb备份失败的标识
        return C_ERR;
    }
	
	//写入执行rdb操作成功的日志文件
    serverLog(LL_NOTICE,"DB saved on disk");
	//重置服务器的脏键
    server.dirty = 0;
	//更新上一次SAVE操作的时间
    server.lastsave = time(NULL);
	//更新SAVE操作的状态
    server.lastbgsave_status = C_OK;
	//返回rdb备份操作成功的标识
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    fclose(fp);
    unlink(tmpfile);
    return C_ERR;
}

/* 后台进行RDB持久化BGSAVE操作 */
int rdbSaveBackground(char *filename, rdbSaveInfo *rsi) {
    pid_t childpid;
    long long start;
	
	//当前没有正在进行AOF和RDB操作，否则返回C_ERR
    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) 
		return C_ERR;
	
	//备份当前数据库的脏键值 即备份前数据进行的脏数据值
    server.dirty_before_bgsave = server.dirty;
	//记录最近一个执行BGSAVE的时间
    server.lastbgsave_try = time(NULL);
	//初始化父子进程进行通信的管道信息
    openChildInfoPipe();
	//fork函数开始时间，记录fork函数的耗时
    start = ustime();
	//创建子进程
    if ((childpid = fork()) == 0) {
        int retval;

        /* Child */
		/*子进程中执行的代码*/
		//关闭监听的套接字
        closeListeningSockets(0);
		//设置进程标题，方便识别
        redisSetProcTitle("redis-rdb-bgsave");
		//执行保存操作，将数据库的写到filename文件中
        retval = rdbSave(filename,rsi);
		//检测是否保存成功
        if (retval == C_OK) {
			//得到子进程进程的私有虚拟页面大小，如果做RDB的同时父进程正在写入的数据，那么子进程就会拷贝一个份父进程的内存，而不是和父进程共享一份内存
            size_t private_dirty = zmalloc_get_private_dirty(-1);

            if (private_dirty) {
				//将子进程分配的内容写日志
                serverLog(LL_NOTICE, "RDB: %zu MB of memory used by copy-on-write", private_dirty/(1024*1024));
            }
			//记录在rdb过程中由于父进程键值对变化导致子进程使用私有虚拟页面的大小
            server.child_info_data.cow_size = private_dirty;
			//子进程向父进程发送信息
            sendChildInfo(CHILD_INFO_TYPE_RDB);
        }
		//子进程退出，发送信号给父进程，发送0表示BGSAVE成功，1表示失败
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
		/* 父进程中指向的代码 */
		//统计执行fork的时间值
        server.stat_fork_time = ustime()-start;
		//
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
		//
		latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
		//检测进行fork子进程是否成功
        if (childpid == -1) {
			//关闭对应的套接字
            closeChildInfoPipe();
			//设置记录本次执行rdb操作失败标识
            server.lastbgsave_status = C_ERR;
			//写入操作日志给系统
            serverLog(LL_WARNING, "Can't save in background: fork: %s", strerror(errno));
			//返回对应的执行错误标识
            return C_ERR;
        }
		//写入执行rdb操作的子进程pid日志
        serverLog(LL_NOTICE,"Background saving started by pid %d",childpid);
		//记录开启rdb备份操作处理的时间
        server.rdb_save_time_start = time(NULL);
		//记录对应的执行rdb操作的子进程pid值
        server.rdb_child_pid = childpid;
		//用于标识执行rdb操作输出的对象类型是磁盘文件 即进行rdb文件存储
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;
		//更新数据库字典数据不能进行改变尺寸操作处理 即不能进行扩容和缩容操作 主要原因是在rdb操作过程中尽可能使用拷贝复制 不要大批量的改变内存 造成内存开销
        updateDictResizePolicy();
		//返回对应的操作成功标识
        return C_OK;
    }
    return C_OK; /* unreached */
}

/* 根据给定的进程pid删除对应的临时文件 */
void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];
	//拼接对应的临时文件名称
    snprintf(tmpfile,sizeof(tmpfile),"temp-%d.rdb", (int) childpid);
	//删除对应的临时文件
    unlink(tmpfile);
}

/* This function is called by rdbLoadObject() when the code is in RDB-check
 * mode and we find a module value of type 2 that can be parsed without
 * the need of the actual module. The value is parsed for errors, finally
 * a dummy redis object is returned just to conform to the API. */
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename) {
    uint64_t opcode;
    while((opcode = rdbLoadLen(rdb,NULL)) != RDB_MODULE_OPCODE_EOF) {
        if (opcode == RDB_MODULE_OPCODE_SINT || opcode == RDB_MODULE_OPCODE_UINT) {
            uint64_t len;
            if (rdbLoadLenByRef(rdb,NULL,&len) == -1) {
                rdbExitReportCorruptRDB("Error reading integer from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            robj *o = rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
            if (o == NULL) {
                rdbExitReportCorruptRDB("Error reading string from module %s value", modulename);
            }
            decrRefCount(o);
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            float val;
            if (rdbLoadBinaryFloatValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB("Error reading float from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            double val;
            if (rdbLoadBinaryDoubleValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB("Error reading double from module %s value", modulename);
            }
        }
    }
    return createStringObject("module-dummy-value",18);
}

/* Load a Redis object of the specified type from the specified file. On success a newly allocated object is returned, otherwise NULL. */
robj *rdbLoadObject(int rdbtype, rio *rdb, robj *key) {
    robj *o = NULL, *ele, *dec;
    uint64_t len;
    unsigned int i;

    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) 
			return NULL;
        o = tryObjectEncoding(o);
    } else if (rdbtype == RDB_TYPE_LIST) {
        /* Read list value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
			return NULL;

        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size, server.list_compress_depth);

        /* Load every single element of the list */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) 
				return NULL;
            dec = getDecodedObject(ele);
            size_t len = sdslen(dec->ptr);
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read Set value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
			return NULL;

        /* Use a regular set when there are too many entries. */
        if (len > server.set_max_intset_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE)
                dictExpand(o->ptr,len);
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the set */
        for (i = 0; i < len; i++) {
            long long llval;
            sds sdsele;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            if (o->encoding == OBJ_ENCODING_INTSET) {
                /* Fetch integer value from element. */
                if (isSdsRepresentableAsLongLong(sdsele,&llval) == C_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                } else {
                    setTypeConvert(o,OBJ_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set. */
            if (o->encoding == OBJ_ENCODING_HT) {
                dictAdd((dict*)o->ptr,sdsele,NULL);
            } else {
                sdsfree(sdsele);
            }
        }
    } else if (rdbtype == RDB_TYPE_ZSET_2 || rdbtype == RDB_TYPE_ZSET) {
        /* Read list/set value. */
        uint64_t zsetlen;
        size_t maxelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
			return NULL;
        o = createZsetObject();
        zs = o->ptr;

        if (zsetlen > DICT_HT_INITIAL_SIZE)
            dictExpand(zs->dict,zsetlen);

        /* Load every single element of the sorted set. */
        while(zsetlen--) {
            sds sdsele;
            double score;
            zskiplistNode *znode;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) 
				return NULL;

            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) 
					return NULL;
            } else {
                if (rdbLoadDoubleValue(rdb,&score) == -1) 
					return NULL;
            }

            /* Don't care about integer-encoded strings. */
            if (sdslen(sdsele) > maxelelen) 
				maxelelen = sdslen(sdsele);

            znode = zslInsert(zs->zsl,score,sdsele);
            dictAdd(zs->dict,sdsele,&znode->score);
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        if (zsetLength(o) <= server.zset_max_ziplist_entries && maxelelen <= server.zset_max_ziplist_value)
            zsetConvert(o,OBJ_ENCODING_ZIPLIST);
    } else if (rdbtype == RDB_TYPE_HASH) {
        uint64_t len;
        int ret;
        sds field, value;

        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;

        o = createHashObject();

        /* Too many entries? Use a hash table. */
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);

        /* Load every field and value into the ziplist */
        while (o->encoding == OBJ_ENCODING_ZIPLIST && len > 0) {
            len--;
            /* Load raw strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) 
				return NULL;
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) 
				return NULL;

            /* Add pair to ziplist */
            o->ptr = ziplistPush(o->ptr, (unsigned char*)field, sdslen(field), ZIPLIST_TAIL);
            o->ptr = ziplistPush(o->ptr, (unsigned char*)value, sdslen(value), ZIPLIST_TAIL);

            /* Convert to hash table if size threshold is exceeded */
            if (sdslen(field) > server.hash_max_ziplist_value || sdslen(value) > server.hash_max_ziplist_value) {
                sdsfree(field);
                sdsfree(value);
                hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            }
            sdsfree(field);
            sdsfree(value);
        }

        if (o->encoding == OBJ_ENCODING_HT && len > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,len);

        /* Load remaining fields and values into the hash table */
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            len--;
            /* Load encoded strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) 
				return NULL;
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) 
				return NULL;

            /* Add pair to hash table */
            ret = dictAdd((dict*)o->ptr, field, value);
            if (ret == DICT_ERR) {
                rdbExitReportCorruptRDB("Duplicate keys detected");
            }
        }

        /* All pairs should be read by now */
        serverAssert(len == 0);
    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST) {
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
			return NULL;
        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size, server.list_compress_depth);

        while (len--) {
            unsigned char *zl = rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (zl == NULL) 
				return NULL;
            quicklistAppendZiplist(o->ptr, zl);
        }
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST) {
        unsigned char *encoded = rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
        if (encoded == NULL) 
			return NULL;
        o = createObject(OBJ_STRING,encoded); /* Obj type fixed below. */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        switch(rdbtype) {
            case RDB_TYPE_HASH_ZIPMAP:
                /* Convert to ziplist encoded hash. This must be deprecated when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries || maxlen > server.hash_max_ziplist_value){
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                }
                break;
            case RDB_TYPE_LIST_ZIPLIST:
                o->type = OBJ_LIST;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                listTypeConvert(o,OBJ_ENCODING_QUICKLIST);
                break;
            case RDB_TYPE_SET_INTSET:
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_ZSET_ZIPLIST:
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)
                    zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                break;
            case RDB_TYPE_HASH_ZIPLIST:
                o->type = OBJ_HASH;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)
                    hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            default:
                rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }
    } else if (rdbtype == RDB_TYPE_STREAM_LISTPACKS) {
        o = createStreamObject();
        stream *s = o->ptr;
        uint64_t listpacks = rdbLoadLen(rdb,NULL);

        while(listpacks--) {
            /* Get the master ID, the one we'll use as key of the radix tree
             * node: the entries inside the listpack itself are delta-encoded
             * relatively to this ID. */
            sds nodekey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (nodekey == NULL) {
                rdbExitReportCorruptRDB("Stream master ID loading failed: invalid encoding or I/O error.");
            }
            if (sdslen(nodekey) != sizeof(streamID)) {
                rdbExitReportCorruptRDB("Stream node key entry is not the "
                                        "size of a stream ID");
            }

            /* Load the listpack. */
            unsigned char *lp = rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (lp == NULL) 
				return NULL;
            unsigned char *first = lpFirst(lp);
            if (first == NULL) {
                /* Serialized listpacks should never be empty, since on deletion we should remove the radix tree key if the resulting listpack is empty. */
                rdbExitReportCorruptRDB("Empty listpack inside stream");
            }

            /* Insert the key in the radix tree. */
            int retval = raxInsert(s->rax, (unsigned char*)nodekey,sizeof(streamID),lp,NULL);
            sdsfree(nodekey);
            if (!retval)
                rdbExitReportCorruptRDB("Listpack re-added with existing key");
        }
        /* Load total number of items inside the stream. */
        s->length = rdbLoadLen(rdb,NULL);
        /* Load the last entry ID. */
        s->last_id.ms = rdbLoadLen(rdb,NULL);
        s->last_id.seq = rdbLoadLen(rdb,NULL);

        /* Consumer groups loading */
        size_t cgroups_count = rdbLoadLen(rdb,NULL);
        while(cgroups_count--) {
            /* Get the consumer group name and ID. We can then create the consumer group ASAP and populate its structure as we read more data. */
            streamID cg_id;
            sds cgname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (cgname == NULL) {
                rdbExitReportCorruptRDB("Error reading the consumer group name from Stream");
            }
            cg_id.ms = rdbLoadLen(rdb,NULL);
            cg_id.seq = rdbLoadLen(rdb,NULL);
            streamCG *cgroup = streamCreateCG(s,cgname,sdslen(cgname),&cg_id);
            if (cgroup == NULL)
                rdbExitReportCorruptRDB("Duplicated consumer group name %s", cgname);
            sdsfree(cgname);

            /* Load the global PEL for this consumer group, however we'll
             * not yet populate the NACK structures with the message
             * owner, since consumers for this group and their messages will
             * be read as a next step. So for now leave them not resolved
             * and later populate it. */
            size_t pel_size = rdbLoadLen(rdb,NULL);
            while(pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                rdbLoadRaw(rdb,rawid,sizeof(rawid));
                streamNACK *nack = streamCreateNACK(NULL);
                nack->delivery_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                nack->delivery_count = rdbLoadLen(rdb,NULL);
                if (!raxInsert(cgroup->pel,rawid,sizeof(rawid),nack,NULL))
                    rdbExitReportCorruptRDB("Duplicated gobal PEL entry "
                                            "loading stream consumer group");
            }

            /* Now that we loaded our global PEL, we need to load the
             * consumers and their local PELs. */
            size_t consumers_num = rdbLoadLen(rdb,NULL);
            while(consumers_num--) {
                sds cname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
                if (cname == NULL) {
                    rdbExitReportCorruptRDB("Error reading the consumer name from Stream group");
                }
                streamConsumer *consumer = streamLookupConsumer(cgroup,cname, 1);
                sdsfree(cname);
                consumer->seen_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);

                /* Load the PEL about entries owned by this specific
                 * consumer. */
                pel_size = rdbLoadLen(rdb,NULL);
                while(pel_size--) {
                    unsigned char rawid[sizeof(streamID)];
                    rdbLoadRaw(rdb,rawid,sizeof(rawid));
                    streamNACK *nack = raxFind(cgroup->pel,rawid,sizeof(rawid));
                    if (nack == raxNotFound)
                        rdbExitReportCorruptRDB("Consumer entry not found in "
                                                "group global PEL");

                    /* Set the NACK consumer, that was left to NULL when
                     * loading the global PEL. Then set the same shared
                     * NACK structure also in the consumer-specific PEL. */
                    nack->consumer = consumer;
                    if (!raxInsert(consumer->pel,rawid,sizeof(rawid),nack,NULL))
                        rdbExitReportCorruptRDB("Duplicated consumer PEL entry "
                                                " loading a stream consumer "
                                                "group");
                }
            }
        }
    } else if (rdbtype == RDB_TYPE_MODULE || rdbtype == RDB_TYPE_MODULE_2) {
        uint64_t moduleid = rdbLoadLen(rdb,NULL);
        moduleType *mt = moduleTypeLookupModuleByID(moduleid);
        char name[10];

        if (rdbCheckMode && rdbtype == RDB_TYPE_MODULE_2) {
            moduleTypeNameByID(name,moduleid);
            return rdbLoadCheckModuleValue(rdb,name);
        }

        if (mt == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data I can't load: no matching module '%s'", name);
            exit(1);
        }
        RedisModuleIO io;
        moduleInitIOContext(io,mt,rdb,key);
        io.ver = (rdbtype == RDB_TYPE_MODULE) ? 1 : 2;
        /* Call the rdb_load method of the module providing the 10 bit encoding version in the lower 10 bits of the module ID. */
        void *ptr = mt->rdb_load(&io,moduleid&1023);
        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        /* Module v2 serialization has an EOF mark at the end. */
        if (io.ver == 2) {
            uint64_t eof = rdbLoadLen(rdb,NULL);
            if (eof != RDB_MODULE_OPCODE_EOF) {
                serverLog(LL_WARNING,"The RDB file contains module data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                exit(1);
            }
        }

        if (ptr == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
            exit(1);
        }
        o = createModuleObject(mt,ptr);
    } else {
        rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields needed to provide loading stats. */
/* 用于标识redis服务正处于加载rdb文件的过程中的标识 */
void startLoading(FILE *fp) {
    struct stat sb;

    /* Load the DB */
	//给服务设置正在加载rdb数据中
    server.loading = 1;
	//设置加载rdb文件的开始时间
    server.loading_start_time = time(NULL);
	//初始化用于记录已加载rdb文件的字节数量
    server.loading_loaded_bytes = 0;
	//读取需要加载的文件总的字节数量
    if (fstat(fileno(fp), &sb) == -1) {
        server.loading_total_bytes = 0;
    } else {
    	//设置总的需要加载的文件字节数量 通常用于处理进度问题
        server.loading_total_bytes = sb.st_size;
    }
}

/* Refresh the loading progress info */
/* 用于刷新已加载的字节数量 */
void loadingProgress(off_t pos) {
	//更新已加载字节数量
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished */
/* 加载完成或者失败后 设置当前已经结束了加载中的标识 */
void stopLoading(void) {
    server.loading = 0;
}

/* Track loading progress in order to serve client's from time to time and if needed calculate rdb checksum  */
/* 在加载rdb数据过程中的相关回调处理 */
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
	//检测是否配置了处理效验码的操作处理函数
    if (server.rdb_checksum)
		//更新并获取最新的校验码值
        rioGenericUpdateChecksum(r, buf, len);
	//
    if (server.loading_process_events_interval_bytes && (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes) {
        /* The DB can take some non trivial amount of time to load. Update our cached time since it is used to create and update the last interaction time with clients and for other important things. */
		//
		updateCachedTime(0);
		//
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
			//
            replicationSendNewlineToMaster();
		//更新以读取rdb文件字节数
        loadingProgress(r->processed_bytes);
		//
        processEventsWhileBlocked();
    }
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned, otherwise C_ERR is returned and 'errno' is set accordingly. */
/* 加载rdb文件的核心处理流程函数 */
int rdbLoadRio(rio *rdb, rdbSaveInfo *rsi, int loading_aof) {
    uint64_t dbid;
    int type, rdbver;
	//初始化为操作第一个索引库
    redisDb *db = server.db+0;
	//用于记录数据的缓存buffer
    char buf[1024];

	//设置处理效验码处理函数
    rdb->update_cksum = rdbLoadProgressCallback;
	//设置加载一批数据的门限值 即分批加载多字节数量
    rdb->max_processing_chunk = server.loading_process_events_interval_bytes;
	//读取9字节数据
    if (rioRead(rdb,buf,9) == 0) 
		goto eoferr;
	//设置结束标识 目的是进行整数转换时需要结束符                        例如 设置结束标记位 "redis0009\0"
    buf[9] = '\0';
	//匹配前5个字符是不是REDIS
    if (memcmp(buf,"REDIS",5) != 0) {
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return C_ERR;
    }
	//获取对应的版本值
    rdbver = atoi(buf+5);
	//检测获取到的版本值是否在合法的范围内
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return C_ERR;
    }

    /* Key-specific attributes, set by opcodes before the key type. */
	//初始化过期时间和lru以及lfu相关的参数
    long long lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
    long long lru_clock = LRU_CLOCK();

	//解析rdb文件的核心处理流程 首先加载一个字节的标识位 然后分析标识位来进行后续的加载数据处理
    while(1) {
        robj *key, *val;

        /* Read type. */
		//首先加载一字节的标识位 此处是后续解析的基础 后续全是根据解析到的标识位进行处理的
        if ((type = rdbLoadType(rdb)) == -1) 
			goto eoferr;

        /* Handle special types. */
		//根据获取到的标识位来触发后续的解析行为
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key to load. Note that after loading an expire we need to load the actual type, and continue. */
			//标识为秒值过期时间 后续需要读取对应的时间值
			expiretime = rdbLoadTime(rdb);
			//转换成对应的毫秒值
            expiretime *= 1000;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced with RDB v3. Like EXPIRETIME but no with more precision. */
			//标识为秒值过期时间 后续需要读取对应的时间值
			expiretime = rdbLoadMillisecondTime(rdb,rdbver);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
			//标识为LFU的访问次数标识 后续需要读取对应的次数值
            uint8_t byte;
			//读取一字节的次数值
            if (rioRead(rdb,&byte,1) == 0) 
				goto eoferr;
			//记录对应的次数值
            lfu_freq = byte;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
			//标识为LRU的空转时间标识值
            uint64_t qword;
			//读取对应的空转值
            if ((qword = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
				goto eoferr;
			//记录对应的空转值
            lru_idle = qword;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
			//如果读到EOF，则直接跳出循环--------------------->此标记标识所有的需要加载的数据操作已经完成了
			//这个地方是唯一的退出while循环的出口
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
			//标识为读取到的是选择库的标识 那么后续就是一个索引值 即读取对应的索引值
            if ((dbid = rdbLoadLen(rdb,NULL)) == RDB_LENERR) 
				goto eoferr;
			//检测对应的索引值是否超过了当前redis服务的最大库索引值----->这个地方有一个问题 如果在一个机器生成了rdb文件 但是在另一台机器上对应的数据库最大索引值要小 那么利用rdb文件加载数据时就会抛出对应的错误信息
            if (dbid >= (unsigned)server.dbnum) {
				//配置对应的错误日志信息
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
				//退出redis服务
                exit(1);
            }
			//设置当前操作的数据库为对应的索引
            db = server.db+dbid;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently selected data base, in order to avoid useless rehashing. */
			//标识为数据库中元素数量的标识值
			uint64_t db_size, expires_size;
			//读取对应的字典元素数量值
            if ((db_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
			//读取对应的过期字典元素数量值
            if ((expires_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
			//对当前数据库的字典进行指定扩容操作处理
            dictExpand(db->dict,db_size);
			//对当前数据库的过期字典进行指定扩容操作处理
            dictExpand(db->expires,expires_size);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading are requierd to skip AUX fields they don't understand.
             * An AUX field is composed of two strings: key and value. */
			//标识为辅助信息的标识位              后续是两个字符串数据
			robj *auxkey, *auxval;
			//加载对应的字符串数据 字段部分
            if ((auxkey = rdbLoadStringObject(rdb)) == NULL) 
				goto eoferr;
			//加载对应的字符串数据 值部分
            if ((auxval = rdbLoadStringObject(rdb)) == NULL) 
				goto eoferr;
			
			//下面是分析对应的辅助信息的处理流程
            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered information fields and are logged at startup with a log level of NOTICE. */
                serverLog(LL_NOTICE,"RDB '%s': %s", (char*)auxkey->ptr, (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-stream-db")) {
                if (rsi) 
					rsi->repl_stream_db = atoi(auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-id")) {
                if (rsi && sdslen(auxval->ptr) == CONFIG_RUN_ID_SIZE) {
                    memcpy(rsi->repl_id,auxval->ptr,CONFIG_RUN_ID_SIZE+1);
                    rsi->repl_id_is_set = 1;
                }
            } else if (!strcasecmp(auxkey->ptr,"repl-offset")) {
                if (rsi) 
					rsi->repl_offset = strtoll(auxval->ptr,NULL,10);
            } else if (!strcasecmp(auxkey->ptr,"lua")) {
                /* Load the script back in memory. */
                if (luaCreateFunction(NULL,server.lua,auxval) == NULL) {
                    rdbExitReportCorruptRDB(
                        "Can't load Lua script from RDB file! "
                        "BODY: %s", auxval->ptr);
                }
            } else {
                /* We ignore fields we don't understand, as by AUX field contract. */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'", (char*)auxkey->ptr);
            }

			//使用完字符串对象之后进行释放操作处理
            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* Load module data that is not related to the Redis key space. Such data can be potentially be stored both before and after the RDB keys-values section. */
            uint64_t moduleid = rdbLoadLen(rdb,NULL);
            int when_opcode = rdbLoadLen(rdb,NULL);
            int when = rdbLoadLen(rdb,NULL);
            if (when_opcode != RDB_MODULE_OPCODE_UINT)
                rdbExitReportCorruptRDB("bad when_opcode");
            moduleType *mt = moduleTypeLookupModuleByID(moduleid);
            char name[10];
            moduleTypeNameByID(name,moduleid);

            if (!rdbCheckMode && mt == NULL) {
                /* Unknown module. */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load: no matching module '%s'", name);
                exit(1);
            } else if (!rdbCheckMode && mt != NULL) {
                if (!mt->aux_load) {
                    /* Module doesn't support AUX. */
                    serverLog(LL_WARNING,"The RDB file contains module AUX data, but the module '%s' doesn't seem to support it.", name);
                    exit(1);
                }

                RedisModuleIO io;
                moduleInitIOContext(io,mt,rdb,NULL);
                io.ver = 2;
                /* Call the rdb_load method of the module providing the 10 bit encoding version in the lower 10 bits of the module ID. */
                if (mt->aux_load(&io,moduleid&1023, when) || io.error) {
                    moduleTypeNameByID(name,moduleid);
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
                    exit(1);
                }
                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }
                uint64_t eof = rdbLoadLen(rdb,NULL);
                if (eof != RDB_MODULE_OPCODE_EOF) {
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                    exit(1);
                }
                continue;
            } else {
                /* RDB check mode. */
                robj *aux = rdbLoadCheckModuleValue(rdb,name);
                decrRefCount(aux);
                continue; /* Read next opcode. */
            }
        }

        /* Read key */
        if ((key = rdbLoadStringObject(rdb)) == NULL) 
			goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,rdb,key)) == NULL) 
			goto eoferr;
        /* Check if the key already expired. This function is used when loading an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave. */
        if (server.masterhost == NULL && !loading_aof && expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
        } else {
            /* Add the new object in the hash table */
            dbAdd(db,key,val);

            /* Set the expire time if needed */
            if (expiretime != -1) 
				setExpire(NULL,db,key,expiretime);
            
            /* Set usage information (for eviction). */
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock);

            /* Decrement the key refcount since dbAdd() will take its own reference. */
            decrRefCount(key);
        }

        /* Reset the state that is key-specified and is populated by opcodes before the key, so that we start from scratch again. */
        expiretime = -1;
        lfu_freq = -1;
        lru_idle = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
	//检测当前的版本值 对应5版本及其以上有对应的效应码
    if (rdbver >= 5) {
		//获取读取出来的数据对应的效应码 从这里可以发现 校验码的值不参与进一步效验
        uint64_t cksum, expected = rdb->cksum;
		//读取rdb文件中存储的8字节效验码值
        if (rioRead(rdb,&cksum,8) == 0) 
			goto eoferr;
		//检测redis服务器是否配置了校验码出来函数
        if (server.rdb_checksum) {
			//对8字节校验码
            memrev64ifbe(&cksum);
			//检测校验码值是否可用
            if (cksum == 0) {
				//记录日志为效验码不可用
                serverLog(LL_WARNING,"RDB file was saved with checksum disabled: no check performed.");
            } else if (cksum != expected) {
            	//记录日志为读取的效应码和计算的效应码不匹配
                serverLog(LL_WARNING,"Wrong RDB checksum. Aborting now.");
				//触发停止redis服务操作处理
                rdbExitReportCorruptRDB("RDB CRC error");
            }
        }
    }
    return C_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    serverLog(LL_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    rdbExitReportCorruptRDB("Unexpected EOF reading RDB file");
    return C_ERR; /* Just to avoid warning */
}

/* Like rdbLoadRio() but takes a filename instead of a rio stream. The filename is open for reading and a rio stream object created in order
 * to do the actual loading. Moreover the ETA displayed in the INFO output is initialized and finalized.
 * If you pass an 'rsi' structure initialied with RDB_SAVE_OPTION_INIT, the loading code will fiil the information fields in the structure. */
int rdbLoad(char *filename, rdbSaveInfo *rsi) {
    FILE *fp;
    rio rdb;
    int retval;

	//检测是否可以打开对应的rdb数据文件
    if ((fp = fopen(filename,"r")) == NULL) 
		//不能读取文件 返回对应的错误
		return C_ERR;
	//设置开始加载rdb数据的启动标识
    startLoading(fp);
	//根据对应的文件初始化rdb结构
    rioInitWithFile(&rdb,fp);
	//开始启动加载rdb数据文件的处理操作 
    retval = rdbLoadRio(&rdb,rsi,0);
	//关闭对应的文件
    fclose(fp);
	//设置结束加载rdb文件的结束标识
    stopLoading();
	//返回是否加载rdb文件是否成功标识
    return retval;
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = C_OK;
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        server.lastbgsave_status = C_ERR;
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.rdb_child_pid);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error condition. */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_DISK);
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Salves socket transfers for
 * diskless replication. */
void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    uint64_t *ok_slaves;

    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");
    } else {
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_start = -1;

    /* If the child returns an OK exit code, read the set of slave client
     * IDs and the associated status code. We'll terminate all the slaves
     * in error state.
     *
     * If the process returned an error, consider the list of slaves that
     * can continue to be empty, so that it's just a special case of the
     * normal code path. */
    ok_slaves = zmalloc(sizeof(uint64_t)); /* Make space for the count. */
    ok_slaves[0] = 0;
    if (!bysignal && exitcode == 0) {
        int readlen = sizeof(uint64_t);

        if (read(server.rdb_pipe_read_result_from_child, ok_slaves, readlen) ==
                 readlen)
        {
            readlen = ok_slaves[0]*sizeof(uint64_t)*2;

            /* Make space for enough elements as specified by the first
             * uint64_t element in the array. */
            ok_slaves = zrealloc(ok_slaves,sizeof(uint64_t)+readlen);
            if (readlen &&
                read(server.rdb_pipe_read_result_from_child, ok_slaves+1,
                     readlen) != readlen)
            {
                ok_slaves[0] = 0;
            }
        }
    }

    close(server.rdb_pipe_read_result_from_child);
    close(server.rdb_pipe_write_result_to_parent);

    /* We can continue the replication process with all the slaves that
     * correctly received the full payload. Others are terminated. */
    listNode *ln;
    listIter li;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            uint64_t j;
            int errorcode = 0;

            /* Search for the slave ID in the reply. In order for a slave to
             * continue the replication process, we need to find it in the list,
             * and it must have an error code set to 0 (which means success). */
            for (j = 0; j < ok_slaves[0]; j++) {
                if (slave->id == ok_slaves[2*j+1]) {
                    errorcode = ok_slaves[2*j+2];
                    break; /* Found in slaves list. */
                }
            }
            if (j == ok_slaves[0] || errorcode != 0) {
                serverLog(LL_WARNING,
                "Closing slave %s: child->slave RDB transfer failed: %s",
                    replicationGetSlaveName(slave),
                    (errorcode == 0) ? "RDB transfer child aborted"
                                     : strerror(errorcode));
                freeClient(slave);
            } else {
                serverLog(LL_WARNING,
                "Slave %s correctly received the streamed RDB file.",
                    replicationGetSlaveName(slave));
                /* Restore the socket as non-blocking. */
                anetNonBlock(NULL,slave->fd);
                anetSendTimeout(NULL,slave->fd,0);
            }
        }
    }
    zfree(ok_slaves);

    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_SOCKET);
}

/* When a background RDB saving/transfer terminates, call the right handler. */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    switch(server.rdb_child_type) {
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. */
int rdbSaveToSlavesSockets(rdbSaveInfo *rsi) {
    int *fds;
    uint64_t *clientids;
    int numfds;
    listNode *ln;
    listIter li;
    pid_t childpid;
    long long start;
    int pipefds[2];

    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;

    /* Before to fork, create a pipe that will be used in order to
     * send back to the parent the IDs of the slaves that successfully
     * received all the writes. */
    if (pipe(pipefds) == -1) return C_ERR;
    server.rdb_pipe_read_result_from_child = pipefds[0];
    server.rdb_pipe_write_result_to_parent = pipefds[1];

    /* Collect the file descriptors of the slaves we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    fds = zmalloc(sizeof(int)*listLength(server.slaves));
    /* We also allocate an array of corresponding client IDs. This will
     * be useful for the child process in order to build the report
     * (sent via unix pipe) that will be sent to the parent. */
    clientids = zmalloc(sizeof(uint64_t)*listLength(server.slaves));
    numfds = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            clientids[numfds] = slave->id;
            fds[numfds++] = slave->fd;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
            /* Put the socket in blocking mode to simplify RDB transfer.
             * We'll restore it when the children returns (since duped socket
             * will share the O_NONBLOCK attribute with the parent). */
            anetBlock(NULL,slave->fd);
            anetSendTimeout(NULL,slave->fd,server.repl_timeout*1000);
        }
    }

    /* Create the child process. */
    openChildInfoPipe();
    start = ustime();
    if ((childpid = fork()) == 0) {
        /* Child */
        int retval;
        rio slave_sockets;

        rioInitWithFdset(&slave_sockets,fds,numfds);
        zfree(fds);

        closeListeningSockets(0);
        redisSetProcTitle("redis-rdb-to-slaves");

        retval = rdbSaveRioWithEOFMark(&slave_sockets,NULL,rsi);
        if (retval == C_OK && rioFlush(&slave_sockets) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            size_t private_dirty = zmalloc_get_private_dirty(-1);

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            server.child_info_data.cow_size = private_dirty;
            sendChildInfo(CHILD_INFO_TYPE_RDB);

            /* If we are returning OK, at least one slave was served
             * with the RDB file as expected, so we need to send a report
             * to the parent via the pipe. The format of the message is:
             *
             * <len> <slave[0].id> <slave[0].error> ...
             *
             * len, slave IDs, and slave errors, are all uint64_t integers,
             * so basically the reply is composed of 64 bits for the len field
             * plus 2 additional 64 bit integers for each entry, for a total
             * of 'len' entries.
             *
             * The 'id' represents the slave's client ID, so that the master
             * can match the report with a specific slave, and 'error' is
             * set to 0 if the replication process terminated with a success
             * or the error code if an error occurred. */
            void *msg = zmalloc(sizeof(uint64_t)*(1+2*numfds));
            uint64_t *len = msg;
            uint64_t *ids = len+1;
            int j, msglen;

            *len = numfds;
            for (j = 0; j < numfds; j++) {
                *ids++ = clientids[j];
                *ids++ = slave_sockets.io.fdset.state[j];
            }

            /* Write the message to the parent. If we have no good slaves or
             * we are unable to transfer the message to the parent, we exit
             * with an error so that the parent will abort the replication
             * process with all the childre that were waiting. */
            msglen = sizeof(uint64_t)*(1+2*numfds);
            if (*len == 0 ||
                write(server.rdb_pipe_write_result_to_parent,msg,msglen)
                != msglen)
            {
                retval = C_ERR;
            }
            zfree(msg);
        }
        zfree(clientids);
        rioFreeFdset(&slave_sockets);
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                int j;

                for (j = 0; j < numfds; j++) {
                    if (slave->id == clientids[j]) {
                        slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
                        break;
                    }
                }
            }
            close(pipefds[0]);
            close(pipefds[1]);
            closeChildInfoPipe();
        } else {
            server.stat_fork_time = ustime()-start;
            server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
            latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);

            serverLog(LL_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            updateDictResizePolicy();
        }
        zfree(clientids);
        zfree(fds);
        return (childpid == -1) ? C_ERR : C_OK;
    }
    return C_OK; /* Unreached. */
}

/*
 * 执行一个同步保存操作，将当前 Redis 实例的所有数据快照(snapshot)以 RDB 文件的形式保存到硬盘
 * 命令格式
 *     SAVE
 * 返回值
 *     保存成功时返回 OK
 */
void saveCommand(client *c) {
	//检测当前是否处于rdb备份过程中
    if (server.rdb_child_pid != -1) {
		//向客户端响应整处于rdb备份中的提示信息
        addReplyError(c,"Background save already in progress");
		//直接返回不再进行相关处理
        return;
    }
    rdbSaveInfo rsi, *rsiptr;
	//
    rsiptr = rdbPopulateSaveInfo(&rsi);
	
	//执行对应的rdb备份操作处理,并检测是否执行成功
    if (rdbSave(server.rdb_filename,rsiptr) == C_OK) {
		//保存成功向客户端响应成功标识
        addReply(c,shared.ok);
    } else {
    	//保存失败向客户端响应失败标识
        addReply(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] */
/*
 * 用于在后台异步保存当前数据库的数据到磁盘
 *     BGSAVE 命令执行之后立即返回 OK ，然后 Redis fork 出一个新子进程，原来的 Redis 进程(父进程)继续处理客户端请求，而子进程则负责将数据保存到磁盘，然后退出。
 * 命令格式
 *     BGSAVE [SCHEDULE]
 * 返回值
 *     反馈信息
 */
void bgsaveCommand(client *c) {
    int schedule = 0;

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite is in progress. Instead of returning an error a BGSAVE gets scheduled. */
	//检测客户端传入的参数是否正确
	if (c->argc > 1) {
		//多参数时 检测是否是对应的schedule参数
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
			//设置schedule标志
            schedule = 1;
        } else {
        	//向客户端响应参数配置错误的响应结果
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
	//检测当前是否处于rdb备份中
    if (server.rdb_child_pid != -1) {
		//向客户端响应对应的提示信息 处于备份过程中 不能再次启动备份操作处理
        addReplyError(c,"Background save already in progress");
    } else if (server.aof_child_pid != -1) { //检测当前是否处于aof重写备份中
    	//检测是否设置任务标识位,即等待执行完aof操作之后,进行对应的rdb备份操作处理
        if (schedule) {
			//设置rdb_bgsave_scheduled为1，表示可以执行BGSAVE
            server.rdb_bgsave_scheduled = 1;
			//向客户端响应对应的提示信息
            addReplyStatus(c,"Background saving scheduled");
        } else {
			//向客户端响应正在处于aof重写备份过程中,不能启动后台rdb备份操作处理的提示
            addReplyError(c,
                "An AOF log rewriting in progress: can't BGSAVE right now. "
                "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
                "possible.");
        }
    } else if (rdbSaveBackground(server.rdb_filename,rsiptr) == C_OK) { //触发执行后台rdb备份操作处理
    	//向客户端响应开始启动后台备份操作处理
        addReplyStatus(c,"Background saving started");
    } else {
		//向客户端响应进行启动后台备份操作处理错误
        addReply(c,shared.err);
    }
}

/* Populate the rdbSaveInfo structure used to persist the replication
 * information inside the RDB file. Currently the structure explicitly
 * contains just the currently selected DB from the master stream, however
 * if the rdbSave*() family functions receive a NULL rsi structure also
 * the Replication ID/offset is not saved. The function popultes 'rsi'
 * that is normally stack-allocated in the caller, returns the populated
 * pointer if the instance has a valid master client, otherwise NULL
 * is returned, and the RDB saving will not persist any replication related information. */
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi) {
    rdbSaveInfo rsi_init = RDB_SAVE_INFO_INIT;
    *rsi = rsi_init;

    /* If the instance is a master, we can populate the replication info
     * only when repl_backlog is not NULL. If the repl_backlog is NULL,
     * it means that the instance isn't in any replication chains. In this
     * scenario the replication info is useless, because when a slave
     * connects to us, the NULL repl_backlog will trigger a full
     * synchronization, at the same time we will use a new replid and clear replid2. */
    if (!server.masterhost && server.repl_backlog) {
        /* Note that when server.slaveseldb is -1, it means that this master
         * didn't apply any write commands after a full synchronization.
         * So we can let repl_stream_db be 0, this allows a restarted slave
         * to reload replication ID/offset, it's safe because the next write command must generate a SELECT statement. */
        rsi->repl_stream_db = server.slaveseldb == -1 ? 0 : server.slaveseldb;
        return rsi;
    }

    /* If the instance is a slave we need a connected master in order to fetch the currently selected DB. */
    if (server.master) {
        rsi->repl_stream_db = server.master->db->id;
        return rsi;
    }

    /* If we have a cached master we can use it in order to populate the
     * replication selected DB info inside the RDB file: the slave can
     * increment the master_repl_offset only from data arriving from the
     * master, so if we are disconnected the offset in the cached master is valid. */
    if (server.cached_master) {
        rsi->repl_stream_db = server.cached_master->db->id;
        return rsi;
    }
    return NULL;
}





