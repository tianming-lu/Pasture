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

// 这是一个回显服务的示例代码
//

#include <iostream>
#include "../src/Reactor.h"

class EchoProtocol: public BaseProtocol		//继承BaseProtocol
{
	void ConnectionMade(HSOCKET hsock, CONN_TYPE type) {};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		HsocketSend(hsock, data, len); 
		HsocketPopBuf(hsock, len);
	};
};

class EchoAccepter: public BaseAccepter		//继承BaseFactory
{
public:
	bool	Init() { 
		return true; 
	};
	void	Close() {};
	void	TimeOut() {};
	
	BaseProtocol* ProtocolCreate() {    //accept建立新连接时创建一个EchoProtocol对象
		return new EchoProtocol;
	};
	void	ProtocolDelete(BaseProtocol* proto) {    //销毁EchoProtocol对象
		delete (EchoProtocol*)proto;
	};
};

int main(){
	int listen_port = 8000;
	//启动全局reactor
	ReactorStart();  

	//创建监听器并监听指定端口
	EchoAccepter* accepter = new EchoAccepter();
	//accepter->Listen("0.0.0.0", listen_port);  //仅ipv4
	accepter->Listen("::", listen_port);		//ipv4、ipv6双协议栈

	printf("正在监听%d端口……", listen_port);
	while (true){
#ifdef __WINDOWS__
		Sleep(10*1000);
#else
		sleep(10)
#endif // __WINDOWS__
	}
}
