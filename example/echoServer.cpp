// 这是一个回显服务的示例代码
//

#include <iostream>
#include "Reactor.h"

class EchoProtocol: public BaseProtocol		//继承BaseProtocol
{
	void ConnectionMade(HSOCKET hsock) {};
	void ConnectionFailed(HSOCKET hsock) {};
	void ConnectionClosed(HSOCKET hsock) {};
	void Recved(HSOCKET hsock, const char* data, int len) { 
		HsocketSend(hsock, data, len); 
		HsocketSkipBuf(hsock, len);
	};
};

class EchoFactory: public BaseFactory		//继承BaseFactory
{
public:
	bool	FactoryInit() { 
		return true; 
	};
	void	FactoryLoop() {};
	void	FactoryClose() {};
	BaseProtocol* CreateProtocol() {    //accept建立新连接时创建一个EchoProtocol对象
		return new EchoProtocol;
	};
	void	DeleteProtocol(BaseProtocol* proto) {    //销毁EchoProtocol对象
		delete (EchoProtocol*)proto;
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
