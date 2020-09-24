# Pasture

#### 介绍

一个跨平台的tcp/udp多线程Reactor网络框架，目前支持windows和linux，windows下使用IOCP，linux下使用epoll ET，平衡性能与业务开发难度，并且优先考虑业务开发的简易性，不管用于编写服务端或者用于客户端连接管理，都是不错的一个框架

#### 软件架构

源码只有三个文件，一个.h文件，一个windows下IOCP实现.cpp源码，一个linux下epoll实现.cpp源码  
使用者只需要选择其中一个.cpp文件在相应平台编译即可，不同的平台提供相同的api，所以项目很容易跨平台运行

#### 以下项目使用本框架

Sheeps： [https://gitee.com/lutianming/Sheeps.git](https://gitee.com/lutianming/Sheeps.git) 用于服务器压力测试的框架



