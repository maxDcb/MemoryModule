#include <windows.h>
#include <iostream>
#include <fstream>
#include <vector>

#include "../MemoryModule.hpp"  

typedef BOOL (*HelloFunc)();

int main() 
{
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
        if (!hello)
        {
            std::cerr << "Could not find HelloFromDll\n";
            MemoryFreeLibrary(mod);
            return 1;
        }

        if (!hello())
        {
            std::cerr << "HelloFromDll returned failure\n";
            MemoryFreeLibrary(mod);
            return 1;
        }

        MemoryFreeLibrary(mod);
    }

    {
        // Read the EXE from disk into a buffer (just for demo)

        std::ifstream file("TestExe.exe", std::ios::binary);
        std::vector<char> buffer((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());

        if (buffer.empty()) 
        {
            std::cerr << "Failed to read EXE file\n";
            return 1;
        }

        HMEMORYMODULE mod = MemoryLoadLibrary(buffer.data(), buffer.size());
        if (!mod) 
        {
            std::cerr << "MemoryLoadLibrary failed\n";
            return 1;
        }        

        SetEnvironmentVariableA("MEMORYMODULE_TEST_EXE_RAN", NULL);
        if (MemoryCallEntryPoint(mod) != 0)
        {
            std::cerr << "MemoryCallEntryPoint failed\n";
            MemoryFreeLibrary(mod);
            return 1;
        }

        char envValue[8] = {};
        DWORD envLen = GetEnvironmentVariableA("MEMORYMODULE_TEST_EXE_RAN", envValue, sizeof(envValue));
        if (envLen == 0 || strcmp(envValue, "true") != 0)
        {
            std::cerr << "TestExe did not mark execution success\n";
            MemoryFreeLibrary(mod);
            return 1;
        }

        MemoryFreeLibrary(mod);
    }

    std::cout << "Finished OK" << std::endl;
    return 0;
}
