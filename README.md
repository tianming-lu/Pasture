# Pasture

#### 介绍

本项目是一个纯净的TCP/UDP/SSL/KCP网络框架，支持IPv4、IPv6双栈，毫秒级定时器，默认多线程运，Proactor模型，上手简单，业务开发简单方便。  

主要特性：  
1.一个或多个连接绑定一个worker用于处理网络事件，并且都在同一个线程执行，复杂场景下也能轻松实现线程安全  
2.支持定时器、自定义事件或者信号投递到worker，并且也在worker所在线程执行，
3.连接可以从一个woker转移给另一个worker  

设计思路：  
1.全局仅有一个actor  
2.actor将启动 1个accept线程 和  CPU核数 个worker线程  
3.accept线程处理tcp监听事件和数据库驱动(待开发功能)  
4.worker线程处理连接读写和业务逻辑，以及定时器、自定义事件和信号  
5.accept线程和worker线程拥有相同的逻辑，能处理的事务并不是固定的  

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
	Socket sock1;
	Socket sock2;
	Timer timer;
	void ConnectionMade(Socket sock, PROTOCOL protocol) {
		if (sock == this->sock1) {
			printf("sock1 连接成功\n");
		}
		if (sock == this->sock2) {
			printf("sock2 连接成功\n");
		}
		/* 创建一个定时器，定时发送 hello world */
		timer.create(this, 1000, 3000, [this, sock]() {
			if (sock == this->sock1) {
				sock.send("hello world 1", 13);
			}
			if (sock == this->sock2) {
				sock.send("hello world 2", 13);
			}
		});
	};
	void ConnectionFailed(Socket sock, int err) {};
	void ConnectionClosed(Socket sock, int err) {
		timer.close();
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
	void ConnectionMade(Socket sock, PROTOCOL protocol) {
		server_sock = sock;
	};
	void ConnectionFailed(Socket sock, int err) {};
	void ConnectionClosed(Socket sock, int err) {};
	void ConnectionRecved(Socket sock, const char* data, int len) {
		printf("server 接收: [%.*s]\n", len, data);
		sock.send(data, len);
		sock.popbuf(len);
		sock.close();	//从容关闭，后续触发ConnectionClosed事件
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
	client->task([client, listen_port]() {  //向client所在的线程(非当前线程)安排个任务，用于实现线程安全，
		//这一段在woker工作的线程执行
		client->sock1.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL); 
		client->sock2.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);
	});
	/*当前线程执行，非线程安全*/
	//client->sock1.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);
	//client->sock2.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);

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
