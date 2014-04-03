#include"JsObject.h"
#include"JsContext.h"
#include"JsEngine.h"
#include"JsVm.h"
#include"JsValue.h"
#include"JsList.h"
#include"JsSys.h"
#include"JsInit.h"
#include"JsDebug.h"
#include"JsException.h"
#include<signal.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>

/***********************************内存管理模块 API*******************************/
/*
	内存回收的时候, 会停止全部线程, 然后等待GC结束, 回收的内存为主内存区标记 JS_GC_MISS 的内存快, 
JS_GC_MISS 是防止在Stop the World的时候, 处理A->B 关系图为 A->C->B的时候, 引用临时断裂而造成内存错误被回收的情况,
如果一块内存快未命中 JS_GC_MISS 则代表该进程已经死了, 虽然错误率还有, 但是基本不大.
	内存快如果处于TLS->HashTable中, 则不进行回收.
*/
//Hash Table表大小
#define JS_GC_HASH_TABLE_SIZE 	1024*1024
//Key Table初始化大小, 随后会增长
#define JS_GC_KEY_TABLE_SIZE 	64
//Root Table初始化大小, 随后会增长
#define JS_GC_ROOT_TABLE_SIZE  	1024
//检测到内存到达上限, 则会停止所有线程进入GC
//(单位 : KB)
#define JS_GC_MEMORY_LINE 		1024
//未命中数值, 一旦内存快(主存中的)未命中次数到达指定数, 则回收
#define JS_GC_MISS 				2
//GC 线程睡觉的时间, 如果内存充足, 建议时间拉长点
#define JS_GC_SLEEP				10


/*BP 和 MP 转换宏*/
#define JS_MP2BP(mp,bp)				\
	do{								\
		bp = (struct JsGcBlock*)mp;	\
		bp--;						\
	}while(0)
	
#define JS_BP2MP(bp,mp) 	\
	do{						\
		bp++;				\
		mp = (void*)bp;		\
		bp--;				\
	}while(0)
	
/**

内存分布结构:

Block+Mem
	bp--> ------------------------------
		 |MarkFn|FreeFn|int|    Mem     |
		 -------------------------------
						   |
					mp---->|
		 
KeyMan:
	  JsGcMan->table-->----------
	                 | JsGcKey*  |
					 ----------
					 | JsGcKey*  |
					 ----------
					 | JsGcKey*  |
					 ----------
					 | JsGcKey*  |
					 ----------
				
RootMan:
	   JsGcMan->table->--------
	                 | JsGcRoot* |
					 ---------
					 | JsGcRoot* |
					 ---------
					 | JsGcRoot* |
					 ---------
					 | JsGcRoot* |
					 ---------
					 
JsGcTlsList---->+
				+
				+[TID1]-->|
						   ---------------
						   |   JsGcHtNode*   |
						   ----------------        	  --------------------
						   |   JsGcHtNode*   | ----->|Count | mp | next |
						   ----------------       	 --------------------
						   |   JsGcHtNode*   |
						   ----------------
								.
								.
								.
							---------------
						   |   JsGcHtNode*   |
						   ----------------
						   
				+[TID2]--->|
						  ---------------
						   |   JsGcHtNode*   |
						   ----------------          --------------------
						   |   JsGcHtNode*   | ----->|Count | mp | next |
						   ----------------          --------------------
						   |   JsGcHtNode*   |
						   ----------------
								.
								.
								.
							---------------
						   |   JsGcHtNode*   |
						   ----------------
Gc后, 可以回收垃圾的表(Main)					   
JsGcMainHt(主存):----->|
					   |
					   ---------------
					   |   JsGcHtNode*   |
					   ----------------          --------------------
					   |   JsGcHtNode*   | ----->|Count | mp | next |
					   ----------------          --------------------
					   |   JsGcHtNode*   |
					   ----------------
							.
							.
							.
						---------------
					   |   JsGcHtNode*   |
					   ----------------
*/

struct JsGcMan{
	//全部空间大小
	int total;
	//已经被使用的大小
	int used;
	void* table;
};

struct JsGcKey{
	void* key;
	char* desc;
};

struct JsGcRoot{
	void* mp; //Mem内存指针
	void* key;// 有效key
};

/*申请内存时, 添加的头*/
struct JsGcBlock{
	JsGcMarkFn markFn; //内存关联mark函数
	JsGcFreeFn freeFn; //内存关联析构函数
	int size; //申请内存的大小
};

/*Hash Table 的节点*/
struct JsGcHtNode{
	/*
		命中计数:
			-1 : 表示DoMark中被标记中
			0+ : 未命中
	*/
	int count;
	void* mp;
	struct JsGcHtNode* next;
};

struct JsGcTlsNode{
	/*所属的线程ID*/
	pthread_t  tid;
	/*所属线程的hash table*/
	struct JsGcHtNode **table;
	/*table中包含的内存大小*/
	unsigned int size;
	/*Tls锁*/
	pthread_mutex_t* lock;
	/*下一个JsGcTlsNode*对象*/
	struct JsGcTlsNode* next;
};
/*++++++++++++++++++++全局静态变量区++++++++++++++++++++++*/

//组合Tls中JsGcTlsNode*的List
static struct JsGcTlsNode* JsGcTlsList = NULL;

/*Hash Table 主存*/
static struct JsGcHtNode* JsGcMainHt[JS_GC_HASH_TABLE_SIZE] = {NULL};

/*Key Manager*/
static struct JsGcMan* JsGcKeyMan;

/*RootManager*/
static struct JsGcMan* JsGcRootMan;

/*申请内存大小计数(byte), 可以计数4GB的内存空间*/
static unsigned int JsGcMemory = 0;

/*GC阶段标识*/
static int JsGcStage = FALSE;
/*++++++++++++++++++++全局静态锁和标记区++++++++++++++++++++++*/


/* GcLock, 对全局数据进行操作的时候, 需要加锁 */
static pthread_mutex_t* JsGcLock = NULL;

/*Stop of the world Lock*/
static pthread_mutex_t* JsGcStopLock = NULL;
/*Stop of the world Cond*/
static pthread_cond_t JsGcStopCond 	= PTHREAD_COND_INITIALIZER;

/* 获取线程Tls中的JsGcTlsNode*对象*/
static pthread_key_t* JsGcTlsKey = NULL;
/*-------------------------------------------------------------------*/

/*将mp插入到hashtable中,加Tls锁*/
static int JsGcMpInsert(struct JsGcHtNode** table,void* mp);
/*
	将mp从table中删除
*/
static int JsGcMpRemove(struct JsGcHtNode** table,void* mp);
/*
	根据mp 查到 JsGcHtNode, 搜索全部Ht(Main+Tls)
	返回:
		NULL 	: 不在HashTable中
		!NULL	: 正常数据
*/
static struct JsGcHtNode* JsGcFindHtNode(struct JsGcHtNode** table,void* mp);
/*计算mp指针对应的hashcode*/
static int JsGcHashCode(void* mp);



/*GC线程*/
static void* JsGcThread(void* data);
/*进行GC工作*/
static void JsGcWork();
/*
	刷新表中node->count = -1 --> 0 , 来重新标记
*/
static void JsGcRefreshHtNode(struct JsGcHtNode** table);
/*
		释放主存中到达释放计数的内存节点, 如果为
	非释放条件, 则为命中计数++
*/
static void JsGcFreeMainHtNode();
/*TlsNode->HashTable中的数据 和 大小 刷入主存等待回收,  最后清空*/
static void JsGcCommit0(struct JsGcTlsNode* node);


/*收到Gc信号(USR1)的时候, 调用的函数, 停止线程, 并且等待Gc结束(JsGcStopCond)*/
static void JsGcSigHandler(int sig);
/*开启线程的时候, 需要调用该函数, 来启动Gc功能*/
static void JsGcTBegin();
/*开启线程时, 对本线程中JsGcHtNode以及相关数据进行处理*/
static void JsGcBtTls();
/*线程结束时, 调用该函数释放Tls的数据*/
static void JsGcEtTls(void *data);



/*Gc模块初始化函数*/
static void JsPrevInitGc();
static void JsPostInitGc();

/*-------------------------------------------------------------------*/

/*
	委托到GC管理的内存中
		size must > 0
		markFm = NULL 的时候, 表示配置的内存空间为原生型 如int*,double* 等
		freeFn 为在回收该内存的时候, 执行的析构动作
	记录内存大小到JsMemory 和 Block中
*/
void* JsGcMalloc(int size,JsGcMarkFn markFn,JsGcFreeFn freeFn){

	JsAssert(size >= 0);
	
	/*block + size*/
	struct JsGcBlock* bp =(struct JsGcBlock*) malloc(size + sizeof(struct JsGcBlock));
	JsAssert(bp != NULL);
	//clear
	memset(bp,0,size + sizeof(struct JsGcBlock));
	/*set Block */
	bp->markFn = markFn;
	bp->freeFn = freeFn;
	bp->size = size;
	/*将MP插入到Hash表中进行管理*/
	void* mp;
	JS_BP2MP(bp,mp);
	//获得Tls的table
	struct JsGcTlsNode* node = (struct JsGcTlsNode*)pthread_getspecific(*JsGcTlsKey);
	//加锁, 防止GC线程破坏插入过程
	pthread_mutex_lock(node->lock);
	//插入Tls->Ht中
	JsGcMpInsert(node->table,mp);
	//增加本Tls申请内存的计数
	node->size += size;
	pthread_mutex_unlock(node->lock);
	
	return mp;
}
/*
	Gc空间realloc大小
	记录新大小到Block和JsMemory中
*/
void* JsGcReAlloc(void* mp0,int newSize){

	JsAssert(mp0 != NULL && newSize >= 0);
	
	void *mp1 = NULL;
	struct JsGcBlock *bp0, *bp1;
	JS_MP2BP(mp0,bp0);
	
	//判断是否在Tls还是Main中(Tls把内存已经Commit到主存了)
	struct JsGcTlsNode* node = (struct JsGcTlsNode*)pthread_getspecific(*JsGcTlsKey);
	pthread_mutex_lock(node->lock);
	struct JsGcHtNode* htNode = JsGcFindHtNode(node->table,mp0);
	if(htNode != NULL){
		//在Tls中
		
		//删除原先block大小
		node->size -= bp0->size;
		//realloc
		bp1 = realloc(bp0,newSize + sizeof(struct JsGcBlock));
		JsAssert(bp1 != NULL);
		bp1->size = newSize;
		//获取新mp1地址
		JS_BP2MP(bp1,mp1);
		//block内存地址被修改了
		if(bp1 != bp0){
			//删除原先Hash表中该对象	
			JsGcMpRemove(node->table,mp0);
			JsGcMpInsert(node->table,mp1);
		}
		/*添加新增内存大小*/
		node->size += newSize;
	}else{
		//在主存中, 则新申请内存, 放在Tls中, 原先的内存快等待回收 
		bp1 = malloc(newSize + sizeof(struct JsGcBlock));
		JsAssert(bp1 != NULL);
		//拷贝
		memcpy(bp1,bp0,bp0->size);
		bp1->size = newSize;
		
		//获取新mp1地址
		JS_BP2MP(bp1,mp1);
		JsGcMpInsert(node->table,mp1);
		/*添加新增内存大小*/
		node->size += newSize;
	}
	pthread_mutex_unlock(node->lock);
	return mp1;

}
/*标记指向内存空间为可用, 并且调用申请该内存空间时候, 配置的fn*/
int JsGcMark(void* mp){
	if(mp == NULL)
		return 0;
	JsAssert(JsGcStage != FALSE);
	struct JsGcHtNode* node = NULL;
	//查询mp的节点, 首先查询主存
	node = JsGcFindHtNode(JsGcMainHt,mp);
	//检测是否在Tls中
	if(node==NULL){
		struct JsGcTlsNode** tlspp = &JsGcTlsList;
		while(*tlspp != NULL){
			node = JsGcFindHtNode((*tlspp)->table,mp);
			//查询到该节点
			if(node != NULL)
				break;
			//下一个TlsNode
			tlspp = &(*tlspp)->next;
		}
	}
	//根据node, 执行相关动作
	if(node == NULL){
		//该内存不受GC管理
		return -1;
	}
	if(node->count == -1){
		//已经命中过
		return 1;
	}
	//Mark the node
	node->count = -1;
	struct JsGcBlock* bp = NULL;
	JS_MP2BP(mp,bp);
	//调用该mp对应的mark函数, 用于mark 和该内存快关联的内存
	if(bp->markFn != NULL)
		(*bp->markFn)(mp,bp->size);

	return 0;
}
/*
	Gc中添加一个有效的Key类型(指针类型),Key != 0
*/
int JsGcRegistKey(void* key,const char* desc){

	pthread_mutex_lock(JsGcLock);

	JsAssert(key != NULL);
	struct JsGcKey** keypp ;
	int i,size;
	
	//查询是否已经存在
	keypp = (struct JsGcKey** ) JsGcKeyMan->table;
	size = JsGcKeyMan->total;
	for(i=0 ; i < size ; ++i){
		if(keypp[i] != NULL && keypp[i]->key == key){
			//已经存在Key, 返回
			pthread_mutex_unlock(JsGcLock);
			return 0;
		}
	}
	//KeyTable中没有存在该Key, 则添加
	//判断是否已经满了
	if(JsGcKeyMan->total == JsGcKeyMan->used){
		
		//重新配置total参数
		int oldTotal =  JsGcKeyMan->total;
		int newTotal =  oldTotal * 2;
		
		//重新配置table空间大小
		//int oldSize = sizeof(struct JsGcKey*) * oldTotal;
		int newSize = sizeof(struct JsGcKey*) * newTotal;
		//重新申请
		struct JsGcKey** table = (struct JsGcKey**)realloc(JsGcKeyMan->table,newSize);
		//刷新内存
		for(i=oldTotal;i<newTotal;++i)
			table[i] = NULL;
		JsGcKeyMan->table = table;
		JsGcKeyMan->total = newTotal;
		//used 不变
	}
	//空间满足需求, 寻找第一个NULL的空间
	keypp = (struct JsGcKey** ) JsGcKeyMan->table;
	size = JsGcKeyMan->total;
	for( i = 0 ; i < size; ++i){
		if(keypp[i]== NULL){
			//申请空间
			keypp[i] = (struct JsGcKey*)malloc(sizeof(struct JsGcKey));
			keypp[i]->key = key;
			if(desc != NULL){
				keypp[i]->desc = malloc(strlen(desc) + 4);
				strcpy(keypp[i]->desc,desc);
			}else{
				keypp[i]->desc = NULL;
			}
			break;
		}
	}
	//被使用空间++
	JsGcKeyMan->used ++;
	
	pthread_mutex_unlock(JsGcLock);
	return 1;
}
/*Gc中删除一个Key , Key != 0, 并且删除RootTable中相关Root*/
void JsGcBurnKey(void* key){
	pthread_mutex_lock(JsGcLock);

	JsAssert(key != NULL);
	struct JsGcKey** keypp ;
	struct JsGcRoot** rootpp;
	int i,size;
	
	
	//剔除KeyTable
	keypp = (struct JsGcKey** ) JsGcKeyMan->table;
	size = JsGcKeyMan->total;
	for(i=0 ; i < size ; ++i){
		if(keypp[i] != NULL && keypp[i]->key == key){
			//释放该空间
			if(keypp[i]->desc != NULL)
				free(keypp[i]->desc);
			free(keypp[i]);
			//清除为NULL
			keypp[i] = NULL;
			//计数--
			JsGcKeyMan->used--;
			//KeyTable不再存在和key相关的
			break;
		}
	}
	//剔除RootTable中相关点
	rootpp = (struct JsGcRoot**)JsGcRootMan->table;
	size = JsGcRootMan->total;
	for(i=0 ; i < size ; ++i){
		if(rootpp[i] != NULL && rootpp[i]->key == key){
			//释放内存空间
			free(rootpp[i]);
			//清除为NULL
			rootpp[i] = NULL;
			//计数--
			JsGcRootMan->used--;
		}
	}
	pthread_mutex_unlock(JsGcLock);
}
/*	
	Gc扫描的时候, Root节点配置, 会检测是否存在KeyTable
	是否存在table, 和 RootTable是否存在相同Root
*/
void JsGcMountRoot(void* mp,void* key){

	pthread_mutex_lock(JsGcLock);
	JsAssert(key != NULL && mp != NULL);
		
	struct JsGcKey** keypp ;
	struct JsGcRoot** rootpp;
	int i,size;
	int flag ;
	
	//查询KeyTable中是否存在key
	flag = FALSE;
	keypp = (struct JsGcKey** ) JsGcKeyMan->table;
	size = JsGcKeyMan->total;
	for(i=0 ; i < size ; ++i){
		if(keypp[i] != NULL && keypp[i]->key == key){
			//存在key
			flag = TRUE;
			break;
		}
	}
	//KeyTable没有存在Key, 则中断程序
	JsAssert(flag);
	
	//查询RootTable中是否存在 (key,mp)
	rootpp = (struct JsGcRoot**)JsGcRootMan->table;
	size = JsGcRootMan->total;
	flag = FALSE;
	for(i=0 ; i < size ; ++i){
		if(rootpp[i] != NULL && rootpp[i]->key == key && rootpp[i]->mp == mp){
			//存在该组合
			flag = TRUE;
			break;
		}
	}
	if(flag){
		//已经存在了
		pthread_mutex_unlock(JsGcLock);
		return;
	}
	//KeyTable 存在Key 并且RootTable 不存在(key,mp)组合
	//验证空间是否满足
	if(JsGcRootMan->total == JsGcRootMan->used){
		
		//重新配置total参数
		int oldTotal =  JsGcRootMan->total;
		int newTotal =  oldTotal * 2;
		
		//重新配置table空间大小
		//int oldSize = sizeof(struct JsGcRoot*) * oldTotal;
		int newSize = sizeof(struct JsGcRoot*) * newTotal;
		
		//重新申请
		struct JsGcRoot** table = (struct JsGcRoot**)realloc(JsGcRootMan->table,newSize);
		//刷新新申请位置
		for(i=oldTotal;i<newTotal;++i)
			table[i]=NULL;
		JsGcRootMan->table = table;
		JsGcRootMan->total = newTotal;
		//used 不变
	}
	//空间满足需求, 寻找第一个NULL的空间
	rootpp = (struct JsGcRoot**)JsGcRootMan->table;
	size = JsGcRootMan->total;
	for(i=0 ; i < size ; ++i){
		if(rootpp[i] == NULL){
			rootpp[i] = (struct JsGcRoot*)malloc(sizeof(struct JsGcRoot));
			rootpp[i]->mp = mp;
			rootpp[i]->key = key;
			break;
		}
	}
	//被使用空间++
	JsGcRootMan->used ++;
	
	pthread_mutex_unlock(JsGcLock);

}
void JsGcCommit(){
	struct JsGcTlsNode* node = (struct JsGcTlsNode*)pthread_getspecific(*JsGcTlsKey);
	if(node == NULL)
		return;
	JsGcCommit0(node);
}
/*-------------------------------------------------------------------*/

static int JsGcMpInsert(struct JsGcHtNode** table,void* mp){
	JsAssert(mp != NULL);
	int hashCode = JsGcHashCode(mp);
	//新节点空间
	struct JsGcHtNode* nodep = (struct JsGcHtNode*)malloc(sizeof(struct JsGcHtNode));
	
	nodep->count = 0;
	//插入到Hashtable 第一个位置
	nodep->next = table[hashCode];
	table[hashCode] = nodep;
	
	nodep->mp = mp;

	return TRUE;
}
/*将mp从hashTable中删除*/
static int JsGcMpRemove(struct JsGcHtNode** table,void* mp){
	JsAssert(mp != NULL);
	int hashCode = JsGcHashCode(mp);
	struct JsGcHtNode** nodepp = &table[hashCode];
	while(*nodepp != NULL ){
		if((*nodepp)->mp == mp){
			//记录要删除的node对象
			struct JsGcHtNode* nodep = (*nodepp);
			//修改prev ~ next 关系
			(*nodepp) = (*nodepp)->next;
			free(nodep);
			break;
		}
		nodepp = &(*nodepp)->next;
	}
	return TRUE;
}

static struct JsGcHtNode* JsGcFindHtNode(struct JsGcHtNode** table,void* mp){
	JsAssert(mp != NULL);
	int hashCode = JsGcHashCode(mp);
	struct JsGcHtNode** nodepp = &table[hashCode];
	while(*nodepp != NULL ){
		if((*nodepp)->mp == mp){
			return *nodepp;
		}
		nodepp = &(*nodepp)->next;
	}
	return NULL;
}
static int JsGcHashCode(void* mp){
	if(mp == NULL)
		return 0;
	char buf[128];
	sprintf(buf,"%p",mp);
	char* pEnd;
	int code;
	code = strtol(buf,&pEnd,0);
	code %= JS_GC_HASH_TABLE_SIZE;
	return code;
}


//Gc线程
static void* JsGcThread(void* data){
	struct JsGcTlsNode** tlspp;
	while(1){
		
		pthread_mutex_lock(JsGcLock);
		//Gc阶段表示
		JsGcStage = TRUE;
		//锁住所有tls
		tlspp = &JsGcTlsList;
		while(*tlspp != NULL){
			pthread_mutex_lock((*tlspp)->lock);
			//pthread_kill((*tlspp)->tid,SIGUSR1);
			tlspp = &(*tlspp)->next;
		}
		//内存到达临界点
		unsigned int mem = JsGcMemory;
		tlspp = &JsGcTlsList;
		while(*tlspp != NULL){
			mem += (*tlspp)->size;
			tlspp = &(*tlspp)->next;
		}
		printf("Alloced Mem : %d \n", mem);
		if(mem >= JS_GC_MEMORY_LINE*1024){
			printf("Do Gc Work...\n");
			//内存到达临界点
			JsGcWork();
			printf("Finish Gc Work...\n");
		}
		
		
		//解锁所有tls
		tlspp = &JsGcTlsList;
		while(*tlspp != NULL){
			pthread_mutex_unlock((*tlspp)->lock);
			tlspp = &(*tlspp)->next;
		}
		//GC结束
		JsGcStage = FALSE;
		pthread_mutex_unlock(JsGcLock);
		
		//休眠该线程
		sleep(JS_GC_SLEEP);
	}
	return NULL;
};
/*
	当进行Gc的时候, 所有的内存申请(除GC线程)都会被锁在JsGcLock处.
*/
static void JsGcWork(){

	//所有开启Gc功能的线程, 进行STOP
	struct JsGcTlsNode** tlspp;
	tlspp = &JsGcTlsList;
	//Tls Table
	while(*tlspp != NULL){
		pthread_kill((*tlspp)->tid,SIGUSR1);
		tlspp = &(*tlspp)->next;
	}
	
	//刷新count=-1 的内存标记为0, 进入mark环节, 对于0+的内存快
	//则不能重新计数
	tlspp = &JsGcTlsList;
	//Tls Table
	while(*tlspp != NULL){
		JsGcRefreshHtNode((*tlspp)->table);
		tlspp = &(*tlspp)->next;
	}
	//Main Table
	JsGcRefreshHtNode(JsGcMainHt);

	int i ;
	struct JsGcRoot** rootpp = (struct JsGcRoot**)JsGcRootMan->table;
	for(i = 0 ; i < JsGcRootMan->total ; ++i){
		if(rootpp[i] != NULL){
			JsGcMark(rootpp[i]->mp);
		}
	}
	//free
	JsGcFreeMainHtNode();

	//通知所有进程可以进行继续工作了
	pthread_cond_broadcast(&JsGcStopCond);

}

static void JsGcRefreshHtNode(struct JsGcHtNode** table){
	int i;
	struct JsGcHtNode* nodep = NULL;
	for(i=0;i<JS_GC_HASH_TABLE_SIZE;++i){
		nodep = table[i];
		while(nodep!=NULL && nodep->count == -1){
			nodep->count = 0;
			nodep = nodep->next;
		}
	}
}
/*释放节点, 和关联的mp 空间*/
static void JsGcFreeMainHtNode(){
	int i;
	struct JsGcHtNode** nodepp = NULL;
	for(i=0;i<JS_GC_HASH_TABLE_SIZE;++i){
		nodepp = &JsGcMainHt[i];
		while(*nodepp != NULL){
			if((*nodepp)->count != -1){
				//对于未命中的节点++
				(*nodepp)->count++;
				//printf("The Node %p Miss: %d \n",(*nodepp),(*nodepp)->count);
				if((*nodepp)->count >= JS_GC_MISS){
					
					//符合释放条件, 则释放内存
					void* mp = (*nodepp)->mp;
					struct JsGcHtNode* nodep = *nodepp;
					struct JsGcBlock* bp ;
					//删除以该地址为Key的条目(TRY，可能不是Key)
					JsGcBurnKey(mp);
					JS_MP2BP(mp,bp);
					//减少空间记录
					JsGcMemory -= bp->size;
					//调用该空间的释放函数
					if(bp->freeFn!=NULL)
						(*bp->freeFn)(mp,bp->size);
					//释放内存
					free(bp);
					free(nodep);
					//修改nodepp,指向该节点的下一个位置
					*nodepp = (*nodepp)->next;
				}else{
					//未到回收标准, 等待下一次回收
					nodepp = &(*nodepp)->next;
				}
			}else{
				//节点被mark, 说明不需要释放, 直接跳到下一个节点
				nodepp = &(*nodepp)->next;
			}
		}
	}

}
void JsGcCommit0(struct JsGcTlsNode* node){
	if(node == NULL)
		return;
	pthread_mutex_lock(JsGcLock);
	//获取Tls对象
	pthread_mutex_lock(node->lock);
	struct JsGcHtNode **tlsHt  = node->table;
	struct JsGcHtNode **mainHt = JsGcMainHt;
	int i;
	for(i=0;i<JS_GC_HASH_TABLE_SIZE;++i){
		//归类同hashcode的MemNode
		if(tlsHt[i] != NULL){
			struct JsGcHtNode *  mp = mainHt[i];
			struct JsGcHtNode ** mpp = &tlsHt[i];
			//查询到TlsHt[i]的最后一项
			while((*mpp)->next != NULL){
				mpp = &(*mpp)->next;
			}
			//指向mainHt[i]原先的表项
			(*mpp)->next = mp;
			//设置mainHt[i]为TlsHt[i]的对象
			mainHt[i] = tlsHt[i];
			//清空表项
			tlsHt[i] = NULL;
		}
	}
	//刷入Tls中申请内存大小, 并设置大小为0
	JsGcMemory += node->size;
	node->size = 0;
	pthread_mutex_unlock(node->lock);
	pthread_mutex_unlock(JsGcLock);
}



/*收到Gc信号(USR1)的时候, 调用的函数, 停止线程, 并且等待Gc结束(cond)*/
static void JsGcSigHandler(int sig){
	pthread_mutex_lock(JsGcStopLock);
	pthread_cond_wait(&JsGcStopCond,JsGcStopLock);
	pthread_mutex_unlock(JsGcStopLock);
}
static void JsGcTBegin(){
	JsGcBtTls();
}
static void JsGcBtTls(){
	struct JsGcTlsNode*  node = (struct JsGcTlsNode*)pthread_getspecific(*JsGcTlsKey);
	if(node != NULL){
		//已经初始化过了, 直接返回
		return;
	}
	pthread_mutex_lock(JsGcLock);
	//申请HashTable空间
	struct JsGcHtNode** table = (struct JsGcHtNode**) malloc(sizeof(struct JsGcHtNode*) * JS_GC_HASH_TABLE_SIZE);
	//清空
	memset(table,0,sizeof(struct JsGcHtNode*) * JS_GC_HASH_TABLE_SIZE);

	node = (struct JsGcTlsNode* )malloc(sizeof(struct JsGcTlsNode));
	node->tid = pthread_self();
	node->table = table;
	node->size = 0;
	//配置计数锁
	node->lock = (pthread_mutex_t*) malloc(sizeof(pthread_mutex_t));
	pthread_mutexattr_t lockAttr;
	pthread_mutexattr_setpshared(&lockAttr, PTHREAD_PROCESS_PRIVATE);
	pthread_mutexattr_settype(&lockAttr ,PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(node->lock,&lockAttr);
	
	//放在 JsGcTlsList 第一个位置
	node->next = JsGcTlsList;
	JsGcTlsList = node;
	//加入到TLS中
	pthread_setspecific(*JsGcTlsKey,node);
	pthread_mutex_unlock(JsGcLock);
}
/*线程退出, JsGcHtKey的后置函数*/
static void JsGcEtTls(void *data){
	//data 可以等于NULL
	pthread_mutex_lock(JsGcLock);
	//把Tls中HtTable的数据和大小刷入主存
	JsGcCommit0((struct JsGcTlsNode*)data);
	//删除JsGcTlsList的该TID的点
	pthread_t tid = pthread_self();
	struct JsGcTlsNode** pp = &JsGcTlsList;
	while(*pp!=NULL){
		if((*pp)->tid == tid){
			struct JsGcTlsNode* p = *pp;
			//节点重新链接
			*pp =  (*pp)->next;
			//释放该内存
			free(p->table);
			//锁释放
			pthread_mutex_destroy(p->lock);
			free(p->lock);
			free(p);
			break;
		}
		//选择下一个节点
		pp =  &(*pp)->next;
	}
	pthread_mutex_unlock(JsGcLock);
}

//模块初始化API
static void JsPrevInitGc(){
	void* table = NULL;
	int size = 0;
	//初始化JsGcKeyMan
	JsGcKeyMan = (struct JsGcMan*)malloc(sizeof(struct JsGcMan));
	JsGcKeyMan->total = JS_GC_KEY_TABLE_SIZE;
	JsGcKeyMan->used =  0;
	
	size = sizeof(struct JsGcKey*) * JS_GC_KEY_TABLE_SIZE;
	table = malloc(size);
	memset(table,0,size);
	JsGcKeyMan->table = table;
	
	//初始化JsGcRootMan
	JsGcRootMan = (struct JsGcMan*)malloc(sizeof(struct JsGcMan));
	JsGcRootMan->total = JS_GC_ROOT_TABLE_SIZE;
	JsGcRootMan->used = 0;
	
	size = sizeof(struct JsGcRoot*) * JS_GC_ROOT_TABLE_SIZE;
	table = malloc(size);
	memset(table,0,size);
	JsGcRootMan->table = table;
	
	//初始化HashTable
	memset(JsGcMainHt,0,sizeof(struct JsGcHtNode*) * JS_GC_HASH_TABLE_SIZE);
	

	//计数锁属性
	pthread_mutexattr_t lockAttr;
	pthread_mutexattr_setpshared(&lockAttr, PTHREAD_PROCESS_PRIVATE);
	pthread_mutexattr_settype(&lockAttr ,PTHREAD_MUTEX_RECURSIVE);
	
	//初始化JsGcLock, 配置一个计数类型的Lock
	JsGcLock = (pthread_mutex_t*) malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(JsGcLock,&lockAttr);
	
	//初始化JsGcStopLock为计数类型Lock
	JsGcStopLock = (pthread_mutex_t*) malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(JsGcStopLock,&lockAttr);
	
	//初始化HashTable Tls Key
	JsGcTlsKey =(pthread_key_t*) malloc(sizeof(pthread_key_t));
	pthread_key_create(JsGcTlsKey, &JsGcEtTls);


	
	//设置GC信号量
	struct sigaction act, oldact;
	act.sa_handler = JsGcSigHandler;
	sigfillset(&act.sa_mask);
	act.sa_flags = 0; //不可打断类型
	sigaction(SIGUSR1, &act, &oldact);
	
	//开启主线程Gc功能
	JsGcTBegin();
	
}

static void JsPostInitGc(){
	
	
	//struct sched_param sched;
	//sched.sched_priority = 18;//正常为20
	pthread_t pid;
	int err = pthread_create(&pid,NULL,&JsGcThread,NULL);
	//低优先级
	//pthread_setschedparam( pid, SCHED_RR,&sched );
	if(err != 0){
		//开启Gc线程不成功
		JsAssert(FALSE);
	}
}
/*************************锁模块*********************/

//配置一个程序内部使用, 可以计数的锁的属性
static pthread_mutexattr_t* JsLockAttr = NULL;
static void JsPrevInitLock();
static void JsInitLockAttr();

static void JsGcFreeLock(void* mp,int size);
/*--------------------------------------------------*/


JsLock JsCreateLock(){
	pthread_mutex_t* a = (pthread_mutex_t*) JsGcMalloc(sizeof(pthread_mutex_t),NULL,&JsGcFreeLock);
	pthread_mutex_init(a,JsLockAttr);
	return a;
}
void JsLockup(JsLock lock){
	JsAssert(lock != NULL);
	pthread_mutex_lock((pthread_mutex_t*)lock);
}
void JsUnlock(JsLock lock){
	JsAssert(lock != NULL);
	pthread_mutex_unlock((pthread_mutex_t*)lock);  
}
/*--------------------------------------------------*/
static void JsInitLockAttr(){
	JsLockAttr = (pthread_mutexattr_t*) JsGcMalloc(sizeof(pthread_mutexattr_t),NULL,NULL);
	pthread_mutexattr_init(JsLockAttr);
	pthread_mutexattr_setpshared(JsLockAttr, PTHREAD_PROCESS_PRIVATE);
	pthread_mutexattr_settype(JsLockAttr ,PTHREAD_MUTEX_RECURSIVE);
	//挂为Root
	JsGcRegistKey(JsLockAttr,"LockAttr");
	JsGcMountRoot(JsLockAttr,JsLockAttr);
}
static void JsPrevInitLock(){
	JsInitLockAttr();	
};

static void JsGcFreeLock(void* mp,int size){
	pthread_mutex_t* lock = (pthread_mutex_t*)mp;
	pthread_mutex_destroy(lock);
}


/********************************线程模块*****************************/
//代理函数的pass数据
struct JsProxyData{
	JsThreadFn fn;
	void* data;
};
//主要目的是开启子线程的GC功能
static void* JsProxyThread(void* data);

//返回线程相关信息
JsThread JsCurThread(){
	pthread_t* p = (pthread_t *)JsGcMalloc(sizeof(pthread_t),NULL,NULL);
	*p = pthread_self();
	return p;
}
JsThread JsStartThread(JsThreadFn fn,void* data){
	pthread_t* p = (pthread_t *)JsGcMalloc(sizeof(pthread_t),NULL,NULL);
	//代理函数, 内存由ProxyThread释放
	struct JsProxyData* proxy = (struct JsProxyData*)malloc(sizeof(struct JsProxyData));
	proxy->fn = fn;
	proxy->data = data;
	
	int err = pthread_create(p,NULL,&JsProxyThread,proxy); 
	if(err !=0){
		return NULL;
	}
	return p;
}
void JsCloseSelf(){
	pthread_exit(NULL);
}
void JsCloseThread(JsThread thread){
	if(thread == NULL)
		return ;
	pthread_t* p = (pthread_t*)thread;
	pthread_cancel(*p);
}
void JsSleep(long time){
	if(time < 0)
		return;
	usleep(time*1000);
}
void JsYield(){
	sched_yield();
}
void JsJoin(JsThread thread){
	void* status;
	JsAssert(thread != NULL);
	pthread_t* tid = (pthread_t*)thread;
	pthread_join(*tid,&status); 
}
void JsHalt(){
	exit(0);
}


static void* JsProxyThread(void* data){
	//开启Gc功能的函数
	JsGcTBegin();
	struct JsProxyData* proxy = (struct JsProxyData*)data;
	JsThreadFn pFn = proxy->fn;
	void* pD = proxy->data;
	//释放Proxy内存
	free(data);
	return (*pFn)(pD);
}
/****************************TLS 模块*******************************/
/*把数据存储在当前TLS中*/
JsTlsKey JsCreateTlsKey(JsTlsFn fn){
	pthread_key_t * key = (pthread_key_t*)JsGcMalloc(sizeof(pthread_key_t),NULL,NULL);
	pthread_key_create( key, fn);
	return key;
}
void JsSetTlsValue(JsTlsKey key, void* value){
	JsAssert(key != NULL);
	pthread_setspecific( *(pthread_key_t*)key,value);
}
void* JsGetTlsValue(JsTlsKey key){
	JsAssert(key != NULL);
	return pthread_getspecific( *(pthread_key_t*)key);
}


/*************************初始化API**********************/

void JsPrevInitSys(){
	//按模块出现的顺序排列
	JsPrevInitGc();
	JsPrevInitLock();
}
void JsPostInitSys(){
	JsPostInitGc();
}