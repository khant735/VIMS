#pragma once
#include <windows.h>
#include <string>
namespace CrashLog {
void initialize();
void shutdownClean();
void write(const std::string& line);
void writeWide(const std::wstring& line);
LONG WINAPI unhandled(EXCEPTION_POINTERS* ep);
[[noreturn]] void terminateHandler();
}
