/*
 * 用于检测rdb文件完整性的测试程序
 */

#include "server.h"
#include "rdb.h"

#include <stdarg.h>

void createSharedObjects(void);
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len);

//用于标识当前是否处于
int rdbCheckMode = 0;

struct {
	//用于记录与rdb文件关联的rio结构
    rio *rio;
	//用于临时记录当前处理的键对象
    robj *key;                      /* Current key we are reading. */
	//用于临时记录当前处理的键值对中值对象的类型
    int key_type;                   /* Current key type if != -1. */
	//用于统计字典中键值对的数量
    unsigned long keys;             /* Number of keys processed. */
	//用于统计过期字典中键值对的数量
    unsigned long expires;          /* Number of keys with an expire. */
	//用于统计已经过期的键值对的数量
    unsigned long already_expired;  /* Number of keys already expired. */
	//用于标识读取rdb文件过程中当前所处的状态
    int doing;                      /* The state while reading the RDB. */
	//用于标识在解析过程中是否出现错误
    int error_set;                  /* True if error is populated. */
	//用于记录对应错误的buffer数组
    char error[1024];
} rdbstate;

/* At every loading step try to remember what we were about to do, so that we can log this information when an error is encountered. */
/* 检测过程中所处的状态宏值 */
#define RDB_CHECK_DOING_START 0
#define RDB_CHECK_DOING_READ_TYPE 1
#define RDB_CHECK_DOING_READ_EXPIRE 2
#define RDB_CHECK_DOING_READ_KEY 3
#define RDB_CHECK_DOING_READ_OBJECT_VALUE 4
#define RDB_CHECK_DOING_CHECK_SUM 5
#define RDB_CHECK_DOING_READ_LEN 6
#define RDB_CHECK_DOING_READ_AUX 7

/* 用于标识当前所处的动作 */
char *rdb_check_doing_string[] = {
    "start",
    "read-type",
    "read-expire",
    "read-key",
    "read-object-value",
    "check-sum",
    "read-len",
    "read-aux"
};

/* 用于标识当前读取到的值对象的类型 */
char *rdb_type_string[] = {
    "string",
    "list-linked",
    "set-hashtable",
    "zset-v1",
    "hash-hashtable",
    "zset-v2",
    "module-value",
    "","",
    "hash-zipmap",
    "list-ziplist",
    "set-intset",
    "zset-ziplist",
    "hash-ziplist",
    "quicklist",
    "stream"
};

/* Show a few stats collected into 'rdbstate' */
/* 显示采集到的rdb文件中的键值对的统计信息 */
void rdbShowGenericInfo(void) {
	//输出字典中键值对的元素个数
    printf("[info] %lu keys read\n", rdbstate.keys);
	//输出过期字典中键值对的元素个数
    printf("[info] %lu expires\n", rdbstate.expires);
	//输出已经有多少键值对已经处于过期中
    printf("[info] %lu already expired\n", rdbstate.already_expired);
}

/* Called on RDB errors. Provides details about the RDB and the offset we were when the error was detected. */
/* 当解析rdb文件出现错误的情况下 输出对应的错误信息 */
void rdbCheckError(const char *fmt, ...) {
    char msg[1024];
	//定义一个具有va_list型的变量，这个变量是指向参数的指针
    va_list ap;
	//第一个参数指向可变列表的地址,地址自动增加，第二个参数位固定值
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    printf("--- RDB ERROR DETECTED ---\n");
	//输出出现错误的索引位置
    printf("[offset %llu] %s\n", (unsigned long long) (rdbstate.rio ? rdbstate.rio->processed_bytes : 0), msg);
	//输出标识当前所处的状态
    printf("[additional info] While doing: %s\n", rdb_check_doing_string[rdbstate.doing]);
    if (rdbstate.key)
		//输出出现问题所对应的当前键对象
        printf("[additional info] Reading key '%s'\n", (char*)rdbstate.key->ptr);
    if (rdbstate.key_type != -1)
		//输出出现问题所对应的当前值对象的类型
        printf("[additional info] Reading type %d (%s)\n", rdbstate.key_type, ((unsigned)rdbstate.key_type < sizeof(rdb_type_string)/sizeof(char*)) ? rdb_type_string[rdbstate.key_type] : "unknown");
	//输出对应的统计信息
	rdbShowGenericInfo();
}

/* Print informations during RDB checking. */
/* 用于输出在rdb检测过程中的相关信息 */
void rdbCheckInfo(const char *fmt, ...) {
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

	//输出当前所处的文件偏移位置 已经对应的信息
    printf("[offset %llu] %s\n", (unsigned long long) (rdbstate.rio ? rdbstate.rio->processed_bytes : 0), msg);
}

/* Used inside rdb.c in order to log specific errors happening inside the RDB loading internals. */
/* */
void rdbCheckSetError(const char *fmt, ...) {
	//https://blog.csdn.net/ZKR_HN/article/details/99558135
	//https://www.cnblogs.com/qiwu1314/p/9844039.html
	//https://www.cnblogs.com/smy87/p/9274705.html
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rdbstate.error, sizeof(rdbstate.error), fmt, ap);
    va_end(ap);
	//设置出现错误的标识
    rdbstate.error_set = 1;
}

/* During RDB check we setup a special signal handler for memory violations and similar conditions, so that we can log the offending part of the RDB if the crash is due to broken content. */
void rdbCheckHandleCrash(int sig, siginfo_t *info, void *secret) {
    UNUSED(sig);
    UNUSED(info);
    UNUSED(secret);
	
	//输出对应的错误日志信息
    rdbCheckError("Server crash checking the specified RDB file!");
	//退出应用程序
    exit(1);
}

void rdbCheckSetupSignals(void) {
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = rdbCheckHandleCrash;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
}

/* Check the specified RDB file. Return 0 if the RDB looks sane, otherwise 1 is returned.
 * The file is specified as a filename in 'rdbfilename' if 'fp' is not NULL, otherwise the already open file 'fp' is checked. */
/* 解析并效验rdb文件合法性的核心处理函数 注意此处与加载rdb文件的不同之处 */
int redis_check_rdb(char *rdbfilename, FILE *fp) {
    uint64_t dbid;
    int type, rdbver;
    char buf[1024];
    long long expiretime, now = mstime();
    static rio rdb; /* Pointed by global struct riostate. */
	//设置是否关闭文件的标识 即对应的文件是通过路劲传入的
    int closefile = (fp == NULL);
	//尝试打开对应的rdb文件
    if (fp == NULL && (fp = fopen(rdbfilename,"r")) == NULL) 
		return 1;

	//根据对应的文件来初始化rio结构
    rioInitWithFile(&rdb,fp);
	//将rio结构设置到记录rdb状态结构中
    rdbstate.rio = &rdb;
	//设置读取文件过程中更新校验码的处理函数
    rdb.update_cksum = rdbLoadProgressCallback;

	//首先读取9个字节
    if (rioRead(&rdb,buf,9) == 0) 
		goto eoferr;

	//设置对应的字符串结束符
    buf[9] = '\0';

	//检测前5个字符是否是REDIS的魔幻数
    if (memcmp(buf,"REDIS",5) != 0) {
		//记录对应的失败日志信息
        rdbCheckError("Wrong signature trying to load DB from file");
        goto err;
    }

	//获取对应的版本值
    rdbver = atoi(buf+5);
	//检测版本值是否在合法范围内
    if (rdbver < 1 || rdbver > RDB_VERSION) {
		//记录对应的失败日志信息
        rdbCheckError("Can't handle RDB format version %d",rdbver);
        goto err;
    }

	//用于记录对应的键值对的过期时间值
    expiretime = -1;
	//设置开始解析rdb文件启动标识
    startLoading(fp);
    while(1) {
        robj *key, *val;

        /* Read type. */
		//设置当前正处于读取标识的状态
        rdbstate.doing = RDB_CHECK_DOING_READ_TYPE;
		//读取一个字节的标识
        if ((type = rdbLoadType(&rdb)) == -1) 
			goto eoferr;

        /* Handle special types. */
		//根据读取到的标识来解析数据处理
        if (type == RDB_OPCODE_EXPIRETIME) {
			//设置当前处于读取过期时间的状态
            rdbstate.doing = RDB_CHECK_DOING_READ_EXPIRE;
            /* EXPIRETIME: load an expire associated with the next key to load. Note that after loading an expire we need to load the actual type, and continue. */
			//读取对应的过期时间值
			if ((expiretime = rdbLoadTime(&rdb)) == -1) 
				goto eoferr;
			//将过期时间转换成对应的秒值
            expiretime *= 1000;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced with RDB v3. Like EXPIRETIME but no with more precision. */
			//设置当前处于读取过期时间的状态
			rdbstate.doing = RDB_CHECK_DOING_READ_EXPIRE;
			//读取对应的秒值
            if ((expiretime = rdbLoadMillisecondTime(&rdb, rdbver)) == -1) 
				goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
			//读取对应的Lfu对应的次数值
            uint8_t byte;
            if (rioRead(&rdb,&byte,1) == 0) 
				goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
			//读取对应的lru对应的 空转时间值
            if (rdbLoadLen(&rdb,NULL) == RDB_LENERR) 
				goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
			//读取到对应的结束符标识 触发退出循环解析数据的操作处理
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
			//设置当前处于读取对应库索引值的状态
            rdbstate.doing = RDB_CHECK_DOING_READ_LEN;
			//读取对应的索引库值
            if ((dbid = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
			//输出对应的选择索引库值的日志信息
            rdbCheckInfo("Selecting DB ID %d", dbid);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently selected data base, in order to avoid useless rehashing. */
			//设置当前处于读取库尺寸信息的标识
            uint64_t db_size, expires_size;
            rdbstate.doing = RDB_CHECK_DOING_READ_LEN;
			//读取字典数据的元素数量值
            if ((db_size = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
			//读取过期字典数据的元素数量值
            if ((expires_size = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB which is backward compatible. Implementations of RDB loading are requierd to skip AUX fields they don't understand.
             * An AUX field is composed of two strings: key and value. */
            //设置当前处于读取辅助信息的标识
            robj *auxkey, *auxval;
            rdbstate.doing = RDB_CHECK_DOING_READ_AUX;
			//读取对应的字段字符串
            if ((auxkey = rdbLoadStringObject(&rdb)) == NULL) 
				goto eoferr;
			//读取对应的值字符串
            if ((auxval = rdbLoadStringObject(&rdb)) == NULL) 
				goto eoferr;
			//输出读取到的辅助信息
            rdbCheckInfo("AUX FIELD %s = '%s'", (char*)auxkey->ptr, (char*)auxval->ptr);
			//释放对应的对象空间
            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else {
			//不属于上述的标识 那么就是对应的值类型的标识 
			//检测对应的类型是否处于合法范围内
            if (!rdbIsObjectType(type)) {
				//输出对应的值类型错误日志
                rdbCheckError("Invalid object type: %d", type);
                goto err;
            }
			//记录当前值对象的类型 
            rdbstate.key_type = type;
        }

        /* Read key */
		//设置当前处于读取键值对的键对象过程中的状态
        rdbstate.doing = RDB_CHECK_DOING_READ_KEY;
		//读取对应的键字符串对象
        if ((key = rdbLoadStringObject(&rdb)) == NULL) 
			goto eoferr;
		//记录对应的键字符串对象
        rdbstate.key = key;
		//记录对应的统计值信息
        rdbstate.keys++;
        /* Read value */
		//设置当前处于读取键值对的值对象过程中的状态
        rdbstate.doing = RDB_CHECK_DOING_READ_OBJECT_VALUE;
		//读取对应的值对象
        if ((val = rdbLoadObject(type,&rdb,key)) == NULL) 
			goto eoferr;
        /* Check if the key already expired. */
		//检测是否设置了过期时间 且是否已经过期了
        if (expiretime != -1 && expiretime < now)
			//统计过期值数量
            rdbstate.already_expired++;
		//检测是否设置了过期时间
        if (expiretime != -1) 
			//统计设置过期时间的键值对的数量
			rdbstate.expires++;
		//置空对应的当前遍历的键对象
        rdbstate.key = NULL;
		//释放对应的键值对空间
        decrRefCount(key);
        decrRefCount(val);
		//重置对应的值对象的类型参数
        rdbstate.key_type = -1;
		//重置键值对的过期时间
        expiretime = -1;
    }
	
    /* Verify the checksum if RDB version is >= 5 */
	//检测对应的版本信息获取是否有必要获取效验码值 注意校验码的写入是没有进行校验操作处理的
    if (rdbver >= 5 && server.rdb_checksum) {
        uint64_t cksum, expected = rdb.cksum;
		//设置当前处于获取效验码的状态
        rdbstate.doing = RDB_CHECK_DOING_CHECK_SUM;
		//读取8字节的效应码值
        if (rioRead(&rdb,&cksum,8) == 0) 
			goto eoferr;
		//进行大端法处理
        memrev64ifbe(&cksum);
		//检测对应的效应码是否匹配
        if (cksum == 0) {
			//输出没有读取到对应的效验码值的错误日志
            rdbCheckInfo("RDB file was saved with checksum disabled: no check performed.");
        } else if (cksum != expected) {
        	//输出对应的效验码值不匹配的错误日志
            rdbCheckError("RDB CRC error");
            goto err;
        } else {
			//输出对应的效验通过的日志信息
            rdbCheckInfo("Checksum OK");
        }
    }

	//检测是否需要关闭打开的文件
    if (closefile) 
		//关闭对应的文件 谁打开的谁负责关闭
		fclose(fp);
	//返回rdb文件完整的标识
    return 0;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    if (rdbstate.error_set) {
        rdbCheckError(rdbstate.error);
    } else {
        rdbCheckError("Unexpected EOF reading RDB file");
    }
err:
    if (closefile) 
		fclose(fp);
    return 1;
}

/* RDB check main: called form redis.c when Redis is executed with the redis-check-rdb alias, on during RDB loading errors.
 * The function works in two ways: can be called with argc/argv as a standalone executable, or called with a non NULL 'fp' argument if we
 * already have an open file to check. This happens when the function is used to check an RDB preamble inside an AOF file.
 * When called with fp = NULL, the function never returns, but exits with the status code according to success (RDB is sane) or error (RDB is corrupted).
 * Otherwise if called with a non NULL fp, the function returns C_OK or C_ERR depending on the success or failure. */
/* 启动检测对应的rdb文件完整性的处理任务 */
int redis_check_rdb_main(int argc, char **argv, FILE *fp) {
	//检测传入的参数是否正确 即确定是否传入对应的目录文件
    if (argc != 2 && fp == NULL) {
		//输出提示日志
        fprintf(stderr, "Usage: %s <rdb-file-name>\n", argv[0]);
		//退出操作
        exit(1);
    }
    /* In order to call the loading functions we need to create the shared integer objects, however since this function may be called from an already initialized Redis instance, check if we really need to. */
	//检测是否建立独立共享对象
	if (shared.integers[0] == NULL)
		//创建对应的共享对象值
        createSharedObjects();
	//
    server.loading_process_events_interval_bytes = 0;
	//设置标识 当前处于rdb文件检测完整性的模式
    rdbCheckMode = 1;
	//
    rdbCheckInfo("Checking RDB file %s", argv[1]);
	//
    rdbCheckSetupSignals();
	//触发检测rdb文件的完整性操作处理----->检测的核心处理函数
    int retval = redis_check_rdb(argv[1],fp);
	//检测执行rdb文件检测完整性是否成功
    if (retval == 0) {
		//输出检测rdb文件完整性成功的信息
        rdbCheckInfo("\\o/ RDB looks OK! \\o/");
		//输出rdb文件中的键值对的统计信息
        rdbShowGenericInfo();
    }
	//检测是否传入了文件
    if (fp) 
		//返回对应的文件完整性标识 这个地方主要是传入了文件 那么就需要在外部进行关闭文件的操作处理 所以需要返回参数 而不是直接退出应用
		return (retval == 0) ? C_OK : C_ERR;
	//退出应用程序
    exit(retval);
}



