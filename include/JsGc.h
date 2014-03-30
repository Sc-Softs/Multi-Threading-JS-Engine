#ifndef JsGcH
#define JsGcH
#include"JsType.h"


//模块初始化API
void JsPrevInitGc();
void JsPostInitGc();

/*委托到GC管理的内存中, fn = NULL 的时候, 表示配置的内存空间为原生型 如int*,double* 等*/
void* JsGcMalloc(int size,JsGcMarkFn fn);
/*Gc空间realloc大小*/
void* JsGcReAlloc(void* rp,int newSize);
/*
	标记指向内存空间为可用, 并且调用申请该内存空间时候, 配置的fn
	返回
		0 	: 先前没有标记过
		1 	: 先前标记过
		-1	: 不在Gc管理的内存中
*/
int JsGcDoMark(void* rp);
/*重新配置该内存指向的mark函数*/
void JsGcSetMarkFn(void* rp,JsGcMarkFn fn);
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
void JsGcMountRoot(void* rp,void* key);

#endif