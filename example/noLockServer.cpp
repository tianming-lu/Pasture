// 这是一个无状态服务的示例代码，避免每个连接接入和关闭，导致EchoProtocol对象频繁创建和销毁
// echo_proto->SetNoLock(); 将EchoProtocol设置为无锁模式，避免多线程竞争资源，提高服务器性能
//

#include <iostream>
#include "Reactor.h"

class EchoProtocol: public BaseProtocol		//继承BaseProtocol
{
	void ConnectionMade(HSOCKET hsock, const char* ip, int port) {};
	void ConnectionFailed(HSOCKET, const char* ip, int port) {};
	void ConnectionClosed(HSOCKET hsock, const char* ip, int port) {};
	void Recved(HSOCKET hsock, const char* ip, int port, const char* data, int len) { 
		HsocketSend(hsock, data, len); 
		HsocketSkipBuf(hsock, len);
	};
};

class EchoFactory: public BaseFactory		//继承BaseFactory
{
public:
	EchoProtocol* echo_proto;
public:
	bool	FactoryInit() { 
		echo_proto = new EchoProtocol();
		//echo_proto->SetFactory(this, CLIENT_PROTOCOL);
		echo_proto->SetNoLock();
		return true; 
	};
	void	FactoryLoop() {};
	void	FactoryClose() {};
	BaseProtocol* CreateProtocol() {    //accept建立新连接时复用EchoProtocol对象
		return this->echo_proto;
	};
	void	DeleteProtocol(BaseProtocol* proto) {    //千万别销毁
	};
};

int main()
{
    std::cout << "Hello World!\n";
    Reactor* rct = new Reactor();
	ReactorStart(rct);
	EchoFactory* bfc = new EchoFactory();
	bfc->Set(rct, 8000);
	FactoryRun(bfc);
	while (true)
	{
#ifdef __WINDOWS__
		Sleep(10*1000);
#else
		sleep(10)
#endif // __WINDOWS__
	}
}
