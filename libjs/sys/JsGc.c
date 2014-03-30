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
#include"JsGc.h"
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>

//初始化Hash表大小
#define JS_HASH_TABLE_SIZE 	1024
//Key Table初始化大小
#define JS_KEY_TABLE_SIZE 	64
//Root Table初始化大小
#define JS_ROOT_TABLE_SIZE  1024

/*Bp 和 Rp 转换宏*/
#define JS_RP2BP(rp,bp)		\
	do{						\
		bp = (void*)rp;		\
		bp--;				\
	}while(0)
	
#define JS_BP2RP(bp,rp) 	\
	do{						\
		rp = (void*)bp;		\
		rp++;				\
	}while(0)

	
/**

内存分布结构:

Block:
	bp--> -------------------
		 |  Fn  |    size   |
		 -------------------
				|
		 rp---->|
		 
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
			   |   JsHtNode*   | ----->|Mark | rp* | next |
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
	void* rp; //指向一段block的rp
	void* key;// 有效key
};

struct JsHtNode{
	int mark;
	void* rp;
	struct JsHtNode* next;
};
/*Hash Table*/
static struct JsHtNode* JsHashTable[JS_HASH_TABLE_SIZE] = {NULL};

/*Key Manager*/
static struct JsMan* JsKeyMan;

/*RootManager*/
static struct JsMan* JsRootMan;

/* Gc Lock */
static JsLock JsGcLock = NULL; 

/***********************************************************************/
/*将rp插入到hashtable中*/
static int JsHashInsert(void* rp);
/*将rp从hashTable中删除*/
static int JsHashRemove(void* rp);
/*
	根据rp 查到 JsHtNode
	返回:
		NULL 	: 不在HashTable中
		!NULL	: 正常数据
*/
static struct JsHtNode* JsFindHtNode(void* rp);
/*计算rp指针对应的hashcode*/
static int JsHashCode(void* rp);
/*把所有标记都清空 为 FALSE*/
static void JsUnMarkHashTable();

/***********************************************************************/

//模块初始化API
void JsPrevInitGc(){
	void* table = NULL;
	int size = 0;
	//初始化JsKeyManager
	JsKeyMan = (struct JsMan*)malloc(sizeof(struct JsMan));
	JsKeyMan->total = JS_KEY_TABLE_SIZE;
	JsKeyMan->used =  0;
	
	size = sizeof(struct JsKey*) * JS_KEY_TABLE_SIZE;
	table = malloc(size);
	memset(table,0,size);
	JsKeyMan->table = table;
	
	//初始化JsRootManager
	JsRootMan = (struct JsMan*)malloc(sizeof(struct JsMan));
	JsRootMan->total = JS_ROOT_TABLE_SIZE;
	JsRootMan->used = 0;
	
	size = sizeof(struct JsRoot*) * JS_ROOT_TABLE_SIZE;
	table = malloc(size);
	memset(table,0,size);
	JsRootMan->table = table;
	
	//初始化HashTable
	memset(JsHashTable,0,sizeof(struct JsHtNode*) * JS_HASH_TABLE_SIZE);
	
	//初始化Lock, 并且添加为Root
	JsGcLock = JsCreateLock();
	JsGcRegistKey(&JsGcLock,"JsGcLock");
	JsGcMountRoot(JsGcLock,&JsGcLock);
	
}
void JsPostInitGc(){
	//开启Gc Thread

}

/*委托到GC管理的内存中, fn = NULL 的时候, 表示配置的内存空间为原生型*/
void* JsGcMalloc(int size,JsGcMarkFn fn){

	JsLockup(JsGcLock);
	JsAssert(size > 0);
	/*fn + size*/
	void* bp = malloc(size + sizeof(JsGcMarkFn));
	JsAssert(bp != NULL);
	/*set MarkFn */
	JsGcMarkFn* mkf =(JsGcMarkFn*) bp;
	*mkf = fn;
	void * rp = NULL;
	JS_BP2RP(bp,rp);
	JsHashInsert(rp);
	
	JsUnlock(JsGcLock);
	return rp;
}
/*Gc空间realloc大小*/
void* JsGcReAlloc(void* rp0,int newSize){
	JsAssert(rp0 != NULL && newSize > 0);
	JsLockup(JsGcLock);
	void *bp0, *bp1, *rp1;
	JS_RP2BP(rp0,bp0);
	/*fn + size*/
	bp1 = realloc(bp0,newSize + sizeof(JsGcMarkFn));
	JsAssert(bp1 != NULL);
	JS_BP2RP(bp1,rp1);
	
	if(bp1 != bp0){
		//block内存地址被修改了
		JsHashRemove(rp0);
		JsHashInsert(rp1);
	}
	JsUnlock(JsGcLock);
	return rp1;

}
/*标记指向内存空间为可用, 并且调用申请该内存空间时候, 配置的fn*/
int JsGcDoMark(void* rp){
	if(rp == NULL)
		return 0;
	JsLockup(JsGcLock);
	struct JsHtNode* node = JsFindHtNode(rp);
	if(node == NULL){
		JsUnlock(JsGcLock);
		return -1;
	}
	if(node->mark == TRUE){
		JsUnlock(JsGcLock);
		return 1;
	}
	//Mark the node
	node->mark = TRUE;
	void* bp = NULL;
	JS_RP2BP(rp,bp);
	JsGcMarkFn* fn = (JsGcMarkFn*)bp;
	//调用该rp对饮的mark函数, 用于mark 和该内存快关联的内存
	(**fn)(rp);
	JsUnlock(JsGcLock);
	return 0;
}
/*重新配置该内存指向的mark函数*/
void JsGcSetMarkFn(void* rp,JsGcMarkFn fn){
	JsLockup(JsGcLock);
	JsAssert(rp!=NULL);
	void* bp;
	JS_RP2BP(rp,bp);
	JsGcMarkFn* mkf = (JsGcMarkFn*)bp;
	*mkf = fn;
	JsUnlock(JsGcLock);
}
/*
	Gc中添加一个有效的Key类型(指针类型),Key != 0
*/
int JsGcRegistKey(void* key,const char* desc){

	JsLockup(JsGcLock);
	JsAssert(key != NULL);
	struct JsKey** keypp ;
	int i,size;
	
	
	//查询是否已经存在
	keypp = (struct JsKey** ) JsKeyMan->table;
	size = JsKeyMan->total;
	for(i=0 ; i < size ; ++i){
		if(keypp[i] != NULL && keypp[i]->key == key){
			//已经存在Key, 返回
			JsUnlock(JsGcLock);
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
	
	JsUnlock(JsGcLock);
	return 1;
}
/*Gc中删除一个Key , Key != 0, 并且删除RootTable中相关Root*/
void JsGcBurnKey(void* key){
	JsLockup(JsGcLock);
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
	JsUnlock(JsGcLock);
}
/*	
	Gc扫描的时候, Root节点配置, 会检测是否存在KeyTable
	是否存在table, 和 RootTable是否存在相同Root
*/
void JsGcMountRoot(void* rp,void* key){

	JsLockup(JsGcLock);
	JsAssert(key != NULL && rp != NULL);
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
	
	//查询RootTable中是否存在 (key,rp)
	rootpp = (struct JsRoot**)JsRootMan->table;
	size = JsRootMan->total;
	flag = FALSE;
	for(i=0 ; i < size ; ++i){
		if(rootpp[i] != NULL && rootpp[i]->key == key && rootpp[i]->rp == rp){
			//存在该组合
			flag = TRUE;
			break;
		}
	}
	if(flag){
		//已经存在了
		JsUnlock(JsGcLock);
		return;
	}
	//KeyTable 存在Key 并且RootTable 不存在(key,rp)组合
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
			rootpp[i]->rp = rp;
			rootpp[i]->key = key;
			break;
		}
	}
	//被使用空间++
	JsKeyMan->used ++;
	
	JsUnlock(JsGcLock);

}




/*****************************************************************/
static int JsHashInsert(void* rp){
	JsAssert(rp != NULL);
	int hashCode = JsHashCode(rp);
	//新节点空间
	struct JsHtNode* nodep = (struct JsHtNode*)malloc(sizeof(struct JsHtNode));
	nodep->mark = FALSE;
	nodep->next = NULL;
	nodep->rp = rp;
	
	struct JsHtNode** nodepp = &JsHashTable[hashCode];
	//最后位置
	while(*nodepp != NULL){
		nodepp = &(*nodepp)->next;
	}
	*nodepp = nodep;
	return TRUE;
}
/*将rp从hashTable中删除*/
static int JsHashRemove(void* rp){
	JsAssert(rp != NULL);
	int hashCode = JsHashCode(rp);
	struct JsHtNode** nodepp = &JsHashTable[hashCode];
	while(*nodepp != NULL ){
		if((*nodepp)->rp == rp){
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

static struct JsHtNode* JsFindHtNode(void* rp){
	JsAssert(rp != NULL);
	int hashCode = JsHashCode(rp);
	struct JsHtNode** nodepp = &JsHashTable[hashCode];
	while(*nodepp != NULL ){
		if((*nodepp)->rp == rp){
			return *nodepp;
		}
		nodepp = &(*nodepp)->next;
	}
	return NULL;
}
static int JsHashCode(void* rp){
	if(rp == NULL)
		return 0;
	char buf[128];
	sprintf(buf,"%p",rp);
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