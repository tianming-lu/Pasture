/*	
*	Copyright(c) 2019 lutianming email：641471957@qq.com
*	Pasture is licensed under the Mulan PSL v1.
*	You can use this software according to the terms and conditions of the Mulan PSL v1.
*	You may obtain a copy of Mulan PSL v1 at :
*	http://license.coscl.org.cn/MulanPSL
*	THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
*	IMPLIED, INCLUDING BUT NOT LIMITED TO NON - INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
*	PURPOSE.
*
*	See the Mulan PSL v1 for more details.
*/

// 这是一个无状态服务的示例代码，避免每个连接接入和关闭，导致EchoProtocol对象频繁创建和销毁
// echo_proto->SetNoLock(); 将EchoProtocol设置为无锁模式，避免多线程竞争资源，提高服务器性能
//

#include <iostream>
#include "../src/Reactor.h"

class EchoProtocol : public BaseProtocol		//继承BaseProtocol
{
	void ConnectionMade(HSOCKET hsock, CONN_TYPE type) {};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		HsocketSend(hsock, data, len);
		HsocketPopBuf(hsock, len);
	};
};

class EchoFactory : public BaseFactory		//继承BaseFactory
{
public:
	EchoProtocol* echo_proto = NULL;
public:
	bool	FactoryInit() {
		echo_proto = new EchoProtocol();
		//echo_proto->SetFactory(this, CLIENT_PROTOCOL);
		echo_proto->SetNoLock();
		return true;
	};
	void	FactoryInited() {};
	void	FactoryLoop() {};
	void	FactoryClose() {};
	BaseProtocol* CreateProtocol() {    //accept建立新连接时复用EchoProtocol对象
		return this->echo_proto;
	};
	void	DeleteProtocol(BaseProtocol* proto) {    //千万别销毁
	};
};


int  i = 0;
static void user_callback(HSOCKET hsock, BaseProtocol* proto){
	i++;
	printf("%s:%d %d\n", __func__, __LINE__, i);
	Sleep(2000);
	if (i == 2){
		printf("%s:%d 删除定时器1\n", __func__, __LINE__);
		TimerDelete(hsock);
		printf("%s:%d 删除定时器2\n", __func__, __LINE__);
	}
}

int main()
{
	std::cout << "Hello World!\n";
	ReactorStart();
	EchoFactory* bfc = new EchoFactory();
	//bfc->Set(rct, "0.0.0.0", 8000);  //仅ipv4
	bfc->Set("::", 8000);		//ipv4、ipv6双协议栈
	FactoryRun(bfc);

	EchoProtocol* user = new EchoProtocol();
	user->SetFactory(bfc, CLIENT_PROTOCOL);
	TimerCreate(user, 0, 1000, user_callback);

	while (true)
	{
#ifdef __WINDOWS__
		Sleep(10 * 1000);
#else
		sleep(10)
#endif // __WINDOWS__
	}
}
