#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <chrono>
#include <exception>
#include <stdio.h>
#include <thread>
#include <windows.h>
#include <commctrl.h>
#define _RICHEDIT_VER 0x0200
#include <richedit.h>
#include <cstdio>
#include <string>
#include <TlHelp32.h>
#include <format>
#include <functional>
#include <MinHook.h>
#include <wingdi.h>
#include <winuser.h>
#pragma comment(lib, "comctl32.lib")


#define ID_OUTPUT 101
#define ID_INPUT  102
#define ID_SEND   103

extern "C" __declspec(dllexport)
void __stdcall HookEntry(DWORD unused);

using ColorRef = uint32_t;
constexpr ColorRef rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<ColorRef>(r | (g << 8) | (b << 16));
}

class InputQueue {
public:
	using functionType = std::function<void(std::string&)>;
	void Read() {
		DWORD size, count;
		while (GetMailslotInfo(hQueue, nullptr, &size, &count, nullptr)) {
			if (size == MAILSLOT_NO_MESSAGE || count == 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}
			std::string name_buffer;
			DWORD read = 0;
			name_buffer.resize(size);
			
			if (ReadFile(hQueue, const_cast<char*>(name_buffer.data()), size, &read, nullptr) && read) {
				fn(name_buffer);
			}
		}
	}
	~InputQueue(){
		CloseHandle(hQueue);

	}

	void Start(std::string_view queuePath, functionType _fn) {
		fn = _fn;
		SECURITY_DESCRIPTOR sd;
		InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
		SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
		SECURITY_ATTRIBUTES sa{ sizeof(sa), &sd };
		hQueue = CreateMailslot(queuePath.data(), 0, MAILSLOT_WAIT_FOREVER, &sa);
		if (hQueue == INVALID_HANDLE_VALUE) {
			throw std::exception("can't create queue");
			return;
		}
		worker = std::thread([this]{this->Read();});
		worker.detach();
		return;
	}
	std::thread worker;
	HANDLE hQueue;
	functionType fn;
};


#pragma pack(push,1)

struct thread_entry_code{
	std::uint8_t code1[7] = {0x55, 0x89, 0xE5, 0x8B, 0x4D, 0x08, 0xB8}; // what the fuck is this abi? fastcall?
	std::uint32_t cbuf_execute_address;
	std::uint8_t code2[6] = {0xFF, 0xD0, 0x5D, 0xC2, 0x04, 0x00 };
};

#pragma pack(pop)

#include <windows.h>
#include <string>

HMODULE GetCurrentDLLBase() {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)GetCurrentDLLBase, &mbi, sizeof(mbi))) {
        return (HMODULE)mbi.AllocationBase;
    }
    return NULL;
}

bool InjectDll(HANDLE hProcess, const std::string& dllPath)
{
    void* remoteMemory = VirtualAllocEx(hProcess, nullptr, dllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMemory)
        return false;

    if (!WriteProcessMemory(hProcess, remoteMemory, dllPath.c_str(), dllPath.size() + 1, nullptr))
    {
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32)
    {
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    FARPROC loadLibraryAddr = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!loadLibraryAddr)
    {
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    HANDLE remoteThread = CreateRemoteThread(
        hProcess,
        nullptr,
        0,
        (LPTHREAD_START_ROUTINE)loadLibraryAddr,
        remoteMemory,
        0,
        nullptr
    );

    if (!remoteThread)
    {
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(remoteThread, INFINITE);

    DWORD_PTR dllBase = 0;
    if (!GetExitCodeThread(remoteThread, (DWORD*)&dllBase))
    {
        CloseHandle(remoteThread);
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        return false;
    }

	auto rva = (uintptr_t)&HookEntry - (uintptr_t)GetCurrentDLLBase();
	auto tocall = (uintptr_t)dllBase + rva;


    HANDLE remoteThread2 = CreateRemoteThread(
        hProcess,
        nullptr,
        0,
        (LPTHREAD_START_ROUTINE)tocall,
        remoteMemory,
        0,
        nullptr
    );

    if (!remoteThread2)
    {
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(remoteThread2, INFINITE);

    CloseHandle(remoteThread);
	CloseHandle(remoteThread2);

    VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);

    return true;
}


void WriteColor(std::string& str, COLORREF color){
    char buffer[4];
    std::memcpy(buffer, &color, 4);
    str.append(buffer, 4);
}

std::string GetDllPath()
{
    char path[MAX_PATH] = {0};
    HMODULE hModule = NULL;

    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&GetDllPath,
            &hModule))
    {
        return "";
    }

    DWORD size = GetModuleFileNameA(hModule, path, MAX_PATH);
    if (size == 0 || size == MAX_PATH)
        return "";

    return std::string(path);
}
bool InjectSelfIntoProcess(HANDLE hProcess)
{
    std::string dllPath = GetDllPath();
    if (dllPath.empty())
        return false;

    return InjectDll(hProcess, dllPath);
}
class MainApp {
public:
	MainApp() {};
	static LRESULT CALLBACK s_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MainApp* pThis = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* pcs = (CREATESTRUCT*)lParam;
            pThis = (MainApp*)pcs->lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
        }
        else {
            pThis = (MainApp*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        }
        if (pThis) {
            return pThis->WndProc(hwnd, msg, wParam, lParam);
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		switch (msg) {
			case WM_CREATE: {
				RECT rc;
				GetClientRect(hwnd, &rc);
				int width = rc.right;
				int height = rc.bottom;

				//LoadLibraryA("Msftedit.dll");
				LoadLibraryA("Riched20.dll");
				hOutput = CreateWindowExA(WS_EX_CLIENTEDGE, "RichEdit20A", "",
					WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
					10, 10, width - 20, height - 90,
					hwnd, (HMENU)ID_OUTPUT, GetModuleHandle(NULL), NULL);

				hInput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", NULL,
					WS_CHILD | WS_VISIBLE | ES_LEFT,
					10, height - 60, width - 100, 25,
					hwnd, (HMENU)ID_INPUT, NULL, NULL);

				hSend = CreateWindowA("BUTTON", "Send",
					WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
					width - 80, height - 60, 70, 25,
					hwnd, (HMENU)ID_SEND, NULL, NULL);
				break;
			}

			case WM_COMMAND:
				if (LOWORD(wParam) == ID_SEND) HandleSend();
				break;

			case WM_SIZE: {
				int width = LOWORD(lParam);
				int height = HIWORD(lParam);

				MoveWindow(hOutput, 10, 10, width - 20, height - 90, TRUE);
				MoveWindow(hInput, 10, height - 60, width - 100, 25, TRUE);
				MoveWindow(hSend, width - 80, height - 60, 70, 25, TRUE);
				break;
			}

			case WM_DESTROY:
				PostQuitMessage(0);
				break;

			default:
				return DefWindowProc(hwnd, msg, wParam, lParam);
		}
		return 0;
	};
	void AppendColoredText(const char* text, COLORREF color) {
		int len = GetWindowTextLengthA(hOutput);
		SendMessageA(hOutput, EM_SETSEL, len, len);

		CHARFORMAT2 cf;
		ZeroMemory(&cf, sizeof(cf));
		cf.cbSize = sizeof(CHARFORMAT2);
		cf.dwMask = CFM_COLOR;
		cf.crTextColor = color;

		SendMessage(hOutput, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

		SendMessageA(hOutput, EM_REPLACESEL, FALSE, (LPARAM)text);
		SendMessageA(hOutput, EM_SCROLL, SB_BOTTOM, 0);
	}

	void Watchdog(){
		DWORD waitResult = WaitForSingleObject(hGameProcess, INFINITE);
		if (waitResult == WAIT_OBJECT_0) {
			TerminateProcess(GetCurrentProcess(), 0);
			ExitProcess(0);
   		}
	}

	void HandleSend() {
		int len = GetWindowTextLengthA(MainApp::hInput);
		if (len <= 0) {
			return;
		}

		std::string buffer(len, '\0');

		GetWindowTextA(hInput, &buffer[0], len + 1);

		AppendColoredText("> ", RGB(0, 0, 0));
		AppendColoredText(buffer.c_str(), RGB(0, 0, 0));
		AppendColoredText("\r\n", RGB(0, 0, 0));

		SetWindowTextA(hInput, "");

		if (!attached) {
			return;
		}

		buffer += '\n';
		if (buffer.length() > usableMemory){
			AppendColoredText("s.length() > usableMemory, wtf r u trying to execute?", RGB(255,20,147));
			return;
		}
		SIZE_T _;
		WriteProcessMemory(hGameProcess, remotePage, buffer.c_str(), buffer.length()+1, &_); // write the command, must end with \n\0
		HANDLE remote_thread = CreateRemoteThread(hGameProcess, NULL, NULL, reinterpret_cast<LPTHREAD_START_ROUTINE>(threadEntry), remotePage, NULL, NULL);
		//WaitForSingleObject(remote_thread, INFINITE);
	}

	void AttachThread(){
		DWORD pid = 0xffffffff;
		while (!attached){
			//AppendColoredText("looping", RGB(0,0,0));
			PROCESSENTRY32 currentProcess;
			HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			currentProcess.dwSize = sizeof(PROCESSENTRY32);
			if (!Process32First(hSnapshot, &currentProcess)){
				continue;
			}
			do {
				if (std::strcmp("cxbxr-ldr.exe", currentProcess.szExeFile) == 0){
					pid = currentProcess.th32ProcessID;
					break;
				}
			} while (Process32Next(hSnapshot, &currentProcess));
			if (pid == 0xffffffff){
				continue;
			} else {
				attached = true;
				CloseHandle(hSnapshot);
				continue;
			}
			CloseHandle(hSnapshot);
			using namespace std::chrono_literals;
			std::this_thread::sleep_for(1s);
		}
		AppendColoredText(std::format("Found game process pid {}\r\n", pid).c_str(), RGB(255,20,147));
		hGameProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
		if (!hGameProcess){
			throw std::exception("couldn't get a handle to the game process");
		}
		remotePage = VirtualAllocEx(hGameProcess, NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); // allocating 0x1000 bytes which should be a full page, can't get less anyways (i think so?)
		if (!remotePage){
			CloseHandle(hGameProcess);
			throw std::exception( "failed to allocate a page in the cxbxr process!");
		}
		usableMemory = 0x1000-sizeof(thread_entry_code);
		threadEntry = reinterpret_cast<std::uintptr_t>(remotePage) + (0x1000-sizeof(thread_entry_code));
		thread_entry_code lol;
		lol.cbuf_execute_address = 0x35DC90;
		AppendColoredText(std::format("threadEntry: {:x}\r\n", threadEntry).c_str(), RGB(255,20,147));
		SIZE_T _;
		WriteProcessMemory(hGameProcess, reinterpret_cast<LPVOID>(threadEntry), &lol, sizeof(lol), &_); // writing the thread entry point code at the end of our allocated page	

		queue.Start("\\\\.\\mailslot\\hl2xconsole_input", std::bind(&MainApp::OnMessage, this, std::placeholders::_1));
		if (!InjectSelfIntoProcess(hGameProcess)){
			throw std::exception("!InjectSelfIntoProcess");
		}
		std::thread watchdog([this]{this->Watchdog();});
		watchdog.detach();
	}
	void OnMessage(std::string& fn){
		std::string color;
		color = fn.substr(0,8);
		std::string str = fn.substr(8);

    	ColorRef value = 0;
		auto ec = std::from_chars(color.data(), color.data() + color.size(), value, 16);
		if (!str.empty() && str.back() == '\n'){
			str.back() = '\r';
			str += '\n';
		} else {
			str += "\r\n";
		}
		AppendColoredText(str.c_str(), value);
	}

	void Run(HINSTANCE hInstance, int nCmdShow) {

		HANDLE hMutex = CreateMutexA(NULL, TRUE, "hl2xconsole_mutex");

		if (GetLastError() == ERROR_ALREADY_EXISTS) {
			return;
		}

		INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
		InitCommonControlsEx(&icex);


		WNDCLASSA wc = {};
		wc.lpfnWndProc = MainApp::s_WndProc;
		wc.hInstance = hInstance;
		wc.lpszClassName = MainApp::CLASS_NAME;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

		RegisterClassA(&wc);

		HWND hwnd = CreateWindowExA(
			0, MainApp::CLASS_NAME, "hl2x console",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, 600, 400,
			NULL, NULL, hInstance, this
		);


		ShowWindow(hwnd, nCmdShow);
		UpdateWindow(hwnd);

		static std::thread t([this] {this->AttachThread();});
		t.detach();

		MSG msg = {};
		while (GetMessage(&msg, NULL, 0, 0)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		CloseHandle(hMutex);
		CloseHandle(hGameProcess);
	}
	std::uintptr_t threadEntry = 0;
	std::uintptr_t usableMemory = 0;
	LPVOID remotePage = nullptr;
	HANDLE hGameProcess = INVALID_HANDLE_VALUE;
	InputQueue queue;
	bool attached = false;
	HWND hOutput, hInput, hSend = nullptr;
	static inline const char CLASS_NAME[] = "BnnuyWndClass";
};


extern "C" __declspec(dllexport)
void Run(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
	MainApp app;
    app.Run(hinst, nCmdShow);
}


static HANDLE sendHandle;


void SendToConsole(const char* channel, std::string& content, ColorRef color){
	auto finalstr = std::format("{:08x}[{}] {}",color,channel,content);
	DWORD written;
	WriteFile(sendHandle, finalstr.c_str(), finalstr.length(), &written, nullptr);
}


std::string vstring_format(const char* format, va_list args) {
    // Copy va_list because vsnprintf may modify it
    va_list args_copy;
    va_copy(args_copy, args);

    // Get the required size
    int size = std::vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (size < 0) {
        throw std::runtime_error("Encoding error during formatting.");
    }

    std::string result(size, '\0'); // allocate string with needed size

    // Now actually format into string
    std::vsnprintf(&result[0], size + 1, format, args);

    return result;
}


void ConMsg(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    auto str = vstring_format(fmt, args);
    va_end(args);
	SendToConsole(__func__, str, rgb(127, 85, 177));
}

void ConDMsg(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    auto str = vstring_format(fmt, args);
    va_end(args);
	SendToConsole(__func__, str, rgb(155, 126, 189));
}

void COM_TimestampedLog(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    auto str = vstring_format(fmt, args);
    va_end(args);
	SendToConsole(__func__, str, rgb(244, 155, 171));
}

using Host_ErrorFn = void(*)(const char* fmt, ...);
Host_ErrorFn orig_HostError = nullptr;
void Host_Error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    auto str = vstring_format(fmt, args);
	SendToConsole(__func__, str, rgb(255,116,108));
	orig_HostError(fmt, args);
    va_end(args);
}

Host_ErrorFn orig_Sys_Error = nullptr;
void Sys_Error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    auto str = vstring_format(fmt, args);
	SendToConsole(__func__, str, rgb(255,0,0));
	orig_Sys_Error(fmt, args);
    va_end(args);
}

void __stdcall HookEntry(DWORD unused) {

	if (MH_Initialize()!= MH_OK){
		MessageBoxA(nullptr, "MH_Initialize()!= MH_OK", "MH_Initialize()!= MH_OK", 0);
	}
	sendHandle = CreateFile("\\\\.\\mailslot\\hl2xconsole_input", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (sendHandle == INVALID_HANDLE_VALUE) {
		MessageBoxA(nullptr, "no mailslot?", "no mailslot?", 0);
		return;
	}
	
	MH_CreateHook((void*)0x00365BE0, (void*)&COM_TimestampedLog, NULL);
	MH_CreateHook((void*)0x003663E0, (void*)&ConMsg, NULL);
	MH_CreateHook((void*)0x00366460, (void*)&ConDMsg, NULL);
	MH_CreateHook((void*)0x0039F340, (void*)&Host_Error, (void**)&orig_HostError);
	MH_CreateHook((void*)0x0040E1A0, (void*)&Sys_Error, (void**)&orig_Sys_Error);

	if (MH_EnableHook(MH_ALL_HOOKS) !=MH_OK){
		MessageBoxA(nullptr, "MH_EnableHook(MH_ALL_HOOKS) !=MH_OK", "MH_EnableHook(MH_ALL_HOOKS) !=MH_OK", 0);
	}
}
