// Copyright 2026 Jake Rieger
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "UI/CrashLog.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <crtdbg.h>

#include <cstdio>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>

namespace Tori::UI {
    namespace {
        std::wstring LogFile;
        std::mutex LogMutex;

        void WriteReport(const std::string& Reason) {
            std::scoped_lock Lock(LogMutex);
            FILE* Out = nullptr;
            if (_wfopen_s(&Out, LogFile.c_str(), L"a") != 0 || !Out) { return; }

            const std::time_t Now = std::time(nullptr);
            char When[64] {};
            std::tm Local {};
            localtime_s(&Local, &Now);
            std::strftime(When, sizeof(When), "%Y-%m-%d %H:%M:%S", &Local);
            std::fprintf(Out, "==== %s, thread %lu: %s\n", When, GetCurrentThreadId(), Reason.c_str());

            const HANDLE Process = GetCurrentProcess();
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(Process, nullptr, TRUE);

            void* Frames[64] {};
            const USHORT Count = RtlCaptureStackBackTrace(0, 64, Frames, nullptr);
            alignas(SYMBOL_INFO) char Buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] {};
            auto* Symbol = reinterpret_cast<SYMBOL_INFO*>(Buffer);
            for (USHORT I = 0; I < Count; ++I) {
                const auto Address   = reinterpret_cast<DWORD64>(Frames[I]);
                Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                Symbol->MaxNameLen   = MAX_SYM_NAME;
                DWORD64 Offset       = 0;
                const bool Named     = SymFromAddr(Process, Address, &Offset, Symbol);

                IMAGEHLP_LINE64 Line {};
                Line.SizeOfStruct  = sizeof(Line);
                DWORD Column       = 0;
                const bool HasLine = SymGetLineFromAddr64(Process, Address, &Column, &Line);
                std::fprintf(Out,
                             "  %2u %s%s%s:%lu\n",
                             I,
                             Named ? Symbol->Name : "?",
                             HasLine ? "  " : "",
                             HasLine ? Line.FileName : "",
                             HasLine ? Line.LineNumber : 0);
            }
            SymCleanup(Process);
            std::fclose(Out);
        }

        LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* Info) {
            char Reason[96] {};
            std::snprintf(Reason,
                          sizeof(Reason),
                          "exception 0x%08lX at %p",
                          Info->ExceptionRecord->ExceptionCode,
                          Info->ExceptionRecord->ExceptionAddress);
            WriteReport(Reason);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        void OnTerminate() {
            std::string Reason = "std::terminate";
            if (const std::exception_ptr Current = std::current_exception()) {
                try {
                    std::rethrow_exception(Current);
                } catch (const std::exception& Ex) {
                    Reason += std::string(": uncaught exception: ") + Ex.what();
                } catch (...) { Reason += ": uncaught non-standard exception"; }
            }
            WriteReport(Reason);
            std::abort();
        }

        int OnCrtReport(const int Type, char* Message, int* ReturnValue) {
            if (Type == _CRT_WARN) { return FALSE; }
            WriteReport(std::string(Type == _CRT_ASSERT ? "assertion: " : "CRT error: ") + (Message ? Message : ""));
            *ReturnValue = 0;
            return FALSE;  // Carry on to the usual report (the debug break dialog)
        }
    }  // namespace

    void InstallCrashLog(const std::filesystem::path& File) {
        if (File.empty()) { return; }
        std::error_code Ec;
        std::filesystem::create_directories(File.parent_path(), Ec);
        LogFile = File.wstring();
        SetUnhandledExceptionFilter(OnUnhandledException);
        std::set_terminate(OnTerminate);
        _CrtSetReportHook(OnCrtReport);
    }
}  // namespace Tori::UI
