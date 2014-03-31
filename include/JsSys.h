#ifndef JsSysH
#define JsSysH
#include"JsType.h"
struct JsVm;
struct JsEngine;
struct JsContext;

/****************************************************************************
									通用API
*****************************************************************************/

//模块初始化API
void JsPrevInitSys();
void JsPostInitSys();

/***********************************内存管理模块 API*******************************/


/*-------------------------兼容原有代码---------------------------------------*/
/*真机内存 API*/
void* JsMalloc(int size);
void* JsReAlloc(void* mem,int newSize);


/*----------------------------------GC 内存管理--------------------------------*/
/*
	委托到GC管理的内存中
		size must > 0
		markFm = NULL 的时候, 表示配置的内存空间为原生型 如int*,double* 等
		freeFn 为在回收该内存的时候, 执行的析构动作
*/
void* JsGcMalloc(int size,JsGcMarkFn markFn,JsGcFreeFn freeFn);
/*Gc空间realloc大小*/
void* JsGcReAlloc(void* mp,int newSize);
/*
	标记指向内存空间为可用, 并且调用申请该内存空间时候, 配置的fn
	返回
		0 	: 先前没有标记过
		1 	: 先前标记过
		-1	: 不在Gc管理的内存中
	*一个属性是否被Mark到, 在于父容器是否对该属性所在的MP(内存地址)进行Mark
*/
int JsGcDoMark(void* mp);
/*重新配置该内存指向的mark函数*/
void JsGcSetMarkFn(void* mp,JsGcMarkFn markFn);
/*重新配置该内存指向的free函数*/
void JsGcSetFreeFn(void* mp,JsGcMarkFn freeFn);
/*
	Gc中添加一个有效的Key类型(指针类型),Key != 0, 
	注意该Key 仅仅是一个标识符, 无论是否指向有效内存.
	返回
		0 : 已经存在该key
		1 : 插入成功
*/
int JsGcRegistKey(void* key,const char* desc);
/*Gc中删除一个Key , Key != 0, 并且RootTable中相关的点*/
void JsGcBurnKey(void* key);
/*Gc扫描的时候, Root节点配置*/
void JsGcMountRoot(void* mp,void* key);
/*
		调用TrapGc的时候, 检测是否需要进行Gc, 则调用该函数, 完成等待Gc
	前需要完成的工作, fn = NULL , 则表示没有要做的, data 为传递给fn的数据
*/
void JsGcTrap(JsGcTrapFn fn,void* data);

/****************************锁模块 API**********************************/
/*锁 API*/
JsLock JsCreateLock();
void JsLockup(JsLock lock);
void JsUnlock(JsLock lock);
void JsDestroyLock(JsLock* lock);

/*****************************线程模块API********************************/

/*线程 API*/
//返回线程相关信息
JsThread JsCurThread();
//启动线程
JsThread JsStartThread(JsThreadFn fn,void* data);
//自身线程安全退出
void JsCloseSelf();
//终止非当前线程
void JsCloseThread(JsThread thread);
//sleep time = ms
void JsSleep(long time);
//yield
void JsYield();
//thread join
void JsJoin(JsThread thread);
//马上退出
void JsHalt();

/*********************************TLS  模块 API******************************/

/*把数据存储在当前TLS中*/
JsTlsKey JsCreateTlsKey(JsTlsFn fn);
void JsSetTlsValue(JsTlsKey key, void* value);
void* JsGetTlsValue(JsTlsKey key);

#endif