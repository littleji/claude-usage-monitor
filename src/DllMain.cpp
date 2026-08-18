#include "UsageService.h"

#include <windows.h>

/*
 * 插件里有一个常驻的取数线程。如果主程序在线程运行期间 FreeLibrary 掉本 DLL，
 * 线程会执行到已经被卸载的代码上，直接崩溃。
 *
 * 这里在加载时把模块 PIN 住（引用计数永不归零），DLL 只会随进程一起退出，
 * 从而避免在 DllMain 里等待线程结束（那样会持有加载器锁，容易死锁）。
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        ::DisableThreadLibraryCalls(hModule);

        HMODULE pinned = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        break;
    }
    default:
        break;
    }
    return TRUE;
}
