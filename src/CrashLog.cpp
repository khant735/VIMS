#include "CrashLog.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <exception>
#include <cstdlib>
namespace { std::mutex gmu; std::ofstream gout; std::filesystem::path marker;
std::filesystem::path exeDir(){wchar_t p[MAX_PATH]{};GetModuleFileNameW(nullptr,p,MAX_PATH);return std::filesystem::path(p).parent_path();}
std::string stamp(){SYSTEMTIME t{};GetLocalTime(&t);char b[64];snprintf(b,sizeof(b),"%04u-%02u-%02u %02u:%02u:%02u.%03u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);return b;}
std::string crashName(){SYSTEMTIME t{};GetLocalTime(&t);char b[96];snprintf(b,sizeof(b),"VulkanImageMaskStudio_crash_%04u-%02u-%02u_%02u-%02u-%02u.log",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);return b;}
void stack(){void* a[48]{};USHORT n=CaptureStackBackTrace(0,48,a,nullptr);for(USHORT i=0;i<n;i++){std::ostringstream s;s<<"  stack["<<i<<"] = 0x"<<std::hex<<(uintptr_t)a[i];CrashLog::write(s.str());}}
}
void CrashLog::initialize(){auto d=exeDir()/L"Logs";std::error_code ec;std::filesystem::create_directories(d,ec);marker=d/L"running.marker"; bool dirty=std::filesystem::exists(marker);gout.open(d/L"latest.log",std::ios::out|std::ios::trunc);write("Vulkan Image Mask Studio 0.4.11.2 log started");write(std::string("Previous session unclean: ")+(dirty?"YES":"NO"));std::ofstream(marker)<<"running";SetUnhandledExceptionFilter(unhandled);std::set_terminate(terminateHandler);}
void CrashLog::shutdownClean(){write("Clean shutdown");std::error_code ec;std::filesystem::remove(marker,ec);std::lock_guard<std::mutex> l(gmu);gout.flush();gout.close();}
void CrashLog::write(const std::string& x){std::lock_guard<std::mutex> l(gmu);if(gout){gout<<"["<<stamp()<<"] "<<x<<"\n";gout.flush();}}
void CrashLog::writeWide(const std::wstring& w){if(w.empty()){write("");return;}int n=WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),nullptr,0,nullptr,nullptr);std::string s(n,0);WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),s.data(),n,nullptr,nullptr);write(s);}
LONG WINAPI CrashLog::unhandled(EXCEPTION_POINTERS* ep){DWORD code=ep&&ep->ExceptionRecord?ep->ExceptionRecord->ExceptionCode:0;void* addr=ep&&ep->ExceptionRecord?ep->ExceptionRecord->ExceptionAddress:nullptr;std::ostringstream s;s<<"UNHANDLED SEH exception code=0x"<<std::hex<<code<<" address=0x"<<(uintptr_t)addr;write(s.str());stack();try{auto src=exeDir()/L"Logs"/L"latest.log";auto dst=exeDir()/L"Logs"/std::filesystem::path(crashName());std::lock_guard<std::mutex> l(gmu);gout.flush();std::error_code ec;std::filesystem::copy_file(src,dst,std::filesystem::copy_options::overwrite_existing,ec);}catch(...){}return EXCEPTION_EXECUTE_HANDLER;}
[[noreturn]] void CrashLog::terminateHandler(){write("std::terminate invoked");try{auto e=std::current_exception();if(e)std::rethrow_exception(e);}catch(const std::exception& e){write(std::string("C++ exception: ")+e.what());}catch(...){write("Unknown C++ exception");}stack();
try{auto src=exeDir()/L"Logs"/L"latest.log";auto dst=exeDir()/L"Logs"/std::filesystem::path(crashName());{std::lock_guard<std::mutex> l(gmu);gout.flush();}std::error_code ec;std::filesystem::copy_file(src,dst,std::filesystem::copy_options::overwrite_existing,ec);}catch(...){}
std::abort();}
