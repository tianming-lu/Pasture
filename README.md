# Pasture

#### 介绍

本项目是一个纯净的TCP/UDP/SSL/KCP网络框架，支持IPv4、IPv6双栈，毫秒级定时器，默认多线程运，Proreactor模型，上手简单，业务开发简单方便。  
本项目的一个特点是每个连接都绑定一个类实例用于逻辑处理和数据保存，并且多个连接可以绑定同一个实例，这利于多个相关的连接更加方便的访问共享数据，业务逻辑开发相对简单很多。


#### 源码说明

src为源码目录  
Reactor.h    头文件  
IOCPReactor.cpp    Windows IOCP 实现  
EpollReactor.cpp    linux epoll 实现

使用者只需要选择其中一个.cpp文件在相应平台编译即可，头文件中为不同的平台提供相同的api，基于本框架的项目可以快速完成跨平台移植

#### 快速入门

example为示例项目目录  
echoServer.cpp    回显服务  

#### 示例
```C++
// 这是一个回显服务的示例代码
//

#include <iostream>
#include <functional>
#include "../src/Reactor.h"

#ifdef __WINDOWS__
#define TimeSleep(x) Sleep(x*1000)
#else
#define TimeSleep(x) sleep(x)
#endif // __WINDOWS__

class EchoClient : public BaseWorker		//继承BaseWorker
{
public:
	Socket sock;
	Timer timer;
	void ConnectionMade(Socket sock, PROTOCOL protocol) {
		printf("client 连接成功\n");
		/* 创建一个定时器，定时发送 hello world */
		timer.create(this, 5000, 3000, [&sock]() {
			printf("cient 发送: [hello world]\n");
			sock.send("hello world", 11);
		});
	};
	void ConnectionFailed(Socket sock, int err) {};
	void ConnectionClosed(Socket sock, int err) {
		timer.close();
		sock = NULL;
	};
	void ConnectionRecved(Socket sock, const char* data, int len) {
		printf("client 接收: [%.*s]\n\n", len, data);
		sock.popbuf(len);
	};
};

class EchoServer: public BaseWorker		//继承BaseWorker
{
public:
	Socket server_sock;
	void ConnectionMade(Socket sock, PROTOCOL protocol) {};
	void ConnectionFailed(Socket sock, int err) {};
	void ConnectionClosed(Socket sock, int err) {};
	void ConnectionRecved(Socket sock, const char* data, int len) {
		printf("server 接收: [%.*s]\n", len, data);
		sock.send(data, len);
		sock.popbuf(len);
		//sock.close();	//从容关闭，后续触发ConnectionClosed事件
		//sock.closed();	//立即关闭，不触发事件
	};
};

int main(){
	int listen_port = 8000;
	//启动全局reactor
	ReactorStart();  

	/*通过模板创建一个工厂类*/
	Factory<EchoServer> factory;

	printf("factory 启动\n");
	//factory.listen("0.0.0.0", listen_port);  //仅ipv4
	factory.listen("::", listen_port);		//ipv4、ipv6双协议栈
	printf("正在监听%d端口……\n", listen_port);

	printf("创建EchoClient,并连接127.0.0.1:8000\n");
	EchoClient* client = new EchoClient;
	client->auto_free(false);	//禁止actor释放对象
	client->sock.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);

	TimeSleep(10);
	printf("factory 优雅关闭\n");
	factory.stop();

	printf("factory 重启\n");
	listen_port = 8001;
	factory.listen("::", listen_port);
	printf("正在监听%d端口……\n", listen_port);
	TimeSleep(30);
	printf("factory 关闭\n");
	factory.stop();

	while (true){
		TimeSleep(10);
	}
}
```


#### 以下项目使用本框架

SuperSheeps服务器压力测试的框架： [http://www.supersheeps.cn/](http://www.supersheeps.cn/) 
