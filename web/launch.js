startServer(function(request,response){
	response.setHeader('Content-Type',"text/html; charset=utf-8");
	response.write('中文测试' + '<br>');
	response.clear();
	response.write('中文测试' + '<br>');
	response.write('访问当前URI:' + request.getRequestURI() + '<br>');
	response.write('参数args:' + request.getParameter('args') + '<br>');
	response.write('方法:' + request.getMethod() + '<br>');
	
},8080);