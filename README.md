# Pasture

#### 介绍

本项目是一个纯净的TCP/UDP/SSL/KCP网络框架，支持IPv4、IPv6双栈，毫秒级定时器，默认多线程运，Proactor模型，上手简单，业务开发简单方便。  

主要特性：  
1. 对TCP/UDP/SSL/KCP连接提供相同的API操作  
2. 绑定IPv6的服务，同时可以接受IPv4的连接  
3. 一个或多个连接绑定一个worker用于处理网络事件，并且都在同一个线程执行，复杂场景下也能轻松实现线程安全  
4. 支持定时器、自定义事件或者信号投递到worker，并且也在worker所在线程执行，
5. 连接可以从一个woker转移给另一个worker  


设计思路：  
1. 全局仅有一个actor,管理所有的accepter和worker
2. actor将启动 1个accept线程 和  CPU核数 个worker线程  
3. accept线程处理tcp监听事件 
4. worker线程处理连接读写和业务逻辑，以及定时器、自定义事件和信号  
5. accept线程和worker线程拥有相同的逻辑，能处理的事务并不是固定的  

接口简洁：

```C++
	HSOCKET tcp_sock = HsocketConnect(worker, "127.0.0.1", 1080, TCP_PROTOCOL);
	HSOCKET udp_sock = HsocketConnect(worker, "127.0.0.1", 1080, UDP_PROTOCOL);

	HsocketSSLCreate(tcp_sock, SSL_SERVER, 0, ca_crt, user_crt, pri_key);
	HsocketSSLCreate(tcp_sock, SSL_CLIENT, 0, ca_crt, user_crt, pri_key);

	HsocketKcpCreate(udp_sock, conv, 0);
	HsocketKcpUpdate(udp_sock, hsock);

	HsocketSend(sock, data, len);
	HsocketClose(sock);
```

#### 源码说明

	src为源码目录  
	actor.h    			头文件  
	actor_iocp.cpp    	Windows IOCP 实现  
	actor_epoll.cpp    	linux epoll 实现

	example示例代码  
	echoServer.cpp		回显服务 

	使用者只需要选择其中一个.cpp文件在相应平台编译即可，头文件中为不同的平台提供相同的api，基于本框架的项目可以快速完成跨平台移植



#### 示例
```C++
// 这是一个回显服务的示例代码
//

#include <iostream>
#include <functional>
#include "actor.h"

#ifdef __WINDOWS__
#define TimeSleep(x) Sleep(x*1000)
#else
#define TimeSleep(x) sleep(x)
#endif // __WINDOWS__

class EchoClient : public BaseWorker		//继承BaseWorker
{
public:
	HSOCKET sock1 = nullptr;
	HSOCKET sock2 = nullptr;
	Timer timer;
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {
		if (hsock == this->sock1) {
			printf("sock1 连接成功\n");
		}
		if (hsock == this->sock2) {
			printf("sock2 连接成功\n");
		}
	};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {
		if (hsock == this->sock1) {
			sock1 = nullptr;
		}
		if (hsock == this->sock2) {
			sock2 = nullptr;
		}
		if (sock1 == nullptr && sock2 == nullptr) {
			timer.close();
		}
	};
	void ConnectionRecved(HSOCKET sock, const char* data, int len) {
		printf("client 接收: [%.*s]\n", len, data);
		HsocketPopBuf(sock, len);
	};
};

class EchoServer: public BaseWorker		//继承BaseWorker
{
public:
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		printf("server 接收: [%.*s]\n", len, data);
		HsocketSend(hsock, data, len);
		HsocketPopBuf(hsock, len);
		//HsocketClose(sock);	//从容关闭，后续触发ConnectionClosed事件
		//HsocketClosed(sock);	//立即关闭，不触发事件
	};
};

int main(){
	int listen_port = 8000;
	//启动全局reactor
	ReactorStart(0);  

	/*通过模板创建一个工厂类*/
	Factory<EchoServer> factory;

	printf("factory 启动\n");
	//factory.listen("0.0.0.0", listen_port);  //仅ipv4
	factory.listen("::", listen_port);		//ipv4、ipv6双协议栈
	printf("正在监听%d端口……\n", listen_port);

	printf("创建EchoClient,并连接127.0.0.1:8000\n");
	EchoClient client;
	client.task([&client, listen_port]() {  //向client所在的线程(非当前线程)安排个任务，用于实现线程安全
		//这一段在woker工作的线程执行
		client.sock1 = HsocketConnect(&client, "127.0.0.1", listen_port, TCP_PROTOCOL); 
		client.sock2 = HsocketConnect(&client, "127.0.0.1", listen_port, TCP_PROTOCOL);
	});
	/* 创建一个定时器，定时发送 hello world */
	client.timer.create(&client, 1000, 3000, [&client]() {
		if (client.sock1)HsocketSend(client.sock1, "hello world 1", 13);
		if (client.sock2)HsocketSend(client.sock2, "hello world 2", 13);
	});

	//TimeSleep(10);
	//printf("factory 优雅关闭\n");
	//factory.stop();
	
	//printf("factory 重启\n");
	//listen_port = 8001;
	//factory.listen("::", listen_port);
	//printf("正在监听%d端口……\n", listen_port);
	//TimeSleep(30);
	//printf("factory 关闭\n");
	//factory.stop();

	while (true){
		TimeSleep(10);
	}
}
```


#### 以下项目使用本框架

SuperSheeps服务器压力测试的框架： [http://www.supersheeps.cn/](http://www.supersheeps.cn/) 
