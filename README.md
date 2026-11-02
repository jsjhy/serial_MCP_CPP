# Serial MCP C++ — Multi-port WinAPI GUI

独立 C++20 复刻，使用 WinAPI、RichEdit、Windows 串口重叠 I/O 和 Winsock，不依赖 Rust、Qt、WebView 或第三方运行时。所有源文件在 src；仅保留 GUI/MCP，不再构建 TUI。GUI 没有自定义功能快捷键。

## 构建与发布

**构建直接使用 CMake 3.21+，不再额外包装构建脚本。** Windows 需要 MinGW-w64 或 MSVC：

```text
cmake -S . -B build/cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --config Release --parallel 4
ctest --test-dir build/cmake -C Release --output-on-failure
```

配置后也可用 `cmake --build build/cmake --config Release --target check --parallel 4` 一次完成构建、发布和测试。MSVC 可在新目录使用本机 Visual Studio 生成器，不传上述 MinGW 生成器参数。

每次成功链接自动复制 EXE 到 dist 并校验 SHA256；默认构建的 publish 目标在没有重新链接时也同步 GUI 产物。保留用户已有 config.toml。运行中的 EXE 如锁定需要更新的发布文件，构建会明确失败。GCC 静态链接 C++/线程运行库，MSVC 使用 /MT；系统 DLL 仍由 Windows 提供。

排版和 MCP 验证改用 **Python 3.11+ 标准库**，无需 PowerShell。CMake 检测到 Python 时，CTest 还会检查 EXE 的 PE 导入表，拒绝动态 GCC/C++/MSVC 开发运行库；未安装 Python 仍可构建，配置输出会说明缺少此项检查。PE 检查不等同于完整依赖安全审计。

Linux/macOS 可安装 MinGW-w64，执行以下命令交叉编译 Windows x64 EXE：

```text
cmake -S . -B build/cross -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cross --parallel 4
```

工具链需要 `x86_64-w64-mingw32-g++` 和 `x86_64-w64-mingw32-windres`。应用仍依赖 WinAPI，不能在 Linux/macOS 原生运行；交叉配置不注册 Windows 运行时自检，有 Python 时仍可运行 PE 检查。更换生成器/工具链应使用独立构建目录。本次实际验证环境为 Windows GCC，尚未在 Linux/macOS 或 MSVC 实测。

产物只有 `dist/serial_mcp_gui.exe`。默认 `--mode both` 为 GUI + MCP；`--mode gui` 仅界面，`--mode mcp` 仅服务。支持 `--port COM8 --baud 115200 --mcp-port 8010 --config config.toml --log-dir logs --watch-config` 及日志参数。启动会话自动连接指定端口或唯一可用端口，多设备时由用户选择；新建会话不自动连接。原 TUI 目标、源码和发布 EXE 已移除。

## 使用

程序图标来自项目中的 `ZhiNanXinYi.bmp`，转换为多尺寸 `ZhiNanXinYi.ico` 后嵌入 EXE，运行时无需外部图片。点击窗口左上角图标，在系统菜单中选择 About...，可查看版本、当前 MCP 协议版本和仓库地址 https://github.com/jsjhy/serial_MCP_CPP.git。

GUI 顶部串口/波特率组合框和 共用的 Connect / Disconnect 按钮及 Refresh 按钮；连接按钮按实际状态显示 Connect 或 Disconnect，操作期间暂时禁用，完成后恢复；MCP 改变连接状态时也会同步。ASCII/HEX 同步切换输入/显示，EOL 切换行尾，Clear 清空显示但保留草稿。直接在显示区末尾输入，Enter 发送（空 Enter 可发 CRLF），失败保留；窗口关闭按钮退出。标题栏使用浅色系统样式，Windows 11 外边框不绘制额外描边。鼠标选择后右键或 Ctrl+C 复制，原生剪切/粘贴可用于草稿。选中历史时暂停显示刷新，日志继续。状态为 CONNECTED / DISCONNECTED / NOT_FOUND；单独显示 log 文件名，底部单行显示当前页的 serial_id 和 MCP 会话状态，可直接对应 MCP 工具参数。

顶部标签页显示独立会话 ID、端口及 ON/OFF；末尾 `+` 新建并选中新页，每页右侧 `X` 关闭并移除对应会话（包括后台页），至少保留一页。每页独立保留草稿、显示、滚动/选择、ASCII/HEX、行尾、波特率及连接状态。切换标签页不关闭串口，后台页继续接收和记录。GUI 支持 ANSI 16/256/真彩前景色和暗淡时间戳。

最多 16 个活动串口会话。每个 SerialSession 拥有独立 Core（接收/任务线程）、LogWriter（日志线程）、有界发送队列、历史与显示缓存。SessionManager 目录锁不参与设备 I/O 或等待线程退出；删除时撤下目录并停止/回收资源，在途请求持有 shared_ptr，旧引用不能重新打开已删除会话。三个线程/会话，缓存预算按会话累加。同一物理端口不能重复占用，Windows 驱动的独占打开失败会明确报告。

文本输入支持 `\\r \\n \\t \\0 \\\\ \\xNN`，默认 CRLF；Hex 精确发送，不附加行尾。成功提示仅确认驱动写入，需读设备回复确认执行。

## MCP

端点 `http://127.0.0.1:8010/mcp`，监听全部 IPv4 网卡，访问者可操作设备；校验 HTTP Origin，不提供账号认证。实现 Streamable HTTP 的 JSON 响应路径，GET 返回 405（不提供 SSE 推送/恢复），POST 支持初始化、通知、ping、tools/list、tools/call，DELETE 关闭会话。协议版本协商返回 2025-03-26；客户端需要支持该版本。依据 [MCP HTTP 规范](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports) 实现并做独立 HTTP 测试。

十一项工具：list_ports、list_sessions、create_session、remove_session、open、close、send、read、set_dtr_rts、export_log、restart。

- `list_sessions {}`：返回 JSON 文本，包含 sessions 数组，每项有 serial_id、port、baud、connected、收发计数、日志目录/文件及错误。
- `create_session {}`：创建空白会话，返回包含 serial_id 的 JSON 文本。
- `remove_session {"serial_id":"serial-2"}`：停止并移除指定会话，至少保留一个；日志退出写盘失败时在移除后报告错误。
- `open {"serial_id":"serial-2","port":"COM9","baud":115200}`：打开指定会话。
- `send {"serial_id":"serial-2","ascii":"help\\r\\n"}`：向指定会话发送；也支持 hex，MCP 不自动加行尾。
- close、read、set_dtr_rts、export_log 同样接受 serial_id。仅一个串口会话时允许省略；多个时省略会报错。此 ID 与 HTTP 的 MCP-Session-Id 无关，GUI 标签页切换不会改变 MCP 的路由。

read 是会话内存快照，ASCII 为字节可打印预览；export_log 返回记录摘要。restart 重启整个进程，影响全部串口和客户端；先返回安排结果，一秒后退出。隐藏助手最多等待旧进程 30 秒，沿用启动参数/工作目录启动，重复请求合并。运行时新增会话及其参数暂不持久化，重启只恢复启动会话；客户端需重新初始化并创建额外会话。

`python verify.py` 使用独立服务端口和无效串口名，验证十一项工具、明确目标/歧义拒绝、端口参数与日志目录隔离、20 次串口会话创建回收、50 次 HTTP 会话回收、Origin 拒绝、真实进程重启/重新连接和 GUI 窗口恢复。测试通过专用 Windows Job 清理本次进程树，CMake 检测到 Python 后也将其注册为 CTest。

任意平台可执行 `python verify.py --url http://WINDOWS_HOST:8010/mcp`，只检查初始化、ping、十一工具目录和会话释放，不操作远端串口或请求重启。远程模式不执行本机资源/窗口检查，网络需能访问该服务。

HTTP MCP 客户端会话最多 128 个、五分钟空闲过期；HTTP 请求体最大 1MiB、四个固定网络线程、有界连接队列与超时。每个串口的 GUI/MCP 发送共用该串口的有界任务队列。慢磁盘时日志队列到上限后背压生产者，可能影响接收吞吐，不声称无限负载无损。

## 日志、配置与测试边界

日志按 `log_dir/serial-N/` 隔离，各自轮转并清理其目录中的过期文件。日志 UTF-8，RX 不带方向前缀，TX/STATUS 保留。跨包 UTF-8/ANSI 解码，按大小轮转、按保留天数清理；程序退出刷新尾部，flush 不等于断电持久化。失败保留待写批次并报告错误，退出遇到持续磁盘故障可能丢失未写尾部。CLI > 配置 > 默认；支持项目示例所示的 TOML 标量及 [gui]（兼容旧 [tui] 表名），不是通用 TOML 解析器。--watch-config 仅重新应用启动会话的 port/baud；若该会话被删除，不会复活它。其他会话通过 GUI/MCP 配置。

`dist/serial_mcp_gui.exe --self-test` 验证 JSON/Unicode/命令编码、日志尾部、无效端口错误、真实隐藏 RichEdit 输入边界，并在预热后创建/销毁 100 个窗口比较 GDI/USER/句柄计数。另外验证隐藏多标签页隔离、独立队列、16 会话上限及 30 次会话回收。该测试不访问真实设备、不改系统剪贴板，不代表真实串口、拔插或长期运行验证。

## 代码排版

源代码统一使用四空格缩进、Allman 花括号、约 100 列行宽；每行一个语句，状态成员分行声明，长参数列表和控制流展开，函数之间留空行。规则保存在 `.clang-format`，源码、脚本和构建配置统一使用英文注释，保留职责、所有权、线程和退出边界的说明。

首次安装开发工具：`python -m pip install --target build/format-tools clang-format==23.1.3`。

- `python format.py`：格式化全部 C++ 源文件和头文件。
- `python format.py --check`：只检查排版，不修改文件，适合提交前检查。

格式工具仅安装在忽略的 build 目录，不随 EXE 发布，也不是运行依赖。

Python 脚本均从自身位置定位项目，可从任意工作目录调用；`python format.py --help`、`python verify.py --help` 查看选项。排版工具优先使用项目内安装，其次 PATH；可用 `--formatter PATH` 指定。

独立检查 EXE 的静态运行库导入：`python verify.py --binaries dist/serial_mcp_gui.exe`，不启动程序。
