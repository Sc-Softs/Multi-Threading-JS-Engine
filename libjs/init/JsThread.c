#include"JsObject.h"
#include"JsContext.h"
#include"JsEngine.h"
#include"JsVm.h"
#include"JsValue.h"
#include"JsList.h"
#include"JsSys.h"
#include"JsDebug.h"
#include"JsAst.h"
#include"JsECMAScript.h"
#include"JsException.h"
#include<stdlib.h>
#include<stdio.h>
#include<string.h>

//新启动线程的时候, 传递的参数
struct JsStartData{
	struct JsContext* c;
	struct JsObject* f;
};

static void JsThreadObjectInit(struct JsObject* thread);

static void JsStart(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res);
static void* JsStartWork(void* data);
static void JsStartTask(struct JsEngine* e,void* data);


static void JsThreadSleep(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res);
		
void JsThreadInit(struct JsVm* vm){
	
	struct JsObject* thread = JsCreateStandardObject(NULL);
	//设置Class
	thread->Class = "Thread";
	JsThreadObjectInit(thread);
	
	//添加到Global中
	struct JsValue* v = (struct JsValue*)JsMalloc(sizeof(struct JsValue));
	v->type = JS_OBJECT;
	v->u.object = thread;
	(*vm->Global->Put)(vm->Global,"Thread",v,JS_OBJECT_ATTR_STRICT);
	
	
}

static void JsThreadObjectInit(struct JsObject* thread){
	
	struct JsValue* vProperty  = NULL;
	struct JsObject* function = NULL;
	//添加start函数
	function  = JsCreateStandardFunctionObject(NULL,NULL,FALSE);
	vProperty = (struct JsValue*) JsMalloc(sizeof(struct JsValue));
	vProperty->type = JS_OBJECT;
	vProperty->u.object = function;
	function->Call = &JsStart;
	(*thread->Put)(thread,"start",vProperty,JS_OBJECT_ATTR_STRICT);
	//添加sleep函数
	function  = JsCreateStandardFunctionObject(NULL,NULL,FALSE);
	vProperty = (struct JsValue*) JsMalloc(sizeof(struct JsValue));
	vProperty->type = JS_OBJECT;
	vProperty->u.object = function;
	function->Call = &JsThreadSleep;
	(*thread->Put)(thread,"sleep",vProperty,JS_OBJECT_ATTR_STRICT);
}
/*
	scope 为原先的Context的Scope
	thisobj 使用调用该函数的Global
*/
static void JsStart(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res){
	struct JsContext* c = JsGetTlsContext();
	if( c != NULL && argc >=0 &&argv[0]->type == JS_OBJECT && 
		argv[0]->u.object != NULL && argv[0]->u.object->Call != NULL){
		//pass test
		struct JsStartData* p =( struct JsStartData* ) JsMalloc(sizeof(struct JsStartData));
		//配置新开线程的context;
		struct JsEngine* newEngine = JsCreateEngine();
		c = JsCreateContext(newEngine,c);
		//修改thisObj = Global
		c->thisObj = JsGetVm()->Global ;
		p->c = c;
		p->f = argv[0]->u.object;
		JsStartThread(&JsStartWork,p);
		res->type = JS_BOOLEAN;
		res->u.number = TRUE;
	}else{
		JsThrowString("TypeError");
	}
}

//开启新线程
static void* JsStartWork(void* data){
	struct JsStartData* p = (struct JsStartData*)data;
	//设置本线程的JsContext
	JsSetTlsContext( p->c);
	//填充当前线程信息
	p->c->thread = JsCurThread();
	//finish -> add to Engine
	JsDispatch(p->c,&JsStartTask,p->f);
	p->c->thread = NULL;
	return NULL;
}

//被Dispatch 调用的task
static void JsStartTask(struct JsEngine* e,void* data){
	struct JsValue res;
	struct JsContext* c = JsGetTlsContext();
	struct JsObject* p =(struct JsObject*)data;
	
	(*p->Call)(p,c->thisObj,0,NULL,&res);
}


static void JsThreadSleep(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res){
		
	if(argc <= 0 || argv[0]->type != JS_NUMBER)
		JsThrowString("Args Error");
	JsSleep(argv[0]->u.number);
}
