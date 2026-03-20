# LemonTea

LemonTea 是 LemonTea 远程控制系统中的服务端程序，负责接收 HoneyTea 客户端连接，并把客户端能力转发为 HTTP API，供前端或其他工具调用。

当前版本是用于本地联调的 C++17 原型，已经包含以下能力：

- 接收 HoneyTea 的 TCP 或 WebRTC 控制连接
- 通过 Crow 暴露 HTTP API
- 转发远程 shell、文件管理、插件控制等请求
- 管理服务端本地插件子进程
- 输出详细日志，便于定位链路问题

## 目录结构

- src/: 服务端主程序源码
- config/: 配置文件示例
- plugins/: 服务端本地插件与 manifest
- build/: 本地构建产物目录

## 依赖

- CMake 3.20 及以上
- 支持 C++17 的编译器
- 网络可用，用于 CMake FetchContent 拉取依赖

构建时会自动拉取以下第三方库：

- Asio
- nlohmann/json
- Crow
- libdatachannel

## 构建

在仓库根目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

构建完成后，二进制默认位于：

```bash
build/lemontea
```

## 配置

示例配置文件位于 [config/server.example.json](config/server.example.json)。

关键字段说明：

- transport.mode: 通信模式，可选 tcp 或 webrtc
- transport.tcp_listen_port: TCP 模式监听端口
- transport.webrtc_signal_port: WebRTC 模式信令端口
- http_host: Crow HTTP 服务绑定地址
- http_port: Crow HTTP 服务端口
- request_timeout_ms: 向 HoneyTea 转发请求的超时时间
- plugin_manifests: 服务端插件 manifest 列表

一个典型配置如下：

```json
{
  "transport": {
    "mode": "tcp",
    "tcp_listen_port": 9000,
    "webrtc_signal_port": 9001,
    "stun_servers": [
      "stun:stun.l.google.com:19302"
    ]
  },
  "http_host": "0.0.0.0",
  "http_port": 18080,
  "request_timeout_ms": 15000,
  "plugin_manifests": [
    "../plugins/server_echo.manifest.json"
  ]
}
```

## 运行

建议先复制配置文件并按实际环境修改：

```bash
cp config/server.example.json config/server.local.json
./build/lemontea config/server.local.json
```

如果你沿用仓库中现成的本地联调配置，也可以直接运行：

```bash
./build/lemontea config/server.webrtc.local.json
```

## HTTP API

LemonTea 的定位是把 HoneyTea 的能力暴露为 HTTP 接口。典型调用流程：

1. 前端或脚本调用 LemonTea HTTP API
2. LemonTea 把请求转发到指定的 HoneyTea 客户端
3. HoneyTea 执行动作并返回结果
4. LemonTea 返回 JSON 响应

示例：

```bash
curl http://127.0.0.1:18080/health
```

```bash
curl -X POST http://127.0.0.1:18080/api/clients/raspi-dev-01/shell \
  -H 'Content-Type: application/json' \
  -d '{"command":"uname -a"}'
```

更完整的接口说明见 [../LemonTea-doc/http-api.md](../LemonTea-doc/http-api.md)。

## 插件机制

LemonTea 也支持本地插件子进程，当前仓库附带一个示例插件：

- server_echo: 用于验证服务端插件管理链路

manifest 示例见 [plugins/server_echo.manifest.json](plugins/server_echo.manifest.json)。

## 推荐联调顺序

1. 启动 LemonTea
2. 启动 HoneyTea
3. 用 curl 验证 /health 和客户端列表接口
4. 再接入 LemonTea-vue3 图形界面

## 相关文档

- [../LemonTea-doc/README.md](../LemonTea-doc/README.md)
- [../LemonTea-doc/architecture.md](../LemonTea-doc/architecture.md)
- [../LemonTea-doc/http-api.md](../LemonTea-doc/http-api.md)
- [../LemonTea-doc/macos-testing.md](../LemonTea-doc/macos-testing.md)