#include "UI/Instance.hpp"

#include <windows.h>

#include <chrono>
#include <thread>

namespace Tori::UI {
    namespace {
        // Per user session; the GUID keeps it from colliding with anything else called "Tori"
        constexpr wchar_t kMutexName[]   = L"Local\\Tori-6F1C2B7E-4D0A-4E57-9C1B-3A8E5D2F9B60";
        constexpr wchar_t kWindowClass[] = L"Tori.InstanceChannel";
        constexpr ULONG_PTR kMessageTag  = 0x546F7269;  // "Tori"
    }  // namespace

    struct InstanceWindow {
        static LRESULT CALLBACK Proc(const HWND Window, const UINT Message, const WPARAM WParam, const LPARAM LParam) {
            if (Message == WM_COPYDATA) {
                auto* Channel     = reinterpret_cast<InstanceChannel*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
                const auto* Copy  = reinterpret_cast<const COPYDATASTRUCT*>(LParam);
                if (!Channel || !Channel->_OnArguments || Copy->dwData != kMessageTag || Copy->cbData % sizeof(wchar_t) != 0) {
                    return FALSE;
                }

                // Arguments separated by NULs
                std::vector<std::wstring> Arguments;
                const auto* Text   = static_cast<const wchar_t*>(Copy->lpData);
                const std::size_t Length = Copy->cbData / sizeof(wchar_t);
                std::wstring Current;
                for (std::size_t I = 0; I < Length; ++I) {
                    if (Text[I] != L'\0') {
                        Current += Text[I];
                    } else if (!Current.empty()) {
                        Arguments.push_back(std::exchange(Current, {}));
                    }
                }
                if (!Current.empty()) { Arguments.push_back(std::move(Current)); }
                Channel->_OnArguments(std::move(Arguments));
                return TRUE;
            }
            return DefWindowProcW(Window, Message, WParam, LParam);
        }
    };

    InstanceChannel::InstanceChannel() {
        _Mutex   = CreateMutexW(nullptr, FALSE, kMutexName);
        _IsFirst = !(_Mutex && GetLastError() == ERROR_ALREADY_EXISTS);
    }

    InstanceChannel::~InstanceChannel() {
        if (_Window) { DestroyWindow(static_cast<HWND>(_Window)); }
        if (_Mutex) { CloseHandle(_Mutex); }
    }

    bool InstanceChannel::SendToFirst(const std::vector<std::wstring>& Arguments) {
        std::wstring Payload;
        for (const std::wstring& Argument : Arguments) {
            Payload += Argument;
            Payload += L'\0';
        }

        // The first copy may still be starting up, before its window exists
        for (int Attempt = 0; Attempt < 50; ++Attempt) {
            if (const HWND Target = FindWindowExW(HWND_MESSAGE, nullptr, kWindowClass, nullptr)) {
                // Let it take the foreground, which Windows otherwise only allows the app the user is using
                DWORD Process = 0;
                GetWindowThreadProcessId(Target, &Process);
                AllowSetForegroundWindow(Process);

                COPYDATASTRUCT Copy {kMessageTag, static_cast<DWORD>(Payload.size() * sizeof(wchar_t)), Payload.data()};
                DWORD_PTR Result = 0;
                return SendMessageTimeoutW(Target,
                                           WM_COPYDATA,
                                           0,
                                           reinterpret_cast<LPARAM>(&Copy),
                                           SMTO_ABORTIFHUNG,
                                           5000,
                                           &Result) != 0 &&
                       Result == TRUE;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    void InstanceChannel::Listen(std::function<void(std::vector<std::wstring>)> OnArguments) {
        _OnArguments = std::move(OnArguments);
        if (_Window) { return; }

        const HINSTANCE Module = GetModuleHandleW(nullptr);
        WNDCLASSEXW Class {};
        Class.cbSize        = sizeof(Class);
        Class.lpfnWndProc   = InstanceWindow::Proc;
        Class.hInstance     = Module;
        Class.lpszClassName = kWindowClass;
        RegisterClassExW(&Class);

        // Message-only: never shown, but findable by class name
        const HWND Window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, Module, nullptr);
        if (!Window) { return; }
        SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        _Window = Window;
    }
}  // namespace Tori::UI
