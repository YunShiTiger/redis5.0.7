/*
 * 用于检测aof文件完整性的测试程序
 *		分析aof完整性的流程
 *			1 首先效验输出参数的信息是否正确 即是否满足aof完整性效验的命令格式
 *				分析出需要效验的文件 已经是否进行修复aof的参数信息
 *			2 然后读取前5个字符分析是否是REDIS 即是否开启aof和rdb的混合模式
 *			3 开启混合模式 先进行rdb部分的数据效验 执行redis_check_rdb_main函数操作
 *		    4 进行aof部分数据的效验操作处理 执行对应的process函数操作
 *				循环解析对应的aof中的命令
 *					1 解析本次命令对应的参数数量 * 后面是命令参数个数
 *					2 循环解析命令字符串
 *						$ 后是命令字符串的长度值
 *						读取对应长度的字符串数据
 */

#include "server.h"
#include <sys/stat.h>

#define ERROR(...) { \
    char __buf[1024]; \
    snprintf(__buf, sizeof(__buf), __VA_ARGS__); \
    snprintf(error, sizeof(error), "0x%16llx: %s", (long long)epos, __buf); \
}

//用于记录对应的错误描述信息的buffer
static char error[1044];
//用于记录出现错误的文件偏移位置
static off_t epos;

/* 检测给定的字符串数据是否是换行标识 */
int consumeNewline(char *buf) {
	//检测对应的字符串数据是否存储的是换行标识
    if (strncmp(buf,"\r\n",2) != 0) {
		//输出对应的存储的内容对应的日志错误信息
        ERROR("Expected \\r\\n, got: %02x%02x",buf[0],buf[1]);
		//返回错误标识
        return 0;
    }
	//返回匹配成功的标识
    return 1;
}

/* 读取对应前缀后的数字值 *标识后续是参数个数值 $表示后续是字符串长度值 */
int readLong(FILE *fp, char prefix, long *target) {
    char buf[128], *eptr;
	//记录当前读取的位置偏移量
    epos = ftello(fp);
	//读取一行数据
    if (fgets(buf,sizeof(buf),fp) == NULL) {
		//返回读取信息失败的标识
        return 0;
    }
	//检测第一个字符值是否是需要匹配的字符
    if (buf[0] != prefix) {
		//输出对应的匹配对应字符失败的错误日志
        ERROR("Expected prefix '%c', got: '%c'",prefix,buf[0]);
		//返回读取失败的标识
        return 0;
    }
	//将对应的字符串数据转换成对应的十进制整数值
    *target = strtol(buf+1,&eptr,10);
	//进一步检测后续字符存储的是否是换行字符数据
    return consumeNewline(eptr);
}

/* 从文件中读取对应长度的数据 */
int readBytes(FILE *fp, char *target, long length) {
    long real;
	//记录当前位置
    epos = ftello(fp);
	//进行读取指定长度的数据
    real = fread(target,1,length,fp);
	//检测读取的数量是否匹配
    if (real != length) {
		//写入不能读取指定数量的数据日志
        ERROR("Expected to read %ld bytes, got %ld bytes",length,real);
		//返回读取失败的标识
        return 0;
    }
	//返回读取成功的标识
    return 1;
}

/* 读取对应的字符串数据 */
int readString(FILE *fp, char** target) {
    long len;
    *target = NULL;
	//进一步读取需要加载的字符串长度值
    if (!readLong(fp,'$',&len)) {
        return 0;
    }

    /* Increase length to also consume \r\n */
	//长度值加2表示读取完字符串后面的换行符
    len += 2;
	//分配对应长度的空间 用于存储读取的字符串数据                       注意在此处开辟的空间 在外部进行释放 需要使用二级指针
    *target = (char*)zmalloc(len);
	//从文件中读取对应长度的字符串数据
    if (!readBytes(fp,*target,len)) {
        return 0;
    }
	//检测后续的两个字符是否是对应的换行符
    if (!consumeNewline(*target+len-2)) {
        return 0;
    }
	//返回字符串的数据时 设置字符串的结束标识
    (*target)[len-2] = '\0';
	//返回读取数据成功的标识
    return 1;
}

/* 读取命令参数数量值 即确认后续需要读取的参数数量值 */
int readArgc(FILE *fp, long *target) {
	//进行读取参数的数量值
    return readLong(fp,'*',target);
}

/* 循环检测对应的命令是否合法 即完成对应aof部分数据的效验操作处理 */
off_t process(FILE *fp) {
    long argc;
    off_t pos = 0;
    int i, multi = 0;
    char *str;
	//循环解析aof中的命令
    while(1) {
        if (!multi) 
			pos = ftello(fp);
		//读取对应的命令参数个数值
        if (!readArgc(fp, &argc)) 
			//命令参数个数读取失败 直接退出循环就可以了
			break;
		//循环读取对应数量的命令参数值
        for (i = 0; i < argc; i++) {
			//读取对应位置上的命令参数字符串
            if (!readString(fp,&str)) 
				//读取失败就可以退出本循环操作了
				break;
			//检测读取的是否是第一个参数字符串 即对应的命令名称
            if (i == 0) {
				//检测对应的命令是否是事物命令开始
                if (strcasecmp(str, "multi") == 0) {
					//进行命令事物数量值自增处理
                    if (multi++) {
                        ERROR("Unexpected MULTI");
                        break;
                    }
                } else if (strcasecmp(str, "exec") == 0) {
                	//进行命令事物数量值自减处理 即完成了一组事物命令
                    if (--multi) {
                        ERROR("Unexpected EXEC");
                        break;
                    }
                }
            }
			//释放对应的字符串空间
            zfree(str);
        }

        /* Stop if the loop did not finish */
		//检测是否读取对应的参数数据正常
        if (i < argc) {
			//检测是否需要进行释放空间操作处理
            if (str) 
				zfree(str);
			//跳出对应的循环
            break;
        }
    }

	//检测文件是否读取到末尾位置 同时对应的事物命令匹配 以及没有对应的错误日志
    if (feof(fp) && multi && strlen(error) == 0) {
        ERROR("Reached EOF before reading EXEC for MULTI");
    }
	//检测是否存储了对应的错误日志
    if (strlen(error) > 0) {
		//输出对应的错误日志信息
        printf("%s\n", error);
    }
	//返回读取到的文件位置偏移 如果一切正常那么返回值就是文件结束位置偏移 出现异常就是对应的异常位置偏移
    return pos;
}

/* 启动检测aof文件完整性的检测任务处理 */
int redis_check_aof_main(int argc, char **argv) {
	//用于记录对应的打开的文件名
    char *filename;
	//用于标识是否对aof中出现错误命令进行修复操作处理
    int fix = 0;
	//进行校验参数操作处理
    if (argc < 2) {
		//参数小于2 即 输出对应的参数帮助信息
        printf("Usage: %s [--fix] <file.aof>\n", argv[0]);
		//退出应用处理
        exit(1);
    } else if (argc == 2) {
		//两个参数 记录对应的待校验的文件名
        filename = argv[1];
    } else if (argc == 3) {
		//三个参数 获取对应的fix参数值
        if (strcmp(argv[1],"--fix") != 0) {
			//不是对应的fix标识 输出对应的参数信息错误
            printf("Invalid argument: %s\n", argv[1]);
            exit(1);
        }
		//记录对应的文件名称
        filename = argv[2];
		//记录对应的fix标识
        fix = 1;
    } else {
		//超过对应的参数数量 输出对应的参数数量错误
        printf("Invalid arguments\n");
		//退出应用处理
        exit(1);
    }

	//打开对应的文件
    FILE *fp = fopen(filename,"r+");
	//检测打开对应文件是否成功
    if (fp == NULL) {
		//输出对应打开文件失败信息
        printf("Cannot open file: %s\n", filename);
		//退出应用处理
        exit(1);
    }

    struct redis_stat sb;
	//获取文件的大小字节数量
    if (redis_fstat(fileno(fp),&sb) == -1) {
		//输出不能获取文件统计信息的错误信息
        printf("Cannot stat file: %s\n", filename);
        exit(1);
    }

	//获取统计的文件字节数量值
    off_t size = sb.st_size;
	//检测文件中是否有内容
    if (size == 0) {
		//输出对应的文件没有数据的信息
        printf("Empty file: %s\n", filename);
        exit(1);
    }

    /* This AOF file may have an RDB preamble. Check this to start, and if this is the case, start processing the RDB part. */
	/* There must be at least room for the RDB header. */
	//检测文件大小是否超过了8字节 即至少满足检测是否是aof和rdb混合模式
    if (size >= 8) { 
		//定义存储前五个字节的缓存空间
        char sig[5];
		//读取5字节并与REDIS字符串进行比较 确认是否是混合模式
        int has_preamble = fread(sig,sizeof(sig),1,fp) == 1 && memcmp(sig,"REDIS",sizeof(sig)) == 0;
		//此处重新将读取位置设置到开始位置
        rewind(fp);
		//检测是否处于混合模式
        if (has_preamble) {
			//输出处于混合模式 优先读取效验rdb部分数据的完整性
            printf("The AOF appears to start with an RDB preamble.\n"
                   "Checking the RDB preamble to start:\n");
			//开始进行rdb部分数据的完整性效验处理
            if (redis_check_rdb_main(argc,argv,fp) == C_ERR) {
				//输出rdb效验完整性不通过的日志信息
                printf("RDB preamble of AOF file is not sane, aborting.\n");
				//退出对应的应用程序
                exit(1);
            } else {
				//输出rdb部分效验通过的日志 后续进行aof命令效验处理
                printf("RDB preamble is OK, proceeding with AOF tail...\n");
            }
        }
    }

	//执行aof命令部分的完整性检测处理
    off_t pos = process(fp);
	//计算对应的差值 即文件总字节数 和已经读取到的文件偏移量 之间的差值
    off_t diff = size-pos;
	//输出对应 文件字节数 读取到的位置偏移位置 差值
    printf("AOF analyzed: size=%lld, ok_up_to=%lld, diff=%lld\n", (long long) size, (long long) pos, (long long) diff);
	//检测是否存在差值 即文件完整性出现了问题
    if (diff > 0) {
		//检测是否配置了进行修复错误aof命令标识
        if (fix) {
            char buf[2];
			//输出对应的字节信息
            printf("This will shrink the AOF from %lld bytes, with %lld bytes, to %lld bytes\n",(long long)size,(long long)diff,(long long)pos);
			//输出是否进行修复操作处理
			printf("Continue? [y/N]: ");
			//获取用户输入的操作字符 即是否需要进行修复
            if (fgets(buf,sizeof(buf),stdin) == NULL || strncasecmp(buf,"y",1) != 0) {
				//不需要进行修复就输出终止标识
				printf("Aborting...\n");
				//退出应用程序
                exit(1);
            }
			//在对应的偏移处进行文件截断处理------->此处说的修复就是将前面正确的命令保存 从后续出现错误命令的地方开始进行截取操作 即后续命令都不要了
            if (ftruncate(fileno(fp), pos) == -1) {
				//进行文件截取失败 输出错误日志
                printf("Failed to truncate AOF\n");
				//退出应用程序
                exit(1);
            } else {
				//输出截取文件成功日志
                printf("Successfully truncated AOF\n");
            }
        } else {
        	//输出aof效验不通过的日志
            printf("AOF is not valid. "
                   "Use the --fix option to try fixing it.\n");
			//退出应用程序
            exit(1);
        }
    } else {
    	//输出文件效验通过的日志
        printf("AOF is valid\n");
    }
	//关闭对应的文件
    fclose(fp);
	//退出应用程序
    exit(0);
}



