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
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>

/***********************************内存管理模块 API*******************************/

//初始化Hash表大小
#define JS_HASH_TABLE_SIZE 	1024
//Key Table初始化大小
#define JS_KEY_TABLE_SIZE 	64
//Root Table初始化大小
#define JS_ROOT_TABLE_SIZE  1024
//内存超过该上限, Engine在轮转的时候会停止等待GC
//(单位 : MB)
#define JS_MEMORY_LINE 		128

/*BP 和 MP 转换宏*/
#define JS_MP2BP(mp,bp)				\
	do{								\
		bp = (struct JsBlock*)mp;	\
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
	bp--> -------------------
		 |Fn|int|    mem    |
		 -------------------
				|
		 mp---->|
		 
KeyTable:
	  JsMan->table-->----------
	                 | JsKey*  |
					 ----------
					 | JsKey*  |
					 ----------
					 | JsKey*  |
					 ----------
					 | JsKey*  |
					 ----------
				
RootTable:
	   JsMan->table->--------
	                 | JsRoot* |
					 ---------
					 | JsRoot* |
					 ---------
					 | JsRoot* |
					 ---------
					 | JsRoot* |
					 ---------
					 
HASH_TABLE---->|
			   ---------------
			   |   JsHtNode*   |
			   ----------------        --------------------
			   |   JsHtNode*   | ----->|Mark | mp | next |
			   ----------------       --------------------
			   |   JsHtNode*   |
			   ----------------
					.
					.
					.
				---------------
			   |   JsHtNode*   |
			   ----------------
*/

struct JsMan{
	//全部空间大小
	int total;
	//已经被使用的大小
	int used;
	void* table;
};

struct JsKey{
	void* key;
	char* desc;
};

struct JsRoot{
	void* mp; //Mem内存指针
	void* key;// 有效key
};

struct JsHtNode{
	int mark;
	void* mp;
	struct JsHtNode* next;
};
/*申请内存时, 添加的头*/
struct JsBlock{
	JsGcMarkFn markFn; //内存关联mark函数
	JsGcFreeFn freeFn; //内存关联析构函数
	int size; //申请内存的大小
};

/*Hash Table*/
static struct JsHtNode* JsHashTable[JS_HASH_TABLE_SIZE] = {NULL};

/*Key Manager*/
static struct JsMan* JsKeyMan;

/*RootManager*/
static struct JsMan* JsRootMan;

/* Gc Lock, 在对GC相关数据进行修改的时候, 需要加锁 */
static pthread_mutex_t* JsGcLock = NULL; 
/*申请内存大小计数(byte), 可以计数4GB的内存空间*/
static unsigned int JsMemory = 0;
/* 
	请求进行GC工作的标志, 只有先有这个请求, GC才会在满足条件的时候进行GC,
	完成GC工作后, 需要重置为 FALSE, 当为TRUE的时候, 表示请求进行GC工作
*/
static int JsGcReq  = FALSE;

/*Trap Lock*/
static pthread_mutex_t JsGcTrapLock = PTHREAD_MUTEX_INITIALIZER;
/*当Gc完成的时候, 需要使用pthread_cond_broadcast(&JsGcTrapCond) 来激活等待Gc完成的函数*/
static pthread_cond_t JsGcTrapCond 	= PTHREAD_COND_INITIALIZER;

/*-------------------------------------------------------------------*/

/*Gc模块初始化函数*/
static void JsPrevInitGc();
static void JsPostInitGc();

/*将mp插入到hashtable中*/
static int JsHashInsert(void* mp);
/*将mp从hashTable中删除*/
static int JsHashRemove(void* mp);
/*
	根据mp 查到 JsHtNode
	返回:
		NULL 	: 不在HashTable中
		!NULL	: 正常数据
*/
static struct JsHtNode* JsFindHtNode(void* mp);
/*计算mp指针对应的hashcode*/
static int JsHashCode(void* mp);
/*把所有标记都清空 为 FALSE*/
static void JsUnMarkHashTable();

/*GC线程*/
static void* JsGcThread(void* data);
/*进行GC工作*/
static void JsDoGcWrok();
/*-------------------------------------------------------------------*/


void* JsMalloc(int size){
	if( size <= 0 )  
		return NULL;
	void * p = malloc(size);
	JsAssert(p != NULL);
	memset(p,0,size);
	return p;
}
void* JsReAlloc(void* mem,int newSize){
	void* p = realloc(mem,newSize);
	JsAssert(p != NULL);
	return p;
}


/*
	委托到GC管理的内存中
		size must > 0
		markFm = NULL 的时候, 表示配置的内存空间为原生型 如int*,double* 等
		freeFn 为在回收该内存的时候, 执行的析构动作
	记录内存大小到JsMemory 和 Block中
*/
void* JsGcMalloc(int size,JsGcMarkFn markFn,JsGcFreeFn freeFn){

	pthread_mutex_lock(JsGcLock);
	JsAssert(size > 0);
	/*block + size*/
	struct JsBlock* bp = malloc(size + sizeof(struct JsBlock));
	JsAssert(bp != NULL);
	//clear
	memset(bp,0,size + sizeof(struct JsBlock));
	/*set Block */
	bp->markFn = markFn;
	bp->freeFn = freeFn;
	bp->size = size;
	/*将MP插入到Hash表中进行管理*/
	void* mp;
	JS_BP2MP(bp,mp);
	JsHashInsert(mp);
	/*记录内存申请大小*/
	JsMemory += size;
	
	pthread_mutex_unlock(JsGcLock);
	return mp;
}
/*
	Gc空间realloc大小
	记录新大小到Block和JsMemory中
*/
void* JsGcReAlloc(void* mp0,int newSize){

	JsAssert(mp0 != NULL && newSize > 0);
	
	pthread_mutex_lock(JsGcLock);
	void *mp1;
	struct JsBlock *bp0, *bp1;
	JS_MP2BP(mp0,bp0);
	/*删除该内存记录大小*/
	JsMemory -= bp0->size;
	/*block + size*/
	bp1 = realloc(bp0,newSize + sizeof(struct JsBlock));
	JsAssert(bp1 != NULL);
	//fn不变
	bp1->size = newSize;
	
	//block内存地址被修改了
	if(bp1 != bp0){
		JS_BP2MP(bp1,mp1);
		//删除原先Hash表中该对象
		JsHashRemove(mp0);
		JsHashInsert(mp1);
	}
	/*添加新增内存大小*/
	JsMemory += newSize;
	
	pthread_mutex_unlock(JsGcLock);
	return mp1;

}
/*标记指向内存空间为可用, 并且调用申请该内存空间时候, 配置的fn*/
int JsGcDoMark(void* mp){
	if(mp == NULL)
		return 0;
	pthread_mutex_lock(JsGcLock);
	struct JsHtNode* node = JsFindHtNode(mp);
	if(node == NULL){
		pthread_mutex_unlock(JsGcLock);
		return -1;
	}
	if(node->mark == TRUE){
		pthread_mutex_unlock(JsGcLock);
		return 1;
	}
	//Mark the node
	node->mark = TRUE;
	struct JsBlock* bp = NULL;
	JS_MP2BP(mp,bp);
	//调用该mp对应的mark函数, 用于mark 和该内存快关联的内存
	if(bp->markFn != NULL)
		(*bp->markFn)(mp);
	pthread_mutex_unlock(JsGcLock);
	return 0;
}
/*重新配置该内存指向的mark函数*/
void JsGcSetMarkFn(void* mp,JsGcMarkFn markFn){
	pthread_mutex_lock(JsGcLock);
	JsAssert(mp!=NULL);
	struct JsBlock* bp;
	JS_MP2BP(mp,bp);
	bp->markFn = markFn;
	pthread_mutex_unlock(JsGcLock);
}
/*重新配置该内存指向的free函数*/
void JsGcSetFreeFn(void* mp,JsGcMarkFn freeFn){
	pthread_mutex_lock(JsGcLock);
	JsAssert(mp!=NULL);
	struct JsBlock* bp;
	JS_MP2BP(mp,bp);
	bp->freeFn = freeFn;
	pthread_mutex_unlock(JsGcLock);
}
/*
	Gc中添加一个有效的Key类型(指针类型),Key != 0
*/
int JsGcRegistKey(void* key,const char* desc){

	pthread_mutex_lock(JsGcLock);
	JsAssert(key != NULL);
	struct JsKey** keypp ;
	int i,size;
	
	
	//查询是否已经存在
	keypp = (struct JsKey** ) JsKeyMan->table;
	size = JsKeyMan->total;
	for(i=0 ; i < size ; ++i){
		if(keypp[i] != NULL && keypp[i]->key == key){
			//已经存在Key, 返回
			pthread_mutex_unlock(JsGcLock);
			return 0;
		}
	}
	//KeyTable中没有存在该Key, 则添加
	//判断是否已经满了
	if(JsKeyMan->total == JsKeyMan->used){
		
		//重新配置total参数
		int oldTotal =  JsKeyMan->total;
		int newTotal =  oldTotal * 2;
		
		//重新配置table空间大小
		int oldSize = sizeof(struct JsKey*) * oldTotal;
		int newSize = sizeof(struct JsKey*) * newTotal;
		
		//重新申请
		void* table = realloc(JsKeyMan->table,newSize);
		//刷新新申请空间
		memset(table + oldTotal , 0, newSize - oldSize);
		
		JsKeyMan->table = table;
		JsKeyMan->total = newTotal;
		//used 不变
	}
	//空间满足需求, 寻找第一个NULL的空间
	keypp = (struct JsKey** ) JsKeyMan->table;
	size = JsKeyMan->total;
	for( i = 0 ; i < size; ++i){
		if(keypp[i]== NULL){
			//申请空间
			keypp[i] = (struct JsKey*)malloc(sizeof(struct JsKey));
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
	JsKeyMan->used ++;
	
	pthread_mutex_unlock(JsGcLock);
	return 1;
}
/*Gc中删除一个Key , Key != 0, 并且删除RootTable中相关Root*/
void JsGcBurnKey(void* key){
	pthread_mutex_lock(JsGcLock);
	JsAssert(key != NULL);
	struct JsKey** keypp ;
	struct JsRoot** rootpp;
	int i,size;
	
	
	//剔除KeyTable
	keypp = (struct JsKey** ) JsKeyMan->table;
	size = JsKeyMan->total;
	for(i=0 ; i < size ; ++i){
		if(keypp[i] != NULL && keypp[i]->key == key){
			//释放该空间
			if(keypp[i]->desc != NULL)
				free(keypp[i]->desc);
			free(keypp[i]);
			//清除为NULL
			keypp[i] = NULL;
			//计数--
			JsKeyMan->used--;
			//KeyTable不再存在和key相关的
			break;
		}
	}
	//剔除RootTable中相关点
	rootpp = (struct JsRoot**)JsRootMan->table;
	size = JsRootMan->total;
	for(i=0 ; i < size ; ++i){
		if(rootpp[i] != NULL && rootpp[i]->key == key){
			//释放内存空间
			free(rootpp[i]);
			//清除为NULL
			rootpp[i] = NULL;
			//计数--
			JsRootMan->used--;
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
	struct JsKey** keypp ;
	struct JsRoot** rootpp;
	int i,size;
	int flag ;
	
	//查询KeyTable中是否存在key
	flag = FALSE;
	keypp = (struct JsKey** ) JsKeyMan->table;
	size = JsKeyMan->total;
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
	rootpp = (struct JsRoot**)JsRootMan->table;
	size = JsRootMan->total;
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
	if(JsRootMan->total == JsRootMan->used){
		
		//重新配置total参数
		int oldTotal =  JsRootMan->total;
		int newTotal =  oldTotal * 2;
		
		//重新配置table空间大小
		int oldSize = sizeof(struct JsRoot*) * oldTotal;
		int newSize = sizeof(struct JsRoot*) * newTotal;
		
		//重新申请
		void* table = realloc(JsRootMan->table,newSize);
		//刷新新申请空间
		memset(table + oldTotal , 0, newSize - oldSize);
		
		JsRootMan->table = table;
		JsRootMan->total = newTotal;
		//used 不变
	}
	//空间满足需求, 寻找第一个NULL的空间
	rootpp = (struct JsRoot**)JsRootMan->table;
	size = JsRootMan->total;
	for(i=0 ; i < size ; ++i){
		if(rootpp[i] != NULL){
			rootpp[i] = (struct JsRoot*)malloc(sizeof(struct JsRoot));
			rootpp[i]->mp = mp;
			rootpp[i]->key = key;
			break;
		}
	}
	//被使用空间++
	JsKeyMan->used ++;
	
	pthread_mutex_unlock(JsGcLock);

}

/*
	挂起线程等待Gc结束:
		调用TrapGc的时候, 检测到需要进行Gc, 则调用该函数, 完成等待Gc
	前需要完成的工作, fn = NULL , 则表示没有要做的
*/
void JsTrapGc(JsGcTrapFn fn,void* data){
	
	pthread_mutex_lock(JsGcLock);

	int line = JsMemory / 1024;
	if(JsGcReq == TRUE ||line >= JS_MEMORY_LINE){
		//标记为请求GC
		JsGcReq = TRUE;
		pthread_mutex_unlock(JsGcLock);
		//调用进入Gc前准备函数
		if(fn!=NULL)
			(*fn)(data);
		//等待Gc完成的信号
		pthread_mutex_lock(&JsGcTrapLock);
		pthread_cond_wait(&JsGcTrapCond, &JsGcTrapLock);
		pthread_mutex_unlock(&JsGcTrapLock);
	}else{
		//不需要GC
		pthread_mutex_unlock(JsGcLock);
	}
}
/*-------------------------------------------------------------------*/
static int JsHashInsert(void* mp){
	JsAssert(mp != NULL);
	int hashCode = JsHashCode(mp);
	//新节点空间
	struct JsHtNode* nodep = (struct JsHtNode*)malloc(sizeof(struct JsHtNode));
	nodep->mark = FALSE;
	nodep->next = NULL;
	nodep->mp = mp;
	
	struct JsHtNode** nodepp = &JsHashTable[hashCode];
	//最后位置
	while(*nodepp != NULL){
		nodepp = &(*nodepp)->next;
	}
	*nodepp = nodep;
	return TRUE;
}
/*将mp从hashTable中删除*/
static int JsHashRemove(void* mp){
	JsAssert(mp != NULL);
	int hashCode = JsHashCode(mp);
	struct JsHtNode** nodepp = &JsHashTable[hashCode];
	while(*nodepp != NULL ){
		if((*nodepp)->mp == mp){
			//记录要删除的node对象
			struct JsHtNode* nodep = (*nodepp);
			//找到该对象
			(*nodepp) = (*nodepp)->next;
			free(nodep);
			break;
		}
		nodepp = &(*nodepp)->next;
	}
	return TRUE;
}

static struct JsHtNode* JsFindHtNode(void* mp){
	JsAssert(mp != NULL);
	int hashCode = JsHashCode(mp);
	struct JsHtNode** nodepp = &JsHashTable[hashCode];
	while(*nodepp != NULL ){
		if((*nodepp)->mp == mp){
			return *nodepp;
		}
		nodepp = &(*nodepp)->next;
	}
	return NULL;
}
static int JsHashCode(void* mp){
	if(mp == NULL)
		return 0;
	char buf[128];
	sprintf(buf,"%p",mp);
	char* pEnd;
	int code;
	code = strtol(buf,&pEnd,0);
	code %= JS_HASH_TABLE_SIZE;
	return code;
}

static void JsUnMarkHashTable(){
	int i;
	struct JsHtNode* nodep = NULL;
	for(i=0;i<JS_HASH_TABLE_SIZE;++i){
		nodep = JsHashTable[i];
		while(nodep!=NULL){
			nodep->mark = FALSE;
			nodep = nodep->next;
		}
	}
}
//Gc线程
static void* JsGcThread(void* data){
	while(1){
		//检测是否已经请求Gc操作
		if(JsGcReq == TRUE){
			pthread_mutex_lock(JsGcLock);
			//检测VM->engines的状态是否符合条件
			struct JsVm* vm =  JsGetVm();
			pthread_mutex_lock((pthread_mutex_t*)vm->lock);
			int size = JsListSize(vm->engines);
			int i;
			int flag = TRUE;
			for( i = 0; i < size ; ++i){
				struct JsEngine* e = JsListGet(vm->engines,i);
				if(!(e->state == JS_ENGINE_IDLE || e->state == JS_ENGINE_GC)){
					// 存在一个不可以回收的Engine状态
					flag = FALSE;
					break;
				}
			}
			if(flag){
				//进行Gc工作
				JsDoGcWrok();
			}
			pthread_mutex_unlock((pthread_mutex_t*)vm->lock);
			pthread_mutex_unlock(JsGcLock);
		}
		//休眠该线程
		sleep(1);
	}
	return NULL;
};
static void JsDoGcWrok(){


}

//模块初始化API
static void JsPrevInitGc(){
	void* table = NULL;
	int size = 0;
	//初始化JsKeyMan
	JsKeyMan = (struct JsMan*)malloc(sizeof(struct JsMan));
	JsKeyMan->total = JS_KEY_TABLE_SIZE;
	JsKeyMan->used =  0;
	
	size = sizeof(struct JsKey*) * JS_KEY_TABLE_SIZE;
	table = malloc(size);
	memset(table,0,size);
	JsKeyMan->table = table;
	
	//初始化JsRootMan
	JsRootMan = (struct JsMan*)malloc(sizeof(struct JsMan));
	JsRootMan->total = JS_ROOT_TABLE_SIZE;
	JsRootMan->used = 0;
	
	size = sizeof(struct JsRoot*) * JS_ROOT_TABLE_SIZE;
	table = malloc(size);
	memset(table,0,size);
	JsRootMan->table = table;
	
	//初始化HashTable
	memset(JsHashTable,0,sizeof(struct JsHtNode*) * JS_HASH_TABLE_SIZE);
	
	//初始化JsGcLock, 配置一个计数类型的Lock
	JsGcLock = (pthread_mutex_t*) malloc(sizeof(pthread_mutex_t));

	pthread_mutexattr_t lockAttr;
	pthread_mutexattr_setpshared(&lockAttr, PTHREAD_PROCESS_PRIVATE);
	pthread_mutexattr_settype(&lockAttr ,PTHREAD_MUTEX_RECURSIVE);

	pthread_mutex_init(JsGcLock,&lockAttr);
	
}
static void JsPostInitGc(){

	struct sched_param sched;
	sched.sched_priority = -1;
	pthread_t  pid  ;
	int err = pthread_create(&pid,NULL,&JsGcThread,NULL);
	//低优先级
	pthread_setschedparam( pid, SCHED_RR,&sched );
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

/*--------------------------------------------------*/


JsLock JsCreateLock(){
	pthread_mutex_t* a = (pthread_mutex_t*) JsMalloc(sizeof(pthread_mutex_t));
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
void JsDestroyLock(JsLock* lock){
	if(lock == NULL || *lock == NULL)
		return;
	pthread_mutex_t** p = (pthread_mutex_t**)lock;
	pthread_mutex_destroy(*p);
	*lock = NULL;
}
/*--------------------------------------------------*/
static void JsInitLockAttr(){
	JsLockAttr = (pthread_mutexattr_t*) JsMalloc(sizeof(pthread_mutexattr_t));
	pthread_mutexattr_init(JsLockAttr);
	pthread_mutexattr_setpshared(JsLockAttr, PTHREAD_PROCESS_PRIVATE);
	pthread_mutexattr_settype(JsLockAttr ,PTHREAD_MUTEX_RECURSIVE);
}
static void JsPrevInitLock(){
	JsInitLockAttr();	
};




/********************************线程模块*****************************/
//返回线程相关信息
JsThread JsCurThread(){
	pthread_t* p = (pthread_t *)JsMalloc(sizeof(pthread_t));
	*p = pthread_self();
	return p;
}
JsThread JsStartThread(JsThreadFn fn,void* data){
	pthread_t* p = (pthread_t *)JsMalloc(sizeof(pthread_t));
	int err = pthread_create(p,NULL,fn,data); 
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

/****************************TLS 模块*******************************/
/*把数据存储在当前TLS中*/
JsTlsKey JsCreateTlsKey(JsTlsFn fn){
	pthread_key_t * key = (pthread_key_t*)JsMalloc(sizeof(pthread_key_t));
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