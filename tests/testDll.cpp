#include <windows.h>
#include <iostream>

extern "C" __declspec(dllexport)
void HelloFromDll()
{
    std::cout << "Hello from inside the DLL!" << std::endl;

    MessageBox( NULL, "Hello from go", "Hi!", MB_OK );

    return;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) 
    {
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
