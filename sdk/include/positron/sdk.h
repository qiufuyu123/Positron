#pragma once
//
// positron — Windows-only Electron / Chromium process injection SDK.
//
// Usage:
//
//   #include <positron/sdk.h>
//
//   for (auto& p : positron::sdk::list_processes()) {
//       std::wcout << p.pid << L" " << p.exe_name << L"\n";
//   }
//
//   positron::sdk::Session s;
//   if (auto err = s.attach(target_pid)) {
//       std::cerr << "attach failed: " << err->message << "\n"; return 1;
//   }
//   auto r = s.eval("process.versions.electron");
//   if (r.ok) std::cout << r.json_value << "\n";
//   s.detach();
//
// Link against:    positron_sdk.lib  +  BlackBone.lib  +  dbghelp.lib  +  Ws2_32.lib
// Include path:    sdk/include
// Library path:    third_party/Blackbone/lib/$(Platform)/$(Configuration)
//
// Build SDK and your consumer with the same /MT or /MD setting and the same
// architecture (Win32 or x64) as the target process.
//
#include <positron/process.h>
#include <positron/session.h>
