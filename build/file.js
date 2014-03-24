(function(){
	var a =1 ,b =2 ,c =3;
	print("============test var==========");
	print(a);
	print(b);
	print(c);
	print("===============================");
	print("============test ;==========");
	;;;;;
	print("=============================");
	print("============test if==========");
	if(  a == 1)
		print("a = 1");
	else
		print(" a != 1");
	if( a != 1)
		print(" a != 1")
	else
		print(" a == 1");
	print("============test if==========");
	print("============test for var==========");
	for(var i = 0 ; i < arguments.length; ++i)
		print(arguments[i]);
	print("==================================");
	print("============test do while==========");
	var i = 0;
	do{
		i++;
		print(i);
		//break;
	}while(i <= 3);
	print("=======================================");
	print("===============test while =============");
	
	var i = 0;
	while(i <= 3){
		print(i);
		i++;
	}
	print("=======================================");
	print("===============test in =============");
	var obj = {a:1,b:'hello'};
	for(var name in obj){
		print("obj "+name+": "+obj[name]);
	}
	print("===============================");
	print("============== test continue break============");
	for(var i = 0 ; i < 100;++i){
		if(i < 10)
			continue;
		print(i);
		break;
	}
	print("===============================");
	print("============test return=========");
	var c  = function(){
		return "succ";
	};
	print(c());
	print("===============================");
	print("====== test try catch throw ===");
	try{
		print("into try");
		throw "error";
	}catch(e){
		print("into catch");
		print(e);
	}finally{
		print("into finally");
	}
	print("===============================");
	print("====== test function  ===");
	function hello(a,b,c){
		print(a+b+c);
	}
	hello("hello",2,"function");
	var func = function(a,b,c){
		for(var i = 0; i < arguments.length ; ++i){
			print(arguments[i]);
		}
		print(c);
	}
	func(1,2);
	print("===============================");
	print("============test prototype===========");
	var Class = function(name){
		this["name"] = name;
	};
	Class.prototype = {soup : 21};
	var C1 = new Class("C1");
	var C2 = new Class("C2");
	print("C1-name: "+C1.name);
	print("C2-name: "+C2.name);
	for(var name in  Class.prototype)
		print(name);
	print("C1-soup: " +C1.soup);
	print("C2-soup: "+  C2.soup);
	Class.prototype.soup = 12;
	//print("C1-soup: " +C1.soup);
	print("C2-soup: "+  C2.soup);
	print("change C1 soup");
	C1.soup = "c1";
	print("C1-soup: "+C1.soup);
	print("C2-soup: "+C2.soup);
	print("===============================");
	print("===========test clouse=========");
	var test1 = {
		hello:'hello',
		good:'good'
	};
	var test2 = {};
	for(var name in test1){
		test2[name] = (function(name){
			return function(){
				print(name);
			}
		})(name);
	}
	test2['hello']();
	test2['good']();
	
	print("===============================");
	print("===========test exception ,======");
	var a = 2;
	print(a + 1),print(a+2);
	print("===============================");
	print("===========test assignment =============");
	var c = 10;
	print("c = 10 : "+ c);
	print("c *=2 : "+ (c*=2));
	print("c /= 2: "+ (c/=2) );
	print("c %= 3: " + (c%=3) );
	print("c += 9: " + (c +=9));
	print("c -= 10:" + (c -=10));
	print("===============================");
	print("=========test exp?exp:exp============");
	var c =1;
	c ? print("c = succ") : print("error");
	print("===============================");
	print("=============test bin logical==========");
	if( false && true){
		print("error");
	}else{
		print("succ");
		
	}
	if( true && true){
		print("succ");
	}else{
		print("error");
		
	}
	if(false || false){
		print("error");
	}else{
		print("succ");
	}
	print("===============================");
	print("=======test equality Operators=============");
	var c = {};
	var d = c;
	if( c == d)
		print("object ref succ");
	var c = "abc";
	var d = "abc";
	if( c==d){
		print("string eq succ");
	}
	d = "abcd";
	if( c!= d){
		print("string != succ");
	}
	print("===============================");
	print("=======test relational Operators=============");
	print("string <" + ("abc" < "abcd"));
	print("string >" + ("abc" > "abcd"));
	
	
})(0,1,2,3,"string",true,false,{a:1},function(){});