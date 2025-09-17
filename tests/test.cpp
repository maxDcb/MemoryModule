#include <windows.h>
#include <iostream>
#include <fstream>
#include <vector>

#include "../MemoryModule.hpp"  

typedef void (*HelloFunc)();

int main() 
{
    // Read the DLL from disk into a buffer (just for demo)

    std::ifstream file("TestDll.dll", std::ios::binary);
    std::vector<char> buffer((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

    if (buffer.empty()) 
    {
        std::cerr << "Failed to read DLL file\n";
        return 1;
    }

    HMEMORYMODULE mod = MemoryLoadLibrary(buffer.data(), buffer.size());
    if (!mod) 
    {
        std::cerr << "MemoryLoadLibrary failed\n";
        return 1;
    }

    HelloFunc hello = (HelloFunc)MemoryGetProcAddress(mod, "HelloFromDll");
    if (hello)
        hello();
    else
        std::cerr << "Could not find HelloFromDll\n";

    MemoryFreeLibrary(mod);
    return 0;
}
