# 安全说明

## 凭据

仓库和 Release 包不包含 TURN 密码、SSH 密码、令牌或私钥。运行客户端前通过环境变量提供 TURN 长期凭据：

- `REMOTE_CONTROL_TURN_USERNAME`
- `REMOTE_CONTROL_TURN_PASSWORD`

示例见 `config/remote-control.env.example`。不要提交真实 `.env` 文件。

## 生产部署基线

- 将 TCP 信令放在 TLS 终止层之后，并为每个用户增加鉴权和短期会话令牌。
- TURN 优先使用短期凭据或 REST API 动态凭据，并定期轮换。
- 限制信令端口、TURN 端口和中继端口范围，配置防火墙、限流和审计。
- 键鼠控制必须由被控端明确授权；涉及管理员窗口时遵循 Windows 完整性级别限制。
- 不要尝试控制 Windows 安全桌面、UAC 密码输入界面或锁屏界面。

如曾在聊天、日志或旧二进制中使用过固定凭据，应在公开发布后立即轮换。
