# 完整依赖包

下载 `RemoteControl_rtc_x64_Full_20260722_GitHub.zip` 后完整解压，再运行客户端。

包内包含：

- Windows x64 MFC 客户端 `RemoteControl_rtc.exe`
- Windows x64 C++ 信令服务器 `SignalingServer.exe`
- `libwebrtc.dll` 和 `d3dcompiler_47.dll`
- Microsoft 官方 `vc_redist.x64.exe` 兼容安装程序
- 环境检查、TURN 启动模板、依赖说明、许可证和逐文件 SHA-256

ZIP SHA-256：

```text
83BF2A11A6B582CBE489A84FF2D9B723561B6A34FA0FF943CE06789557A804CA
```

公开包不包含 TURN/SSH 密码。复制包内 `StartWithTurn.example.cmd` 为本机私有副本，填入已经轮换的 TURN 凭据后启动。
