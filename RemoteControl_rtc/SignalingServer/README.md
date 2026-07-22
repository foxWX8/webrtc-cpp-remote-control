# C++ 信令服务器

这是远程桌面客户端配套的轻量 TCP 信令服务器。服务器基于 `select` 事件模型，Windows 和 Linux 共用一套 C++ 源码，每行传输一个 UTF-8 JSON 对象。

## 构建

Windows：可直接在主解决方案中构建 `SignalingServer` 项目。

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

## 启动

```bash
./SignalingServer 0.0.0.0 8000
```

Windows 可执行文件的参数相同：

```powershell
.\SignalingServer.exe 0.0.0.0 8000
```

支持的消息类型：`login`、`ping`、`pong`、`presence.snapshot`、`presence.changed`、`offer`、`answer`、`ice-candidate`、`hangup`。

> 公网生产部署应在服务器前增加 TLS、身份认证、限流、审计和进程守护。当前实现适合作为可扩展的信令核心，不应裸露在不受信任网络中长期运行。
