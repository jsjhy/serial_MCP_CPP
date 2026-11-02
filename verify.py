#!/usr/bin/env python3
"""MCP verification: isolated Windows GUI integration or remote protocol checks on any host.

Local tests use invalid serial names and a dedicated Windows Job that owns all test
process descendants, including replacements. --url checks handshake, ping, tool discovery
and session deletion only; it never operates a remote device or restarts its server.
These tests do not validate real hardware or long-running stability."""

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


def check_imports(path):
    """Read ordinary PE imports and reject dynamic compiler runtimes or malformed files.

    This checks release dependencies, not full PE security. System DLLs remain OS-provided.
    RVAs must resolve inside file-backed section data, not merely virtual address ranges."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"Not a PE executable: {path}")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    count, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from("<H", data, optional)
    if magic not in (0x10B, 0x20B):
        raise ValueError("Unsupported PE optional header")
    directory = optional + (112 if magic == 0x20B else 96)
    import_rva, import_size = struct.unpack_from("<II", data, directory + 8)
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        _, address, size, raw = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((address, size, raw))

    def file_offset(rva, length=1):
        for address, size, raw in sections:
            if address <= rva and rva + length <= address + size:
                result = raw + rva - address
                if result + length <= len(data):
                    return result
        raise ValueError(f"Invalid PE RVA: {rva:#x}")

    imports = []
    if import_rva:
        terminated = False
        for delta in range(0, import_size - 19, 20):
            entry = struct.unpack_from("<IIIII", data, file_offset(import_rva + delta, 20))
            if not any(entry):
                terminated = True
                break
            name = bytearray()
            for index in range(260):
                character = data[file_offset(entry[3] + index)]
                if character == 0:
                    break
                name.append(character)
            else:
                raise ValueError("Unterminated DLL name")
            imports.append(name.decode("ascii"))
        if not terminated:
            raise ValueError("Unterminated PE import table")
    forbidden = ("libgcc", "libstdc++", "libwinpthread", "vcruntime", "msvcp")
    if any(token in name.lower() for name in imports for token in forbidden):
        raise ValueError(f"Dynamic compiler runtime found: {imports}")
    print(f"{path.name}: system imports: {', '.join(imports)}")


ROOT = Path(__file__).resolve().parent
TOOLS = {"list_ports", "open", "close", "send", "read", "set_dtr_rts", "export_log", "restart", "list_sessions", "create_session", "remove_session"}


def require(condition, message):
    """Fail the contract explicitly; unlike assert, this remains active under python -O."""
    if not condition:
        raise RuntimeError(message)


class Client:
    """Synchronous HTTP client with an instance-owned session and a five-second request timeout."""

    def __init__(self, url):
        self.url = url
        self.session = None
        self.request_id = 10
        # Bypass proxies for loopback tests; remote URLs retain host proxy settings.
        handlers = [urllib.request.ProxyHandler({})] if urllib.parse.urlparse(url).hostname in (
            "127.0.0.1", "localhost", "::1"
        ) else []
        self.opener = urllib.request.build_opener(*handlers)

    def http(self, payload=None, method="POST", extra_headers=None):
        """Return status, headers and decoded JSON; preserve HTTP errors for Origin assertions."""
        headers = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json"}
        if self.session:
            headers["mcp-session-id"] = self.session
            headers["MCP-Protocol-Version"] = "2025-03-26"
        headers.update(extra_headers or {})
        data = json.dumps(payload).encode("utf-8") if payload is not None else None
        request = urllib.request.Request(self.url, data=data, headers=headers, method=method)
        with self.opener.open(request, timeout=5) as response:
            body = response.read()
            return response.status, response.headers, json.loads(body) if body else None

    def initialize(self):
        """Create a fresh session and send the initialized notification; never reuse a stale ID."""
        self.session = None
        _, headers, body = self.http({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-03-26", "capabilities": {},
            "clientInfo": {"name": "cpp-python-check", "version": "1.0"}
        }})
        require(body["result"]["protocolVersion"] == "2025-03-26", "Protocol negotiation")
        self.session = headers.get("mcp-session-id")
        require(self.session, "Missing session ID")
        status, _, _ = self.http({"jsonrpc": "2.0", "method": "notifications/initialized"})
        require(status == 202, "Initialized notification")

    def rpc(self, method, params=None):
        """Execute JSON-RPC; remote errors and mismatched response IDs fail the test."""
        self.request_id += 1
        _, _, body = self.http({"jsonrpc": "2.0", "id": self.request_id,
                                "method": method, "params": params or {}})
        require(body.get("id") == self.request_id, "Response ID mismatch")
        require("error" not in body, f"RPC error: {body.get('error')}")
        return body["result"]

    def tool(self, name, arguments=None):
        return self.rpc("tools/call", {"name": name, "arguments": arguments or {}})

    def delete(self):
        """Release only this client session without stopping the server."""
        status, _, _ = self.http(method="DELETE")
        require(status == 202, "Delete session")
        self.session = None


def eventually(operation, seconds=15):
    """Wait for server/window readiness; rethrow the last failure when the deadline expires."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return operation()
        except (OSError, RuntimeError, KeyError, ValueError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


class WindowsJob:
    """Own the local test process tree; closing the kernel Job kills all test descendants.

    Construct only on Windows so other hosts can import this module and use --url.
    Assign the initial process before sending any test request. Restart helpers and
    replacement processes inherit the Job and remain within the cleanup boundary."""

    def __init__(self):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.handle = None
        self.process = None
        # Declare HANDLE return types explicitly to prevent 64-bit truncation to ctypes default int.
        for name, arguments, result in (
            ("CreateJobObjectW", [ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            ("SetInformationJobObject",
             [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
            ("AssignProcessToJobObject", [wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            ("QueryInformationJobObject",
             [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p],
             wintypes.BOOL),
            ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
            ("GetProcessHandleCount", [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            ("OpenProcess", [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            ("QueryFullProcessImageNameW", [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                            ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
        ):
            function = getattr(self.kernel, name)
            function.argtypes = arguments
            function.restype = result

        class BasicLimits(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                        ("flags", wintypes.DWORD), ("minimum", ctypes.c_size_t),
                        ("maximum", ctypes.c_size_t), ("active", wintypes.DWORD),
                        ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
                        ("scheduling", wintypes.DWORD)]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [("basic", BasicLimits), ("io", ctypes.c_uint64 * 6),
                        ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                        ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]

        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def start(self, command):
        """Launch an invalid-port test instance; terminate only this child if Job assignment fails."""
        self.process = subprocess.Popen(command, cwd=ROOT)
        if not self.kernel.AssignProcessToJobObject(self.handle, int(self.process._handle)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.process.kill()
            self.process.wait(timeout=5)
            raise error

    def pids(self):
        """Enumerate active Job PIDs; capacity exceeds the expected app/helper/replacement count."""
        class PidList(ctypes.Structure):
            _fields_ = [("assigned", wintypes.DWORD), ("count", wintypes.DWORD),
                        ("ids", ctypes.c_size_t * 64)]

        result = PidList()
        if not self.kernel.QueryInformationJobObject(self.handle, 3, ctypes.byref(result), ctypes.sizeof(result), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return list(result.ids[:result.count])

    def handles(self):
        result = wintypes.DWORD()
        if not self.kernel.GetProcessHandleCount(int(self.process._handle), ctypes.byref(result)):
            raise ctypes.WinError(ctypes.get_last_error())
        return result.value

    def application_pids(self, executable):
        """Count only executables at the same path; the Job also owns system descendants.

        Do not treat every Job PID as an app instance. Each temporary query handle is
        owned by this method and closed on every branch."""
        result = []
        for pid in self.pids():
            handle = self.kernel.OpenProcess(0x1000, False, pid)
            if not handle:
                continue  # A process that exits between enumeration and lookup is no longer active.
            try:
                size = wintypes.DWORD(32768)
                buffer = ctypes.create_unicode_buffer(size.value)
                if self.kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                    if Path(buffer.value).resolve() == executable.resolve():
                        result.append(pid)
            finally:
                self.kernel.CloseHandle(handle)
        return result

    def close(self):
        """Close the Job to kill all test descendants, then reap the Python child process object."""
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None
        if self.process:
            self.process.wait(timeout=10)


def check_gui(pid):
    """Confirm the replacement PID restored a visible top-level window without touching other apps."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    found = []

    @callback_type
    def inspect(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(window):
            found.append(window)
        return True

    require(user.EnumWindows(inspect, 0), "Window enumeration failed")
    require(found, "GUI was not restored")


def protocol_check(client):
    """Read protocol/tool metadata only; never call tools that change remote device state."""
    client.rpc("ping")
    tools = client.rpc("tools/list")["tools"]
    require(len(tools) == 11 and {tool["name"] for tool in tools} == TOOLS, "Expected eleven MCP tools")


def content(result):
    return "\n".join(item.get("text", "") for item in result.get("content", []))


def local_check():
    """Check eleven tools, session/resource isolation, Origin errors, restart and GUI restoration."""
    executable = ROOT / "dist" / "serial_mcp_gui.exe"
    require(executable.is_file(), f"Missing {executable}; build with CMake first")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    client = Client(f"http://127.0.0.1:{port}/mcp")
    job = WindowsJob()
    try:
        job.start([str(executable), "--mode", "both",
                   "--port", "COM_CPP_HTTP_TEST_INVALID", "--mcp-port", str(port),
                   "--log-dir", str(ROOT / "build" / f"http-check-{uuid.uuid4().hex}")])
        eventually(client.initialize)
        protocol_check(client)
        require(not client.tool("list_ports").get("isError"), "Port enumeration")
        invalid_open = client.tool("open", {"port": "COM_CPP_HTTP_TEST_INVALID", "baud": 115200})
        require(invalid_open.get("isError"), "Invalid port must fail")
        require(not client.tool("close").get("isError"), "Close must be idempotent")
        for name, arguments in (("send", {"ascii": "help\\r\\n"}), ("send", {"hex": "zz"}),
                                ("set_dtr_rts", {"dtr": True, "rts": False})):
            require(client.tool(name, arguments).get("isError"), f"Invalid/disconnected {name} must fail")
        require(content(client.tool("read", {"limit": 20, "ascii": True})) == "no data", "Unexpected RX")
        require("STATUS" in content(client.tool("export_log", {"limit": 20})), "Missing log summary")
        # Use invalid devices to verify routing/state/log isolation, not real serial RX throughput.
        primary = json.loads(content(client.tool("list_sessions")))["sessions"][0]
        secondary = json.loads(content(client.tool("create_session")))
        first_id = primary["serial_id"]
        second_id = secondary["serial_id"]
        require(first_id != second_id and primary["log_dir"] != secondary["log_dir"],
                "Independent session identity/log directories")
        ambiguous = client.tool("send", {"ascii": "help"})
        require(ambiguous.get("isError") and "specify serial_id" in content(ambiguous),
                "Ambiguous send must require explicit serial_id")
        require(client.tool("read", {"serial_id": "unknown"}).get("isError"), "Unknown session rejected")
        require(client.tool("read", {"serial_id": 7}).get("isError"), "Invalid session type rejected")
        for serial_id, port_name, baud in (
            (first_id, "COM_CPP_HTTP_A_INVALID", 9600),
            (second_id, "COM_CPP_HTTP_B_INVALID", 57600),
        ):
            require(client.tool("open", {"serial_id": serial_id, "port": port_name,
                                         "baud": baud}).get("isError"), "Invalid device rejected")
            require(content(client.tool("read", {"serial_id": serial_id})) == "no data", "Isolated RX")
        states = json.loads(content(client.tool("list_sessions")))["sessions"]
        states = {state["serial_id"]: state for state in states}
        require(states[first_id]["baud"] == 9600 and states[second_id]["baud"] == 57600,
                "Independent baud configuration")
        require(states[first_id]["port"] == "COM_CPP_HTTP_A_INVALID" and
                states[second_id]["port"] == "COM_CPP_HTTP_B_INVALID", "Independent port state")
        require(not client.tool("close", {"serial_id": second_id}).get("isError"), "Targeted close")
        require(not client.tool("remove_session", {"serial_id": second_id}).get("isError"), "Remove session")
        require(client.tool("read", {"serial_id": second_id}).get("isError"), "Removed session inaccessible")
        require(client.tool("remove_session", {"serial_id": first_id}).get("isError"), "Keep last session")
        require(content(client.tool("read")) == "no data", "Legacy single-session routing restored")
        for _ in range(3):
            created = json.loads(content(client.tool("create_session")))
            require(not client.tool("remove_session", {"serial_id": created["serial_id"]}).get("isError"),
                    "Session cleanup warmup")
        serial_before = job.handles()
        for _ in range(20):
            created = json.loads(content(client.tool("create_session")))
            require(not client.tool("remove_session", {"serial_id": created["serial_id"]}).get("isError"),
                    "Serial session cleanup")
        serial_after = job.handles()
        require(serial_after <= serial_before + 8, f"Serial resource growth: {serial_before} -> {serial_after}")
        print(f"20 serial sessions: handles {serial_before} -> {serial_after}")
        active = client.session
        # Compare warmed-up handle counts after session recycling; a short test cannot prove long-term leak freedom.
        for _ in range(5):
            client.initialize()
            client.delete()
        before = job.handles()
        for _ in range(50):
            client.initialize()
            client.delete()
        after = job.handles()
        require(after <= before + 8, f"Session handle growth: {before} -> {after}")
        print(f"50 HTTP sessions: handles {before} -> {after}")
        client.session = active
        try:
            # Origin is rejected before reading the body. An empty body avoids unread request bytes
            # causing a TCP reset, so this assertion checks an explicit 403 response.
            client.http(extra_headers={"Origin": "https://invalid.example"})
        except urllib.error.HTTPError as error:
            require(error.code == 403, f"Unexpected Origin response: {error.code}")
        else:
            raise RuntimeError("Invalid Origin accepted")
        require("restart scheduled;" in content(client.tool("restart")), "Restart response")
        require("already scheduled" in content(client.tool("restart")), "Duplicate restart response")
        old_pid = job.process.pid
        job.process.wait(timeout=15)
        eventually(client.initialize)

        def replacement():
            pids = job.application_pids(executable)
            require(len(pids) == 1 and pids[0] != old_pid, f"Expected one replacement PID: {pids}")
            return pids[0]

        new_pid = eventually(replacement)
        eventually(lambda: check_gui(new_pid), seconds=5)
        protocol_check(client)
        client.delete()
        print(f"PASS gui: eleven tools, sessions, input/Origin errors, restart {old_pid} -> {new_pid}, reconnect")
    finally:
        job.close()


def main():
    """Run isolated local integration tests, or protocol-only checks when --url is supplied."""
    parser = argparse.ArgumentParser(
        description="Verify local Windows GUI, or check an existing MCP server's protocol."
    )
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--binaries", nargs="+", type=Path, help="Check static runtime imports in PE files")
    modes.add_argument("--url", help="Existing MCP URL; protocol-only check, no device operations/restart")
    args = parser.parse_args()
    if args.binaries:
        for path in args.binaries:
            check_imports(path)
    elif args.url:
        client = Client(args.url)
        try:
            client.initialize()
            protocol_check(client)
        finally:
            if client.session:
                client.delete()
        print("PASS remote protocol: initialize, ping, eleven-tool discovery, DELETE")
    else:
        if os.name != "nt":
            parser.error("Local WinAPI execution requires Windows; use --url to check an existing server")
        local_check()


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError, struct.error, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
