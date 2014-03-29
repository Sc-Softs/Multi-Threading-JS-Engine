#ifndef JsNIOH
#define JsNIOH
/**
	NIO线程  和 开启新线程的时候, 调用的函数, 提供了一个统一的接口
	不必在配置context 和 engine, 并且添加默认异常处理.
	注: 新线程中的context的this为Gloabl
*/
#include"JsType.h"
/*
	work 		: 开启线程后, NIO工作, 可以为NULL
	data 		: 传递给work的数据包
	o    		: 完成工作后, 调用的Js函数, 可以为NULL
	openEngine	: 是否开启新引擎单元来完成该工作.
*/
JsThread JsNIO(JsThreadFn work,void* data, struct JsObject* o, int openEngine);

#endif