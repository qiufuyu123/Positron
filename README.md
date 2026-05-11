# positron

**[English](#english) | [中文](#中文)**

---

<a id="english"></a>

## English

Windows-only Electron process injection toolkit. Attach to any running Electron app, evaluate JavaScript in its main or renderer process, and extend functionality through a live module system — all without restarting the target.

### How it works

```
host.exe ──attach──> target (Electron)
   │                    │
   │  1. inject payload.dll (BlackBone manual map, no loader)
   │  2. payload bootstraps a JS TCP server inside V8 main thread
   │  3. payload.dll self-unmaps from memory
   │                    │
   │  4. host connects to JS server (length-prefixed JSON over TCP)
   │  5. eval / renderer hop / module load all go through JS server
   │                    │
   └────────────────────┘
```

**v2 transport** (default): After bootstrap, the native DLL removes BlackBone's VEH + scratch pages, then frees its own image via a shellcode trampoline. All subsequent communication is pure JS. Host can disconnect and reconnect without re-injecting.

**Native transport** (`--native`): Keeps the C++ IPC server alive for native-symbol hooking via BlackBone runtime detours.

### Build

Requires Visual Studio 2022+ (v145 toolset, C++20) and Node.js (for terser minification).

```
npm install
msbuild positron.slnx /p:Configuration=Debug /p:Platform=x64 /m
msbuild positron.slnx /p:Configuration=Debug /p:Platform=Win32 /m
```

Outputs: `host.exe`, `payload.dll`, `bootstrap.js`, `positron_sdk.lib`

### Usage

```bash
host.exe list                              # list Electron processes
host.exe attach <pid>                      # interactive REPL
host.exe eval <pid> "1+1"                  # one-shot eval
host.exe eval -r <pid> "document.title"    # renderer eval
host.exe run <pid> script.js               # run a .js file
host.exe attach --native <pid>             # native mode (for .hook)
```

### REPL commands

| Command | Description |
|---|---|
| `<expression>` | Evaluate JS in current world |
| `.world auto\|node` | Eval in main V8 context (default) |
| `.world renderer[:N]` | Eval in BrowserWindow[N] renderer |
| `.mod load <file>` | Load a JS module into the target |
| `.mod unload <name>` | Unload a module by name |
| `.mod list` | List loaded modules |
| `.dump [name]` | (renderer) Save page HTML to file |
| `.hook <symbol>` | (native only) Install detour on export |
| `.detach` | Disconnect and exit |

### Module system

Modules are `.js` files evaluated in the target:

```js
(function() {
  return {
    name: 'my-module',
    onLoad: function(api) {
      api.log('loaded');
      api.eval('process.pid').then(function(pid) {
        api.send({ pid: pid });
      });
    },
    onUnload: function(api) {
      api.log('bye');
    }
  };
})()
```

**Module API**: `api.eval(code)`, `api.evalRenderer(code, idx)`, `api.send(msg)`, `api.log(str)`, `api.getElectron()`, `api.require`

### SDK

```cpp
#include <positron/sdk.h>

positron::sdk::Session s;
s.attach(pid);                    // v2 by default
auto r = s.eval("1+1");          // r.json_value == "2"
s.detach();
```

### Architecture

```
positron/
  host/              CLI + REPL (host.exe)
  payload/           Injected DLL — V8 bridge, IPC, self-unmap teardown
  sdk/               Static lib, public headers, bootstrap.js
  shared/wire/       Length-prefixed JSON framing
  examples/          SDK consumer, JS modules
  tests/             Integration + unit tests
  third_party/       BlackBone, nlohmann/json, replxx, CLI11, MinHook
```

---

<a id="中文"></a>

## 中文

Windows 平台的 Electron 进程注入工具。可以附加到任意运行中的 Electron 应用，在主进程或渲染进程中执行 JavaScript，并通过模块系统扩展功能——无需重启目标程序。

### 工作原理

```
host.exe ──attach──> 目标进程 (Electron)
   │                    │
   │  1. 注入 payload.dll（BlackBone 手动映射，绕过加载器）
   │  2. payload 在 V8 主线程中启动 JS TCP 服务
   │  3. payload.dll 从内存中自卸载
   │                    │
   │  4. host 连接 JS 服务（TCP，长度前缀 + JSON）
   │  5. eval / 渲染进程跳转 / 模块加载均通过 JS 服务
   │                    │
   └────────────────────┘
```

**v2 传输**（默认）：bootstrap 完成后，原生 DLL 移除 BlackBone 的 VEH 和临时页面，然后通过 shellcode 跳板释放自身镜像。后续通信完全走 JS。host 可以断开重连而无需重新注入。

**原生传输**（`--native`）：保留 C++ IPC 服务，用于通过 BlackBone 运行时 detour 挂钩原生导出函数。

### 构建

需要 Visual Studio 2022+（v145 工具集，C++20）和 Node.js（用于 terser 压缩）。

```
npm install
msbuild positron.slnx /p:Configuration=Debug /p:Platform=x64 /m
msbuild positron.slnx /p:Configuration=Debug /p:Platform=Win32 /m
```

产物：`host.exe`、`payload.dll`、`bootstrap.js`、`positron_sdk.lib`

### 使用

```bash
host.exe list                              # 列出 Electron 进程
host.exe attach <pid>                      # 交互式 REPL
host.exe eval <pid> "1+1"                  # 单次 eval
host.exe eval -r <pid> "document.title"    # 渲染进程 eval
host.exe run <pid> script.js               # 执行 .js 文件
host.exe attach --native <pid>             # 原生模式（支持 .hook）
```

### REPL 命令

| 命令 | 说明 |
|---|---|
| `<表达式>` | 在当前 world 中执行 JS |
| `.world auto\|node` | 在主进程 V8 上下文中执行（默认） |
| `.world renderer[:N]` | 在 BrowserWindow[N] 渲染进程中执行 |
| `.mod load <文件>` | 将 JS 模块加载到目标进程 |
| `.mod unload <名称>` | 按名称卸载模块 |
| `.mod list` | 列出已加载的模块 |
| `.dump [名称]` | （渲染模式）将页面 HTML 保存到文件 |
| `.hook <符号>` | （仅原生模式）在导出函数上安装 detour |
| `.detach` | 断开连接并退出 |

### 模块系统

模块是在目标进程中执行的 `.js` 文件：

```js
(function() {
  return {
    name: 'my-module',
    onLoad: function(api) {
      api.log('已加载');
      api.eval('process.pid').then(function(pid) {
        api.send({ pid: pid });
      });
    },
    onUnload: function(api) {
      api.log('再见');
    }
  };
})()
```

**模块 API**：`api.eval(code)`、`api.evalRenderer(code, idx)`、`api.send(msg)`、`api.log(str)`、`api.getElectron()`、`api.require`

### SDK

```cpp
#include <positron/sdk.h>

positron::sdk::Session s;
s.attach(pid);                    // 默认 v2 传输
auto r = s.eval("1+1");          // r.json_value == "2"
s.detach();
```

### 项目结构

```
positron/
  host/              命令行工具 + REPL (host.exe)
  payload/           注入 DLL — V8 桥接、IPC、自卸载
  sdk/               静态库、公共头文件、bootstrap.js
  shared/wire/       长度前缀 JSON 帧协议
  examples/          SDK 使用示例、JS 模块
  tests/             集成测试 + 单元测试
  third_party/       BlackBone, nlohmann/json, replxx, CLI11, MinHook
```

### v2 自卸载流程

JS 服务确认存活后，payload 执行以下清理：

1. 停止分离线程（渲染轮询器、hook 刷新器）
2. `RemoveVectoredExceptionHandler`（移除 BlackBone 的 VEH）
3. `VirtualFree` 释放 BlackBone 临时页面（约 12 KB）
4. 在独立 RWX 页上生成跳板线程：`Sleep(500ms)` → `VirtualFree(payload基址)` → `ExitThread(0)`
5. 后续 `host.exe attach` 检测到存活的 JS 服务 → 跳过注入

## License

Research and educational use.
