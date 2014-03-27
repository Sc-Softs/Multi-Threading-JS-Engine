#include"JsObject.h"
#include"JsContext.h"
#include"JsEngine.h"
#include"JsVm.h"
#include"JsList.h"
#include"JsValue.h"
#include"JsSys.h"
#include"JsDebug.h"
#include"JsException.h"
#include"JsAst.h"
#include"JsParser.h"
#include"JsEval.h"
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<setjmp.h>
#include<unistd.h>
#include <sys/dir.h>
#include <sys/stat.h>

/**************************Print Function***************************/
//一个参数value
static void JsPrintFn(struct JsEngine* e,void* data,struct JsValue* res){
	struct JsValue v;
	struct JsContext* c = JsGetTlsContext();
	if(c ==  NULL)
		return;
	JsFindValue(c,"value",&v);
	JsPrintValue(&v);
}
static void CreatePrintFn(){
	char** argv = JsMalloc(sizeof(char*) * 1);
	argv[0] = "value";
	//创建PrintFunction
	struct JsObject* print = JsCreateStandardSpecFunction(NULL,NULL,1,argv,
		&JsPrintFn,NULL,"print",FALSE);
	struct JsValue* vPrint = (struct JsValue*)JsMalloc(sizeof(struct JsValue));
	vPrint->type = JS_OBJECT;
	vPrint->u.object = print;
	(*JsGetVm()->Global->Put)(JsGetVm()->Global,"print",vPrint,JS_OBJECT_ATTR_STRICT);
}
/**************************NIO SetTimeout***************************/
struct JsPass{
	struct JsContext* c;
	struct JsObject* f;
	int t;
};
static void JsRunFunctionTask(struct JsEngine* e,void* data){
	struct JsValue res;
	struct JsContext* c = JsGetTlsContext();
	struct JsObject* p =(struct JsObject*)data;
	
	(*p->Call)(p,c->thisObj,0,NULL,&res);
}
//NIO work
static void* JsSetTimeoutThread(void* data){
	struct JsPass* p = (struct JsPass*)data;
	JsAssert(p->c && p->f && p->t);
	//配置本线程的context;
	JsSetTlsContext( p->c);
	//nio 
	sleep(p->t);
	//finish -> add to Engine
	JsDispatch(p->c,&JsRunFunctionTask,p->f);
	return NULL;
}
//一个参数value
static void JsSetTimeout(struct JsEngine* e,void* data,struct JsValue* res){
	struct JsValue vt,vf;
	struct JsContext* c = JsGetTlsContext();
	if(c ==  NULL)
		return;
	JsFindValue(c,"Function",&vf);
	JsFindValue(c,"Time",&vt);
	if(vt.type == JS_NUMBER && vf.type == JS_OBJECT 
		&&vf.u.object != NULL && vf.u.object->Call != NULL){
		struct JsPass* p =( struct JsPass* ) JsMalloc(sizeof(struct JsPass));
		//配置新的context
		struct JsContext* newContext = JsCreateContext(e,c);
		p->c = newContext;
		p->f = vf.u.object;
		p->t = vt.u.number;
		JsStartThread(&JsSetTimeoutThread,p);
	}else{
		JsThrowString("TypeError");
	}
}
static void CreateSetTimeout(){
	char** argv = JsMalloc(sizeof(char*) * 2);
	argv[0] = "Function";
	argv[1] = "Time";
	//创建SetTimeout
	struct JsObject* setTimeout = JsCreateStandardSpecFunction(NULL,NULL,2, argv,&JsSetTimeout
		,NULL,"setTimeout",FALSE);
	struct JsValue* vSetTimeout = (struct JsValue*)JsMalloc(sizeof(struct JsValue));
	vSetTimeout->type = JS_OBJECT;
	vSetTimeout->u.object = setTimeout;
	(*JsGetVm()->Global->Put)(JsGetVm()->Global,"setTimeout",vSetTimeout,JS_OBJECT_ATTR_STRICT);
}

/**********************创建一个简单的thread 函数 *****************/
//开启新线程调用的函数
static void* JsNewEngineThread(void* data){
	struct JsPass* p = (struct JsPass*)data;
	JsAssert(p->c && p->f);
	struct JsContext* c = p->c;
	JsSetTlsContext( c);
	//finish -> add to Engine
	JsDispatch(c,&JsRunFunctionTask,p->f);
	return NULL;
}
//一个参数value
static void JsThread0(struct JsEngine* e,void* data,struct JsValue* res){
	struct JsValue vf;
	struct JsContext* c = JsGetTlsContext();
	if(c ==  NULL)
		return;
	JsFindValue(c,"Function",&vf);
	
	if(vf.type == JS_OBJECT &&vf.u.object != NULL && vf.u.object->Call != NULL){
		struct JsPass* p =( struct JsPass* ) JsMalloc(sizeof(struct JsPass));
		//配置新开线程的context;
		struct JsEngine* newEngine = JsCreateEngine();
		c = JsCreateContext(newEngine,c);
		p->c = c;
		p->f = vf.u.object;
		JsStartThread(&JsNewEngineThread,p);
	}else{
		JsThrowString("TypeError");
	}
}
static void CreateThread(){
	char** argv = JsMalloc(sizeof(char*) * 1);
	argv[0] = "Function";
	//创建SetTimeout
	struct JsObject* thread = JsCreateStandardSpecFunction(NULL,NULL,1, argv,&JsThread0,
			NULL,"thread",FALSE);
	struct JsValue* vThread = (struct JsValue*)JsMalloc(sizeof(struct JsValue));
	vThread->type = JS_OBJECT;
	vThread->u.object = thread;
	(*JsGetVm()->Global->Put)(JsGetVm()->Global,"thread",vThread,JS_OBJECT_ATTR_STRICT);
}
/*****************************Run First Task*********************************/
static void JsContextTask(struct JsEngine* e,void* data){
	char* path = "../../test/";
	DIR *pdir;
    struct dirent *pdirent;
    char temp[256];
    pdir = opendir(path);
    if(pdir)
    {
        while((pdirent = readdir(pdir)))
        {
            //跳过"."和".."
            if(strcmp(pdirent->d_name, ".") == 0
                    || strcmp(pdirent->d_name, "..") == 0)
                continue;
			sprintf(temp,"%s%s",path,pdirent->d_name);
			printf("open test file  : %s \n", temp);
			//测试test/*文件
			JS_TRY(0){
				struct JsAstNode* ast = NULL;
				struct JsValue v;
				ast = JsParseFile(JS_PARSER_DEBUG_ERROR,temp);
				JsAssert(ast != NULL);
				JsEval(e,ast,&v);
			}
			struct JsValue* err;
			JS_CATCH(err){
				JsPrintValue(err);
				JsPrintStack(JsGetExceptionStack());
			}
        }
    }
    else
    {
        printf("opendir error:%s\n", path);
    }
    closedir(pdir);

}
int main(){
	JsCreateVm(TRUE,0,NULL, NULL);
	struct JsEngine* e = JsCreateEngine();
	struct JsContext* c = JsCreateContext(e, NULL);
	CreatePrintFn();
	CreateSetTimeout();
	CreateThread();
	JsDispatch(c,&JsContextTask,NULL);
	//安全推出主线程
	JsCloseSelf();
	return 0;
}