# Positron — Electron 程序 Windows 注入框架设计文档

**日期**：2026-05-09
**状态**：Design — pending implementation plan
**作者**：fuyuqiu2@illinois.edu

---

## 1. 目标与范围

构建一个面向 Windows 的通用 Electron 程序注入框架，用于**逆向、调试与运行时观察**。框架由两部分构成：

- `positron-host.exe`：宿主端 CLI/REPL，负责进程枚举、DLL 注入、与目标通信
- `positron.dll`：注入到目标 Electron 进程的 payload，提供 napi 桥接、JS 执行、Hook 能力

### 1.1 已确认需求

| 维度 | 决定 |
|---|---|
| 用例 | 逆向 / 调试 / 运行时观察 |
| 目标进程 | Electron main 进程 + renderer 进程，二者均支持，CLI 区分 |
| 注入模式 | Attach 已运行进程（不做 spawn-and-suspend） |
| 宿主形态 | CLI + 交互式 REPL |
| Hook 能力 | 一等公民（基于 MinHook） |
| Renderer JS 注入 World | 默认 Main World（页面世界）；可选 isolated/node |
| JS 执行核心机制 | napi-bridge：解析 napi_* 导出 → hook `napi_register_module_v1` 抓 `napi_env` → 通过 `uv_async_t` 排队到 V8 线程 → `napi_run_script` |

### 1.2 非目标（明确划出）

- 不做 spawn + suspend 注入（仅 attach）
- 不做反检测/绕过 EDR；目标是普通 Electron 程序
- 不内置沙箱：用户脚本是任意 JS（逆向场景预期）
- 不做多 Electron 版本兼容矩阵；fixture 锁定单一 Electron 版本
- 不实现 V8 内部 pattern-scan fallback（保留扩展点，第一期不做）
- 不支持一次会话同时管理多个目标进程

---

## 2. 高层架构

```
┌─────────────────────┐         enum / open
│  positron-host      │ ───────────────────────► [target electron.exe]
│  • 进程枚举/选择    │  CreateRemoteThread     ┌──────────────────────┐
│  • REPL/脚本运行    │  + LoadLibraryW ───────►│  payload DLL
│  • Named Pipe 客户端│                         │  ├ pe-resolver       │
│  • 命令编排         │ ◄── \\.\pipe\positron-* │  ├ napi-bridge       │
└─────────────────────┘     双向 JSON 帧        │  ├ hook-engine       │
                                                │  ├ ipc-server        │
                                                │  └ js-executor       │
                                                └──────────────────────┘
```

### 2.1 拓扑要点

- 一次会话一个目标。多目标场景由 host 多实例提供
- 通信走每实例独立的 Named Pipe，pipe 名 `\\.\pipe\positron-<pid>`
- Payload 内所有跨线程任务统一通过 `uv_async_t` 投递到 V8 线程，避免 isolate 跨线程访问
- 位数严格匹配：x64 host 注入 x64 目标；启动时检查目标 PE 拒绝错配

---

## 3. 组件分解

### 3.1 Host 端（`positron-host.exe`）

| 组件 | 职责 | 依赖 |
|---|---|---|
| `process-enum` | `CreateToolhelp32Snapshot` 列进程；解析命令行 `--type=renderer` 区分；标注父子关系 | psapi, win32 |
| `injector` | `OpenProcess` → `VirtualAllocEx` → `WriteProcessMemory` → `CreateRemoteThread(LoadLibraryW)`；位数检查、超时回滚 | kernel32 |
| `pipe-client` | 连接 `\\.\pipe\positron-<pid>`，长度前缀 + JSON 帧双向收发 | win32 |
| `repl` | 行编辑、历史、点命令分发；裸文本默认走 `.eval` | replxx 或同类 |
| `cli` | 子命令：`list` / `attach <pid>` / `script <file>`；选项 `--type main\|renderer` | CLI11 |

### 3.2 Payload 端（`positron.dll`）

| 模块 | 职责 |
|---|---|
| `pe-resolver` | 异步线程内对当前进程候选模块解析导出表，构建 `name → RVA` 映射，定位 napi 符号。候选模块查找顺序：`GetModuleHandle(NULL)`（即主 exe，Electron 多版本将 napi 静态链接到此） → `node.dll` → `libnode.dll` → `electron.exe`。第一个含 `napi_register_module_v1` 导出的模块作为 napi 符号宿主 |
| `napi-bridge` | (1) hook `napi_register_module_v1` 抓首个 `napi_env`；(2) 通过 `napi_get_uv_event_loop` 拿 `uv_loop_t`；(3) 维护 `uv_async_t` + 任务队列；(4) 提供 `RunOnV8Thread(fn)` API |
| `hook-engine` | MinHook 包装：`install(symbol, detour, &original)`、句柄管理、线程安全 enable/disable；用户级 hook 命中走代理 detour，事件经 ring buffer 投递回 host |
| `js-executor` | `eval(code, world)` —— 在 V8 线程上 `napi_create_string_utf8` + `napi_run_script`，结果带类型标签序列化（`{type, json}`）；renderer 时枚举 `Isolate->GetCurrentContext()` 链选 main world |
| `ipc-server` | Named Pipe server，单连接，命令 dispatcher 路由到 `js-executor` / `hook-engine` |
| `bootstrap` | `DllMain` 仅 `CreateThread` 启动初始化线程；初始化按序：pe-resolver → 安装 napi 抓取 hook → 启动 pipe server |

### 3.3 共享层

- `wire/`：协议定义（命令/事件 enum、JSON schema）host 与 payload 共享
- `util/`：日志（payload 端 `OutputDebugString` + pipe 转发）

---

## 4. 数据流

### 4.1 Attach 流程

```
host                                      target
  │ enum & list                              │
  │ ── user picks PID + type ──►            │
  │ create pipe \\.\pipe\positron-<pid>      │
  │ OpenProcess(PROCESS_ALL_ACCESS)          │
  │ VirtualAllocEx + WriteProcessMemory      │
  │ CreateRemoteThread(LoadLibraryW, dll)──►│
  │                                          │ DllMain → spawn init thread
  │                                          │   pe-resolver.scan()
  │                                          │   hook napi_register_module_v1
  │                                          │   pipe-server.listen()
  │ ◄── pipe connect (HELLO {symbols, ver}) │
  │                                          │ (首个 napi_register_module_v1
  │                                          │  → cache napi_env
  │                                          │  → 取 uv_loop, 装 uv_async_t)
  │ ◄── EVENT {ready: true}                 │
```

`HELLO` 携带：解析到的 napi 符号列表、Electron 版本字符串、目标 type。Host 校验后进入 REPL 提示符。

### 4.2 `eval` 命令往返

```
REPL: > 1+1
host  ─► CMD {id:1, kind:"eval", code:"1+1", world:"main"}
payload:
  ipc-server.recv → js-executor.eval()
  RunOnV8Thread(λ):
    napi_open_handle_scope
    napi_create_string_utf8(code)
    napi_run_script(...) → result
    serialize(result) → {type:"number", json:"2"}
    napi_close_handle_scope
host ◄─ RESP {id:1, ok:true, result:{type:"number", json:"2"}}
REPL: 2
```

异常：`napi_run_script` 报错 → `napi_get_and_clear_last_exception` → `RESP {ok:false, error:{message, stack}}`。

### 4.3 `hook` 命令

```
REPL: > .hook napi_throw on('hit', e => log(e.args[1]))
host ─► CMD {id, kind:"hook.install", target:"napi_throw", script:"..."}
payload:
  hook-engine.install(napi_throw):
    分配 trampoline detour
    detour: push call ctx 到 ring buffer
    MH_EnableHook
  script 编译为 v8 函数缓存
host ◄─ EVENT {kind:"hook.hit", id, args:[...]}（异步流）
```

ring buffer + `uv_async` 通知主线程；溢出策略：丢弃 + 递增 `dropped_count`，host 显示。

### 4.4 Detach

`.detach` → 禁用所有 hook → 解绑 `uv_async_t` → 关 pipe → DLL 留在进程内不卸载。**有意为之**：trampoline 难安全回收，强行卸载易崩。

---

## 5. 错误处理与边界

### 5.1 注入阶段

| 场景 | 处理 |
|---|---|
| `OpenProcess` 拒绝 | 报错："需以管理员运行" 或 "目标受保护"；不重试 |
| `LoadLibraryW` 失败（远程线程退出码=0） | 报错并提示常见原因（位数、依赖 DLL） |
| 位数不匹配 | 启动时检测 PE，提前拒绝 |
| Pipe 超时未连接 | 放弃，提示 "DLL 已加载但未握手"，引导查 DebugView |

### 5.2 napi_env 抓不到

- 30s 看门狗，未拿到 env 发 `EVENT {ready:false, reason:"no_napi_env_seen"}`
- REPL 进入 hook-only 模式：hook 可用，eval 不可用直到 ready
- 兜底（保留扩展点）：v8 内部签名 fallback —— 第一期不实现

### 5.3 V8 异常

- 用户脚本抛错 → 提取 message/stack 序列化回 host
- detour script 抛错 → 不传给原函数；记 `EVENT {kind:"hook.script_error"}`
- 死锁/无限循环 → 暂不处理；REPL 提示超时

### 5.4 Renderer Main World 选择

- 多 frame / 多 context 识别策略：
  - 启动时枚举 `Isolate` 下所有 `Local<Context>`（通过 hook `v8::Context::New` 维护活动 context 列表，或在 napi_env 拿到时遍历）
  - main world 识别启发式：context 上 `Global()->Get("window")` 不为空且 `window.top === window`
  - 默认选首个匹配；`.world.list` 列所有 context，`.eval --world=<index>` 显式选
- 页面未加载完最多等 5s（轮询 `document.readyState`）

### 5.5 Hook 安全

- 同一 target 重复 install → 拒绝
- ring buffer 用 SRWLock 保护，满了丢弃 + 计数
- detour 第一件事 SEH（`__try`）兜底，hit 失败不带垮目标

### 5.6 资源清理

- `DLL_PROCESS_DETACH` 仅禁用 hook、关 pipe；不 `MH_Uninitialize`
- Host 崩溃 → pipe 断 → payload 保持现状不卸载，目标不受影响

### 5.7 命令注入与路径

- pipe 名固定前缀 + PID，不接受用户输入
- 用户 JS 不沙箱（预期能力）
- DLL 注入用绝对 wide path

---

## 6. 测试策略

### 6.1 金字塔

| 层 | 范围 | 内容 |
|---|---|---|
| 单元 | host & payload 各自 | PE 解析、wire 协议序列化、ring buffer、hook-engine 包装 |
| 集成（核心） | 端到端 | 自带最小 Electron fixture，host attach → eval → assert |
| 手工冒烟 | dev | 真实 Electron 程序（VS Code/Discord）跑 happy path |

### 6.2 Electron Fixture

`tests/fixtures/sample-electron-app/`：

- `package.json` + `main.js`（BrowserWindow + 本地 `index.html`）
- `index.html`：含 `window.__positron_test_marker__ = "ok"`、setInterval 计数器
- `tests/fixtures/sample-addon/`：node-addon-api 写的最小 native addon，确保 `napi_register_module_v1` 被调用
- 启动脚本：`npm i electron --no-save` → spawn → 等 ready → 测试 → kill

### 6.3 关键集成用例

- `attach_main_eval_returns_value`：eval `1+1` → `2`
- `attach_renderer_reads_dom`：eval `document.title` → fixture title
- `attach_renderer_main_world_isolation`：eval 看到 `window.__positron_test_marker__`
- `hook_napi_throw_fires_event`：fixture 故意 throw，断言收到 `hook.hit`
- `detach_then_target_keeps_running`：detach 后目标存活
- `napi_env_watchdog_no_addon`：移除 fixture 的 addon，断言进入 hook-only 模式

### 6.4 Payload 可测性

- `js-executor` 接受 `IThreadDispatcher` 抽象；测试用 `InlineDispatcher` 同步执行
- `pe-resolver` 纯函数化（base → symbol map），host 单测可跑同一份代码

### 6.5 不测

- detour 内 SEH 命中路径（需 fault injection，第一期跳过）
- Electron 版本兼容矩阵（fixture 单版本）
- Wire 协议 fuzz（仅本地 pipe，威胁面小）

---

## 7. 项目布局演进

当前是干净的 VS 解决方案：

```
positron.slnx
├ positron/                ← 现有 DLL 项目
└ positron-test/           ← 现有 console 项目
```

目标布局：

```
positron.slnx
├ host/                    ← positron-test 重命名/重写为 host CLI
│  ├ src/
│  │  ├ cli/
│  │  ├ injector/
│  │  ├ pipe_client/
│  │  ├ process_enum/
│  │  └ repl/
│  └ host.vcxproj
├ payload/                 ← 现有 positron 项目重组
│  ├ src/
│  │  ├ bootstrap/         (DllMain + init thread)
│  │  ├ pe_resolver/
│  │  ├ napi_bridge/
│  │  ├ hook_engine/
│  │  ├ ipc_server/
│  │  └ js_executor/
│  └ payload.vcxproj
├ shared/                  ← wire / util
│  ├ wire/
│  └ util/
├ third_party/
│  ├ minhook/
│  ├ cli11/
│  ├ replxx/
│  └ json/                 (nlohmann/json)
├ tests/
│  ├ unit_host/
│  ├ unit_payload/
│  ├ integration/
│  └ fixtures/
│     ├ sample-electron-app/
│     └ sample-addon/
└ docs/
   └ superpowers/specs/
```

### 7.1 第三方依赖

- **MinHook**：inline hook 引擎
- **CLI11**：参数解析
- **replxx**：交互式 REPL（行编辑 + 历史）
- **nlohmann/json**：wire 协议序列化
- **node-addon-api**：测试 fixture addon（headers only）

引入方式：第一期用 git submodule 拉到 `third_party/`，由 vcxproj 直接引用头文件 + 源码（避免引入 vcpkg/CMake 多构建系统）。每个依赖引入前用 Chrome 浏览器查仓库主页确认最新 release。

---

## 8. 开放问题（实施期再决）

- **Electron 版本下限**：napi `register_module_v1` 是 Node 14+，Electron 13+ 应该都覆盖；fixture 用最新 stable
- **REPL 历史持久化路径**：暂用 `%APPDATA%\positron\history`
- **日志位置**：host 默认 stderr；payload `OutputDebugString` + pipe 转发到 host

---

## 9. 关键 Trade-offs 记录

| 决定 | 替代方案 | 选择理由 |
|---|---|---|
| napi-bridge | CDP / V8 pattern-scan | 用户原始思路；ABI 稳定；复杂度可控 |
| Attach-only | spawn+suspend | 用户明确要求；省去 hook `CreateProcessW` |
| Named Pipe | TCP / shared mem | 本机够用；无端口冲突；权限模型清晰 |
| Detach 不卸载 DLL | FreeLibrary | trampoline 难回收；目标进程优先稳定 |
| 单连接 pipe | 多连接 | 一次一目标的语义已经足够；简化 server |
| 自带 fixture | 测既有 Electron 程序 | CI 可重复；不依赖第三方版本 |
