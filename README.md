# Pasture

#### 介绍

本项目是一个纯净的TCP/UDP网络框架，支持IPv4、IPv6双栈，默认多线程运，Proreactor模型，上手简单，业务开发简单方便。  
本项目的一个特点是每个连接都绑定一个类实例用于逻辑处理和数据保存，并且多个连接可以绑定同一个实例，这利于多个相关的连接更加方便的访问共享数据，业务逻辑开发相对简单很多。又由于框架本身是多线程的，所以可能出现资源竞争，所以会在调用类成员时加锁，这是以牺牲性能为代价的，不过整体来说，用于快速开发一些简单的业务，这点牺牲是值得的。

#### 源码说明

src为源码目录  
Reactor.h    头文件  
IOCPReactor.cpp    Windows IOCP 实现  
EpollReactor.cpp    linux epoll 实现

使用者只需要选择其中一个.cpp文件在相应平台编译即可，头文件中为不同的平台提供相同的api，基于本框架的项目可以快速完成跨平台移植

#### 快速入门

example为示例项目目录  
echoServer.cpp    回显服务  
noLockServer.cpp    无状态服务


#### 以下项目使用本框架

Sheeps： [https://gitee.com/lutianming/Sheeps.git](https://gitee.com/lutianming/Sheeps.git) 用于服务器压力测试的框架



