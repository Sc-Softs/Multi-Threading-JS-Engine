#include"JsNIO.h"
#include"JsObject.h"
#include"JsContext.h"
#include"JsEngine.h"
#include"JsVm.h"
#include"JsValue.h"
#include"JsList.h"
#include"JsSys.h"
#include"JsDebug.h"
#include"JsAst.h"
#include"JsException.h"
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<setjmp.h>
struct JsNIOData{
	JsThreadFn work;
	void* data;
	struct JsContext* context;
	struct JsObject* function;
};

static void* JsNIOWork(void* data);
static void JsNIOTask(struct JsEngine* e,void* data);
/*
	work 		: 开启线程后, NIO工作
	data 		: 传递给work的数据包
	o    		: 完成工作后, 调用的Js函数
	openEngine	: 是否开启新引擎单元来完成该工作.
*/
JsThread JsNIO(JsThreadFn work,void* data, struct JsObject* o, int openEngine){

	if((o!= NULL) && 
		(o == NULL  ||o->Call == NULL))
			JsThrowString("Object Is't Function");
	struct JsEngine* e;
	struct JsContext* c;
	c = JsGetTlsContext();
	e = c->engine;
	if(openEngine == TRUE){
		//开启新引擎 
		e = JsCreateEngine();
	}
	//创建新上下文
	c = JsCreateContext(e,c);
	//统一为Global对象
	c->thisObj = JsGetVm()->Global;
	//配置传递数据
	struct JsNIOData* p =( struct JsNIOData* ) JsMalloc(sizeof(struct JsNIOData));
	p->work = work;
	p->data = data;
	p->context = c;
	p->function = o;
	JsThread thread = JsStartThread(&JsNIOWork,p);
	return thread;
}


static void* JsNIOWork(void* data){
	struct JsValue* error = NULL;
	struct JsNIOData* p = (struct JsNIOData*)data;
	//设置本线程的JsContext
	JsSetTlsContext( p->context);
	//填充当前线程信息
	p->context->thread = JsCurThread();
	
	JS_TRY(0){
		//DO NIO WORK
		if(p->work != NULL)
			(*p->work)(p->data);
	}
	JS_CATCH(error){
		JsPrintValue(error);
		JsPrintStack(JsGetExceptionStack());
		//从Engine中删除该context
		JsBurnContext(p->context->engine,p->context);
		p->context->thread = NULL;
		return NULL;
	}
	//Finish
	if(p->function != NULL)
		JsDispatch(p->context,&JsNIOTask,p->function);
	else
		JsBurnContext(p->context->engine,p->context);
	p->context->thread = NULL;
	return NULL;
}
//被Dispatch 调用的task
static void JsNIOTask(struct JsEngine* e,void* data){
	struct JsValue res;
	struct JsContext* c = JsGetTlsContext();
	struct JsObject* p =(struct JsObject*)data;
	
	(*p->Call)(p,c->thisObj,0,NULL,&res);
}
