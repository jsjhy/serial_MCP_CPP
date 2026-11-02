// GUI boundary: each serial session has its own tab, RichEdit, draft and incremental parser.
// main owns SessionManager; tabs retain shared session references. Only the UI thread accesses controls.
#pragma once
#include "mcp.hpp"

namespace sm
{
int gui(SessionManager& sessions, Restart& restart, Config config);
void native_tests(Core& core, LogWriter& log, Restart& restart);
int self_test();
} // namespace sm
