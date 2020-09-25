// echoServer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
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

// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
