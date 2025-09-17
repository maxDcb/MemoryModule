#pragma once

#include <windows.h>

#include <cstddef>
#include <memory>

class MemoryModule {
public:
    using ResourceHandle = const void*;
    using CustomModuleHandle = void*;

    using CustomAllocFunc = LPVOID (*)(LPVOID, SIZE_T, DWORD, DWORD, void*);
    using CustomFreeFunc = BOOL (*)(LPVOID, SIZE_T, DWORD, void*);
    using CustomLoadLibraryFunc = CustomModuleHandle (*)(LPCSTR, void*);
    using CustomGetProcAddressFunc = FARPROC (*)(CustomModuleHandle, LPCSTR, void*);
    using CustomFreeLibraryFunc = void (*)(CustomModuleHandle, void*);

    MemoryModule() = default;
    ~MemoryModule();

    MemoryModule(const MemoryModule&) = delete;
    MemoryModule& operator=(const MemoryModule&) = delete;

    MemoryModule(MemoryModule&& other) noexcept;
    MemoryModule& operator=(MemoryModule&& other) noexcept;

    static MemoryModule load(const void* data, size_t size);
    static MemoryModule load(const void* data, size_t size,
                             CustomAllocFunc alloc,
                             CustomFreeFunc free,
                             CustomLoadLibraryFunc loadLibrary,
                             CustomGetProcAddressFunc getProcAddress,
                             CustomFreeLibraryFunc freeLibrary,
                             void* userData);

    [[nodiscard]] bool isLoaded() const noexcept { return static_cast<bool>(impl_); }
    explicit operator bool() const noexcept { return isLoaded(); }

    FARPROC getProcAddress(LPCSTR name) const;
    int callEntryPoint();
    ResourceHandle findResource(LPCTSTR name, LPCTSTR type) const;
    ResourceHandle findResourceEx(LPCTSTR name, LPCTSTR type, WORD language) const;
    DWORD sizeofResource(ResourceHandle resource) const;
    LPVOID loadResource(ResourceHandle resource) const;
    int loadString(UINT id, LPTSTR buffer, int maxsize) const;
    int loadStringEx(UINT id, LPTSTR buffer, int maxsize, WORD language) const;

    void reset() noexcept;

    static LPVOID defaultAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect, void* userData);
    static BOOL defaultFree(LPVOID address, SIZE_T size, DWORD freeType, void* userData);
    static CustomModuleHandle defaultLoadLibrary(LPCSTR filename, void* userData);
    static FARPROC defaultGetProcAddress(CustomModuleHandle module, LPCSTR name, void* userData);
    static void defaultFreeLibrary(CustomModuleHandle module, void* userData);

private:
    struct Impl;
    struct ImplDeleter {
        void operator()(Impl* impl) const noexcept;
    };

    explicit MemoryModule(Impl* impl) noexcept;

    static Impl* loadInternal(const void* data, size_t size,
                              CustomAllocFunc alloc,
                              CustomFreeFunc free,
                              CustomLoadLibraryFunc loadLibrary,
                              CustomGetProcAddressFunc getProcAddress,
                              CustomFreeLibraryFunc freeLibrary,
                              void* userData);

    std::unique_ptr<Impl, ImplDeleter> impl_{};
};

