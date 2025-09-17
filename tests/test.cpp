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

    MemoryModule module = MemoryModule::load(buffer.data(), buffer.size());
    if (!module)
    {
        std::cerr << "MemoryLoadLibrary failed\n";
        return 1;
    }

    HelloFunc hello = reinterpret_cast<HelloFunc>(module.getProcAddress("HelloFromDll"));
    if (hello)
        hello();
    else
        std::cerr << "Could not find HelloFromDll\n";

    return 0;
}
