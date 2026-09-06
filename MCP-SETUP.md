# ALS 项目 MCP

已于 2026-09-06 安装并完成实际连接验证。

- 项目：`AdvancedLocomotionV4.uproject`
- 引擎：UE 5.7.4（BuildId `47537391`）
- 插件：MCP for Unreal Editor 0.6.1，安装于 `Plugins/ModelContextProtocol`
- 来源：https://github.com/AvatarGanymede/ue5.7-mcp/releases/tag/v0.6.1
- 地址：`http://127.0.0.1:18777/mcp`，仅监听本机
- Codex 项目配置：`.codex/config.toml`，覆盖全局 `unreal-mcp` 的 8000 端口
- UE 项目端口配置：`Config/DefaultModelContextProtocol.ini`

打开此项目时 MCP 随编辑器启动；关闭编辑器后服务停止。其他 UE 5.8 项目仍可使用原来的 8000 端口。

Codex 已打开的会话可能需要重新连接或重启 Codex 后，才能载入新工具。工具名为 `unreal`，支持 `health`、`discover`、`execute`、`task`；先用 `health` 验证连接。

## 连接检查

在当前项目目录的 PowerShell 中运行：

```powershell
.\Scripts\Test-UnrealMCP.ps1
```

该脚本执行 MCP 握手、工具枚举、健康检查及读取项目路径，不修改场景或资产。首次验证结果在 `Saved/MCPSetup/verification.json`：Python 已加载、运行于游戏线程、项目路径匹配当前 ALS 项目。

## 还原

关闭编辑器后，可用 `Saved/MCPSetup/AdvancedLocomotionV4.uproject.before-mcp` 还原安装前的项目描述文件（如果之后又修改过项目插件配置，请手动合并）。移除本次添加的插件目录、项目 MCP 配置和端口配置即可撤销；全局 Codex 配置没有修改。

安装包及校验记录保存在 `Saved/MCPSetup`，DLL 的 SHA-256 与安装包附带记录相符，插件和本机引擎 BuildId 一致。
