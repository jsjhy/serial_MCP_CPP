# Serial Monitor C++ 项目重建规格书

本文定义 Windows 串口监视器的源码结构、运行行为、外部接口、资源边界和验收条件。目标是在空目录中根据本文生成一个可编译、可运行、可测试的项目。兼容性以可观察行为和接口为准。窗口像素、原始图标字节和构建产物哈希随实现与工具链变化。

## 1. 产品与交付物

实现一个 Windows x64 C++20 图形子系统程序 `serial_mcp_gui.exe`，支持 WinAPI GUI 与 Streamable HTTP MCP，单个进程可同时提供两者。无 TUI、无自定义功能快捷键。界面文字和源码注释用英文。采用 Unicode WinAPI、RichEdit、重叠串口 I/O、Winsock 和标准库，不要求 Qt、WebView 或外部开发运行库。

| 文件 | 责任 |
|---|---|
| `CMakeLists.txt` | 编译、资源、发布、CTest |
| `cmake/mingw64.cmake` | MinGW x64 Windows 交叉工具链 |
| `app.rc`、`app.manifest`、`resource.h`、ICO | 嵌入多尺寸图标、公共控件清单和资源 ID。可用自制图标重建 |
| `src/main.cpp` | Unicode 入口、运行模式、诊断、启动会话、配置监听和退出 |
| `src/common.hpp/.cpp` | 配置、编码、字节输入、端口发现、通用资源所有权 |
| `src/json.hpp/.cpp` | JSON 值、严格解析和序列化，支持 Unicode 转义 |
| `src/text.hpp/.cpp` | 流式 UTF-8/ANSI 解码、逻辑行、样式、终端补丁 |
| `src/core.hpp/.cpp` | 串口、任务队列、状态快照、历史和日志写入 |
| `src/sessions.hpp/.cpp` | 会话注册、引用和关闭 |
| `src/mcp.hpp/.cpp` | HTTP、JSON-RPC、工具路由、重启 |
| `src/ui.hpp`、`src/gui.cpp` | GUI、资源、控件、工作区和隐藏窗口测试入口 |
| `src/tests.cpp`、`tests/simulated_serial_log.cpp` | 单元、自检、压力与可选合成日志测试 |
| `config.example.toml`、`README.md`、`.clang-format`、`format.py`、`verify.py`、`publish.cmake` | 配置示例、说明、排版、验收、发布 |

## 2. 配置与启动

按 CLI、TOML、内置默认值依次取值。默认读取工作目录的 `config.toml`，`--config PATH` 指定配置文件。路径支持 Unicode。TOML 支持所列标量和 `[gui]`，兼容旧名 `[tui]`。忽略未知配置项，已知项的非法值报错。CLI 支持 `--port`、`--baud`、`--mode`、`--mcp-port`、`--log-dir`、日志参数、`--watch-config`、`--help`、`--self-test`、`--stress-test`。诊断模式单独运行并用退出码报告结果。

| 配置 | 默认值与语义 |
|---|---|
| `port` | 空。启动时有明确端口则尝试连接，否则仅在发现唯一端口时自动连接 |
| `baud` | 115200。GUI 合法范围 1 到 4000000 |
| `mode` | `both` 同时启动。另有 `gui` 和 `mcp` |
| `mcp_port` | 8010 |
| `log_dir` | `logs` |
| `log_rotate` | 10485760 字节，完整批次之间轮转 |
| `log_keep_days` | 30 天 |
| `log_flush_bytes`、`log_flush_ms` | 4096 字节、100 毫秒 |
| `[gui].theme` | `dark` |
| `[gui].line_ending` | `crlf` |
| `[gui].hex_view`、`[gui].input_hex` | 均为 false |
| `[gui].max_lines` | 500，配置范围 50 到 2000，仅实时窗口 |

启动创建 `serial-1`。新建空白会话不自动连接。配置监听仅重新应用启动会话的端口和波特率，被删除的启动会话不复活。运行时新增会话不持久化。

## 3. 会话和串口

- 同时保有 1 到 16 个会话。编号从 `serial-1` 单调递增，进程内不重用。每个会话有独立 Core、LogWriter、端口状态、累计收发字节、队列、显示缓存和日志子目录。
- 管理器在锁内查询或更新目录，先取得共享引用再释放锁。移除时先摘除条目，再在锁外阻止新操作并停止线程。持有旧引用的调用不能重开已停止会话。GUI 每页持有自己的共享引用。
- Windows 端口独占打开，支持大于 COM9 的端口名。配置波特率、8 数据位、无校验、1 停止位，使用重叠读写和可取消的等待。使用驱动独占语义报告重复占用。断开与退出取消 I/O，等待持有句柄的线程结束后释放句柄。
- 每个会话由接收线程、任务线程和日志线程处理。GUI 与 MCP 共享有界发送队列。GUI 操作提交后异步取结果。成功仅表示驱动接受写入。关闭和失败要给等待任务一个明确结果。
- 支持文本转义 `\\r`、`\\n`、`\\t`、`\\0`、`\\\\`、`\\xNN`，以及十六进制字节输入。GUI 默认追加 CRLF，MCP 无隐式行尾。
- 显示队列上限 1024 条或 4 MiB，历史上限 4096 条或 4 MiB。丢弃最旧显示记录并累加 `display_dropped`，恢复渲染时提示跳过。磁盘日志与显示丢弃独立。每次 GUI 消费最多 64 条或 32 KiB，批量写入 RichEdit。

## 4. GUI

使用原生 Windows 窗口。标题栏左侧是图标和系统菜单，右侧使用系统最小化、最大化、关闭。标题区包含会话标签，显示 ID、端口、ON/OFF，右端 X 移除该页，至少保留一页。紧随标签的 + 创建并选中新页。可拖动空白标题区、双击最大化和从边缘缩放。系统菜单 About 显示版本、MCP 协议版本与可点击项目地址。

每页包含 Port、Baud、Connect/Disconnect、Refresh、EOL、Clear，显示 CONNECTED、DISCONNECTED 或 NOT_FOUND、已写日志文件和行号、串口会话 ID、MCP 状态。连接按钮随实际连接状态变化，在途禁用。切换页继续收发和写盘，后台或最小化暂停渲染。各页独立保留草稿、选择、滚动、连接参数和日志查看状态。

实时 RichEdit 显示 RX、TX、状态和低对比度时间戳，支持 ANSI 16/256/真彩前景色、Unicode、复制及自动滚底。只允许编辑历史之后的草稿。Enter 发送，失败保留草稿，成功清空。Clear 清历史并保留草稿。关闭软折行。按 `max_lines` 裁掉最旧逻辑行，计入当前输入行。另有 `max_lines × 1024` UTF-16 单元的裁剪阈值，触发后优先从行边界裁剪。裁剪维护历史边界和选区位置，不触碰草稿。实时窗口始终显示未经过滤的输出。

实时窗口右侧 `>` 展开只读日志窗口，`<` 收起。默认收起，展开后并排显示。日志窗口关闭自动折行，长行可横向滚动，保留文件中的换行。展开和 **Reload log** 从当前会话日志目录读取最近提交的当前文件，包含从第一行到最新已提交行的快照。轮转后重载新文件。无文件时提示 `No log has been written yet.`。读取在后台执行，控件更新在 GUI 线程，读取失败显示英文信息。载入文本后显式应用主题前景色。

日志窗口独有 Include、Exclude、Enable、Apply、Match case、搜索框、Previous、Next。Include 和 Exclude 是独立输入框，默认均为空。启用过滤后，一行必须包含非空的 Include 关键词，且不得包含非空的 Exclude 关键词。Enable 和 Match case 默认开启。Apply 对当前文件完整快照逐行重筛，空关键词显示全部。Enable 启停已应用谓词。Match case 同时控制过滤和搜索，改变后重载已应用过滤。搜索以当前选择为起点，只在当前可见日志内容中向前或向后查找，遇到边界提示，无回绕。筛选和搜索不修改日志文件。

## 5. 文件日志

每会话写入 `log_dir/serial-N/serial_<timestamp>_<sequence>.log`，文件名不碰撞。文件为可读 UTF-8，去掉 ANSI，RX 无方向前缀，TX 使用 [TX] 标记，STATUS 保留方向标记。跨接收块保存 UTF-8、ANSI 和残行解码状态。未完成行中已解码的 UTF-8 内容也按大小或定时自动写入文件，后续内容追加至原行。未完成 UTF-8 序列保留解码状态。日志查看器包含已保存的末尾残行。成功写入并 flush 后发布文件名和一基行号，首行前显示 pending。

按大小在完整批次间轮转，软阈值允许跨过。只清理该会话目录内属于本应用且超龄的文件。日志队列默认约 16 MiB / 4096 条，慢磁盘通过背压阻止无限队列增长。写盘失败保留待写内容并报告，持续错误可影响吞吐和尾部保存。正常退出先唤醒受背压生产者，再停止 Core，最后排空日志并释放文件。区分排队、写盘、flush 与断电持久化。

## 6. MCP 对外契约

默认监听所有 IPv4 网卡，端点为 `http://127.0.0.1:8010/mcp`。无账号认证，访问者能够操作串口。校验 HTTP Origin，拒绝非法来源。限制请求头和最大 1 MiB 请求体，四个网络工作线程，有界连接队列与超时。最多 128 个客户端协议会话，空闲五分钟过期。HTTP 连接关闭不删除逻辑会话。

- `POST /mcp` 接收 UTF-8 JSON-RPC 2.0，初始化返回 HTTP 200、`Mcp-Session-Id` 和协议版本 `2025-03-26`。返回 `serverInfo.name=serial-mcp-cpp`、`version=1.0.0`、`capabilities.tools={}`。后续请求使用该头。合法通知返回 202 空体，带 ID 的请求返回同 ID 结果。处理 ping、tools/list、tools/call 和 JSON-RPC 批处理。
- `GET /mcp` 返回 405，不提供 SSE。`DELETE /mcp` 删除指定客户端会话，成功返回 202，未知会话返回 404。其他路径返回 404，非法来源返回 403。缺失必要会话头或坏请求返回 400，未知会话返回 404，容量满返回 503。
- 工具目录定义 JSON Schema object、properties、required 和 `additionalProperties: false`。工具结果为 `{"content":[{"type":"text","text":"..."}],"isError":false}`，失败置 true 并返回英文原因。JSON-RPC 结构错误使用 JSON-RPC error 对象。
- `serial_id` 是设备会话 ID，与客户端 HTTP 会话头独立。open、close、send、read、set_dtr_rts、export_log 在只有一个设备会话时允许省略，多会话必须明确指定。remove_session 始终要求该字段。

| 工具 | 参数与结果 |
|---|---|
| list_ports | 无参数。文本 `available: COM8, COM9` 或 `no serial ports found` |
| list_sessions | 无参数。JSON 文本 `{"sessions":[...]}` |
| create_session | 无参数。创建空白会话并返回会话对象 JSON 文本，达到 16 时失败 |
| remove_session | 必填 `serial_id`。停止和移除，成功文本 `removed <id>`，禁止移除最后一个 |
| open | `port` 字符串、`baud` 整数均必填。允许 0 表示 115200，最高 4000000。成功文本 `opened` |
| close | 断开但保留会话，成功文本 `closed` |
| send | `ascii` 文本或 `hex` 字符串。非空 hex 优先。成功报告写入字节数 |
| read | `limit` 是最近记录数，默认 20，限制到 1–4096。只输出其中 RX，默认十六进制。`ascii=true` 使用可打印 ASCII，其他字节显示点。记录带时间戳 |
| set_dtr_rts | `dtr` 与 `rts` 均为必填布尔值，成功文本 `ok` |
| export_log | `limit` 默认 20，范围同 read。返回内存中每条记录的时间、方向和字节数 |
| restart | 无参数。调度整个进程重启，合并重复请求 |

会话对象字段为 `serial_id, port, baud, connected, rx_bytes, tx_bytes, display_dropped, notice, log_dir, log_file, log_error`。read 与 export_log 无数据时返回 `no data`，输出超过约 2 MiB 时附截断提示。restart 先响应，约一秒后发起退出。用同一 EXE 的内部助手等待旧进程退出，最多 30 秒，沿用原参数及工作目录启动新实例。重启后客户端重新初始化。

## 7. 构建、发布与验收

CMake 最低 3.21，启用 CXX 与 RC，生成唯一 WIN32 目标 `serial_mcp_gui`。定义 UNICODE、_UNICODE、NOMINMAX、WIN32_LEAN_AND_MEAN 和 _WIN32_WINNT=0x0A00。链接 comctl32、dwmapi、gdi32、user32、shell32、ws2_32、bcrypt。MinGW 使用 `-static -static-libgcc -static-libstdc++ -municode`。MSVC 使用静态运行库和 `/utf-8`。原生 Windows 构建命令如下。

```powershell
cmake -S . -B build/cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --config Release --parallel 4
ctest --test-dir build/cmake -C Release --output-on-failure
python format.py --check
```

每次链接成功后复制 EXE 至 dist，比较构建和发布文件 SHA256，失败时构建失败。默认 publish 目标也覆盖无须重链的情况。复制 README，并仅在不存在时从示例生成 dist/config.toml。运行中的旧 EXE 锁住文件时，关闭后重试。格式采用四空格、Allman 花括号、逐行声明和英文生命周期注释。

| 验收项 | 操作和预期 |
|---|---|
| self_tests | `--self-test` 覆盖 JSON、Unicode、命令编码、日志尾部、无效端口、隐藏 RichEdit 编辑边界和日志主题字体、标签隔离、会话回收及预热后窗口资源差异 |
| static_runtime_imports | Python 3.11+ 读取 PE 导入表，拒绝动态 GCC、C++、MSVC 开发运行库 |
| mcp_integration | 使用本地独立端口、临时日志目录、无效串口与 Windows Job 管理进程。核对 11 项工具、会话隔离和歧义拒绝、Origin 拒绝、HTTP 会话回收、重启与 GUI 恢复 |
| 日志窗回归 | 合成已提交文件，确认默认收起。展开后显示全部提交行，深色和浅色下可读。核对 Include/Exclude、Match case、Previous/Next、Reload log、文件轮转及会话切换 |
| 实时裁剪回归 | 注入超过 max_lines 的短行，检查只保留最新行且草稿完整。再注入长行，观察字符裁剪、滚动和选区行为。检查完整记录仍落盘 |
| 合成压力 | `--stress-test` 使用三个合成会话，默认 18 秒，环境变量 `SERIAL_MCP_STRESS_SECONDS` 支持 5–300 秒。输出内存、句柄、GDI/USER、线程与最长 GUI 消息时间，清理临时日志 |
| 真机验收 | 在实际 Windows 串口上验证 COM 枚举、独占冲突、收发、DTR/RTS、断开、拔插和长时间接收。单独记录设备、驱动、波特率、持续时间、日志及资源变化 |

可选合成日志回归通过 `-DSERIAL_MCP_SIMULATED_TESTS=ON` 编译。远程只读协议检查用 `python verify.py --url http://WINDOWS_HOST:8010/mcp`，验证初始化、ping、工具目录及会话释放。自动化、无效端口与合成压力的结果不得归入真机验收。

从空目录实现时，依次交付通用层和资源、会话及日志、串口 Core、MCP、GUI、诊断与发布脚本。完成后运行完整验收表，把实际结果与尚未执行的真机项目分别记录。
