#include <windows.h>
#include <iostream>

extern "C" __declspec(dllexport)
BOOL HelloFromDll()
{
    std::cout << "Hello from inside the DLL!" << std::endl;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) 
    {
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
