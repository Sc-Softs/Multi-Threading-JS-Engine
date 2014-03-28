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

#define JS_NEW_THREAD_OBJECT_FLOOR 1

//新启动线程的时候, 传递的参数
struct JsStartData{
	struct JsContext* c;
	struct JsObject* f;
};

static void JsThreadObjectInit(struct JsObject* thread);
//创建一个由于start函数构造处理来的对象
static struct JsObject* JsCreateMockThread(JsThread t);

static void JsStart(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res);
static void* JsStartWork(void* data);
static void JsStartTask(struct JsEngine* e,void* data);


static void JsThreadSleep(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res);

static void JsThreadYield(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res);
		
static void JsThreadJoin(struct JsObject *self, struct JsObject *thisobj, 
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
	//添加yield函数
	function  = JsCreateStandardFunctionObject(NULL,NULL,FALSE);
	vProperty = (struct JsValue*) JsMalloc(sizeof(struct JsValue));
	vProperty->type = JS_OBJECT;
	vProperty->u.object = function;
	function->Call = &JsThreadYield;
	(*thread->Put)(thread,"yield",vProperty,JS_OBJECT_ATTR_STRICT);
}
/*
	特殊的Thread对象
*/
static struct JsObject* JsCreateMockThread(JsThread t){
	struct JsValue* vProperty  = NULL;
	struct JsObject* function = NULL;
	
	if(t == NULL)
		JsThrowString("Create Thread Error");
	struct JsObject* thread = JsAllocObject(JS_NEW_THREAD_OBJECT_FLOOR);
	//构建了standard
	JsCreateStandardObject(thread);
	thread->Class = "MockThread";
	thread->sb[JS_NEW_THREAD_OBJECT_FLOOR] = t;
	//添加sleep函数
	function  = JsCreateStandardFunctionObject(NULL,NULL,FALSE);
	vProperty = (struct JsValue*) JsMalloc(sizeof(struct JsValue));
	vProperty->type = JS_OBJECT;
	vProperty->u.object = function;
	function->Call = &JsThreadJoin;
	(*thread->Put)(thread,"join",vProperty,JS_OBJECT_ATTR_STRICT);
	return thread;
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
		JsThread thread = JsStartThread(&JsStartWork,p);
		res->type = JS_OBJECT;
		res->u.object = JsCreateMockThread(thread);
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

static void JsThreadYield(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res){
		
	JsYield();
		
}

static void JsThreadJoin(struct JsObject *self, struct JsObject *thisobj, 
		int argc, struct JsValue **argv, struct JsValue *res){
		
	if(strcmp(thisobj->Class,"MockThread") == 0 ){
		JsThread thread = (JsThread)thisobj->sb[JS_NEW_THREAD_OBJECT_FLOOR];
		JsJoin(thread);
	}
}
