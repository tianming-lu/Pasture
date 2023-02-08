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
#include "../src/Reactor.h"

#ifdef __WINDOWS__
#define TimeSleep(x) Sleep(x*1000)
#else
#define TimeSleep(x) sleep(x)
#endif // __WINDOWS__

class EchoProtocol: public BaseProtocol		//继承BaseProtocol
{
	void ConnectionMade(HSOCKET hsock, CONN_TYPE type) {};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		HsocketSend(hsock, data, len); 
		HsocketPopBuf(hsock, len);
		//HsocketClose(hsock);   //从容关闭，等待关闭通知
		//HsocketClosed(hsock);  //立即关闭，无通知
	};
};

class EchoAccepter: public BaseAccepter		//继承BaseFactory
{
public:
	bool	Init() { 
		return true; 
	};
	void	TimeOut() {
	};
	BaseProtocol* ProtocolCreate() {    //accept建立新连接时创建一个EchoProtocol对象
		EchoProtocol* proto = new EchoProtocol;
		//proto->Set(CLIENT_PROTOCOL);   默认为SERVER_PROTOCOL，连接关闭时proto会被自动释放，否则由用户控制释放时机
		//proto->ThreadSet();   //自动分配工作线程
		//proto->ThreadSet(0);   //分配到指定工作线程， 0 ~ ActorThreadWorker - 1 
		return proto;
	};
};

int main(){
	int listen_port = 8000;
	//启动全局reactor
	ReactorStart();  

	//创建监听器并监听指定端口
	EchoAccepter* accepter = new EchoAccepter();

	printf("accepter 启动\n");
	//accepter->Listen("0.0.0.0", listen_port);  //仅ipv4
	accepter->Listen("::", listen_port);		//ipv4、ipv6双协议栈
	printf("正在监听%d端口……\n", listen_port);
	TimeSleep(10);
	printf("accepter 优雅关闭\n");
	accepter->Stop();

	//安全起见，等待accpeter完全停止
	while (true){
		if (accepter->Listening == false) {
			printf("accepter 已关闭\n");
			break;
		}
	}

	printf("accepter 重启\n");
	listen_port = 8001;
	accepter->Listen("::", listen_port);
	printf("正在监听%d端口……\n", listen_port);
	//TimeSleep(30);
	//printf("accepter 关闭\n");
	//accepter->Stop();

	while (true){
		TimeSleep(10);
	}
}
```


#### 以下项目使用本框架

SuperSheeps服务器压力测试的框架： [http://www.supersheeps.cn/](http://www.supersheeps.cn/) 
