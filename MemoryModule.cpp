/*
 * Memory DLL loading code
 * Version 0.0.4
 *
 * Copyright (c) 2004-2015 by Joachim Bauch / mail@joachim-bauch.de
 * http://www.joachim-bauch.de
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 2.0 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is MemoryModule.c
 *
 * The Initial Developer of the Original Code is Joachim Bauch.
 *
 * Portions created by Joachim Bauch are Copyright (C) 2004-2015
 * Joachim Bauch. All Rights Reserved.
 *
 *
 * THeller: Added binary search in MemoryGetProcAddress function
 * (#define USE_BINARY_SEARCH to enable it).  This gives a very large
 * speedup for libraries that exports lots of functions.
 *
 * These portions are Copyright (C) 2013 Thomas Heller.
 */

#include <windows.h>
#include <winnt.h>
#include <tchar.h>
#ifdef DEBUG_OUTPUT
#include <stdio.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#if _MSC_VER
// Disable warning about data -> function pointer conversion
#pragma warning(disable:4055)
 // C4244: conversion from 'uintptr_t' to 'DWORD', possible loss of data.
#pragma warning(error: 4244)
// C4267: conversion from 'size_t' to 'int', possible loss of data.
#pragma warning(error: 4267)

#define inline __inline
#endif

#ifndef IMAGE_SIZEOF_BASE_RELOCATION
// Vista SDKs no longer define IMAGE_SIZEOF_BASE_RELOCATION!?
#define IMAGE_SIZEOF_BASE_RELOCATION (sizeof(IMAGE_BASE_RELOCATION))
#endif

#ifdef _WIN64
#define HOST_MACHINE IMAGE_FILE_MACHINE_AMD64
#else
#define HOST_MACHINE IMAGE_FILE_MACHINE_I386
#endif

#include "MemoryModule.hpp"

struct ExportNameEntry {
    LPCSTR name;
    WORD idx;
};

using DllEntryProc = BOOL(WINAPI*)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);
using ExeEntryProc = int(WINAPI*)();

struct MemoryModule::Impl {
    PIMAGE_NT_HEADERS headers = nullptr;
    unsigned char* codeBase = nullptr;
    std::vector<MemoryModule::CustomModuleHandle> modules;
    bool initialized = false;
    bool isDLL = false;
    bool isRelocated = false;
    MemoryModule::CustomAllocFunc alloc = nullptr;
    MemoryModule::CustomFreeFunc free = nullptr;
    MemoryModule::CustomLoadLibraryFunc loadLibrary = nullptr;
    MemoryModule::CustomGetProcAddressFunc getProcAddress = nullptr;
    MemoryModule::CustomFreeLibraryFunc freeLibrary = nullptr;
    mutable std::vector<ExportNameEntry> nameExportsTable;
    mutable bool exportsSorted = false;
    void* userdata = nullptr;
    ExeEntryProc exeEntry = nullptr;
    DWORD pageSize = 0;
#ifdef _WIN64
    std::vector<void*> blockedMemory;
#endif
};

struct SectionFinalizeData {
    LPVOID address = nullptr;
    LPVOID alignedAddress = nullptr;
    SIZE_T size = 0;
    DWORD characteristics = 0;
    bool last = false;
};

#define GET_HEADER_DICTIONARY(module, idx)  &(module)->headers->OptionalHeader.DataDirectory[idx]

static inline uintptr_t
AlignValueDown(uintptr_t value, uintptr_t alignment) {
    return value & ~(alignment - 1);
}

static inline LPVOID
AlignAddressDown(LPVOID address, uintptr_t alignment) {
    return (LPVOID) AlignValueDown((uintptr_t) address, alignment);
}

static inline size_t
AlignValueUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline void*
OffsetPointer(void* data, ptrdiff_t offset) {
    return (void*) ((uintptr_t) data + offset);
}

static inline void
OutputLastError(const char *msg)
{
#ifndef DEBUG_OUTPUT
    UNREFERENCED_PARAMETER(msg);
#else
    LPVOID tmp;
    char *tmpmsg;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&tmp, 0, nullptr);
    tmpmsg = (char *)LocalAlloc(LPTR, strlen(msg) + strlen(tmp) + 3);
    sprintf(tmpmsg, "%s: %s", msg, tmp);
    OutputDebugString(tmpmsg);
    LocalFree(tmpmsg);
    LocalFree(tmp);
#endif
}

#ifdef _WIN64
static void FreeBlockedMemory(const std::vector<void*>& blockedMemory,
    MemoryModule::CustomFreeFunc freeMemory,
    void* userdata)
{
    for (void* address : blockedMemory) {
        freeMemory(address, 0, MEM_RELEASE, userdata);
    }
}
#endif

static BOOL
CheckSize(size_t size, size_t expected) {
    if (size < expected) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    return TRUE;
}

static BOOL
CopySections(const unsigned char* data, size_t size, PIMAGE_NT_HEADERS old_headers, MemoryModule::Impl* module)
{
    int i, section_size;
    unsigned char *codeBase = module->codeBase;
    unsigned char *dest;
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
    for (i=0; i<module->headers->FileHeader.NumberOfSections; i++, section++) {
        if (section->SizeOfRawData == 0) {
            // section doesn't contain data in the dll itself, but may define
            // uninitialized data
            section_size = old_headers->OptionalHeader.SectionAlignment;
            if (section_size > 0) {
                dest = (unsigned char *)module->alloc(codeBase + section->VirtualAddress,
                    section_size,
                    MEM_COMMIT,
                    PAGE_READWRITE,
                    module->userdata);
                if (dest == nullptr) {
                    return FALSE;
                }

                // Always use position from file to support alignments smaller
                // than page size (allocation above will align to page size).
                dest = codeBase + section->VirtualAddress;
                // NOTE: On 64bit systems we truncate to 32bit here but expand
                // again later when "PhysicalAddress" is used.
                section->Misc.PhysicalAddress = (DWORD) ((uintptr_t) dest & 0xffffffff);
                memset(dest, 0, section_size);
            }

            // section is empty
            continue;
        }

        if (!CheckSize(size, section->PointerToRawData + section->SizeOfRawData)) {
            return FALSE;
        }

        // commit memory block and copy data from dll
        dest = (unsigned char *)module->alloc(codeBase + section->VirtualAddress,
                            section->SizeOfRawData,
                            MEM_COMMIT,
                            PAGE_READWRITE,
                            module->userdata);
        if (dest == nullptr) {
            return FALSE;
        }

        // Always use position from file to support alignments smaller
        // than page size (allocation above will align to page size).
        dest = codeBase + section->VirtualAddress;
        memcpy(dest, data + section->PointerToRawData, section->SizeOfRawData);
        // NOTE: On 64bit systems we truncate to 32bit here but expand
        // again later when "PhysicalAddress" is used.
        section->Misc.PhysicalAddress = (DWORD) ((uintptr_t) dest & 0xffffffff);
    }

    return TRUE;
}

// Protection flags for memory pages (Executable, Readable, Writeable)
static int ProtectionFlags[2][2][2] = {
    {
        // not executable
        {PAGE_NOACCESS, PAGE_WRITECOPY},
        {PAGE_READONLY, PAGE_READWRITE},
    }, {
        // executable
        {PAGE_EXECUTE, PAGE_EXECUTE_WRITECOPY},
        {PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE},
    },
};

static SIZE_T
GetRealSectionSize(PMEMORYMODULE module, PIMAGE_SECTION_HEADER section) {
    DWORD size = section->SizeOfRawData;
    if (size == 0) {
        if (section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) {
            size = module->headers->OptionalHeader.SizeOfInitializedData;
        } else if (section->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) {
            size = module->headers->OptionalHeader.SizeOfUninitializedData;
        }
    }
    return (SIZE_T) size;
}

static BOOL
FinalizeSection(MemoryModule::Impl* module, SectionFinalizeData* sectionData) {
    DWORD protect, oldProtect;
    BOOL executable;
    BOOL readable;
    BOOL writeable;

    if (sectionData->size == 0) {
        return TRUE;
    }

    if (sectionData->characteristics & IMAGE_SCN_MEM_DISCARDABLE) {
        // section is not needed any more and can safely be freed
        if (sectionData->address == sectionData->alignedAddress &&
            (sectionData->last ||
             module->headers->OptionalHeader.SectionAlignment == module->pageSize ||
             (sectionData->size % module->pageSize) == 0)
           ) {
            // Only allowed to decommit whole pages
            module->free(sectionData->address, sectionData->size, MEM_DECOMMIT, module->userdata);
        }
        return TRUE;
    }

    // determine protection flags based on characteristics
    executable = (sectionData->characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    readable =   (sectionData->characteristics & IMAGE_SCN_MEM_READ) != 0;
    writeable =  (sectionData->characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    protect = ProtectionFlags[executable][readable][writeable];
    if (sectionData->characteristics & IMAGE_SCN_MEM_NOT_CACHED) {
        protect |= PAGE_NOCACHE;
    }

    // change memory access flags
    if (VirtualProtect(sectionData->address, sectionData->size, protect, &oldProtect) == 0) {
        OutputLastError("Error protecting memory page");
        return FALSE;
    }

    return TRUE;
}

static BOOL
FinalizeSections(MemoryModule::Impl* module)
{
    int i;
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
#ifdef _WIN64
    // "PhysicalAddress" might have been truncated to 32bit above, expand to
    // 64bits again.
    uintptr_t imageOffset = ((uintptr_t) module->headers->OptionalHeader.ImageBase & 0xffffffff00000000);
#else
    static const uintptr_t imageOffset = 0;
#endif
    SectionFinalizeData sectionData;
    sectionData.address = (LPVOID)((uintptr_t)section->Misc.PhysicalAddress | imageOffset);
    sectionData.alignedAddress = AlignAddressDown(sectionData.address, module->pageSize);
    sectionData.size = GetRealSectionSize(module, section);
    sectionData.characteristics = section->Characteristics;
    sectionData.last = false;
    section++;

    // loop through all sections and change access flags
    for (i=1; i<module->headers->FileHeader.NumberOfSections; i++, section++) {
        LPVOID sectionAddress = (LPVOID)((uintptr_t)section->Misc.PhysicalAddress | imageOffset);
        LPVOID alignedAddress = AlignAddressDown(sectionAddress, module->pageSize);
        SIZE_T sectionSize = GetRealSectionSize(module, section);
        // Combine access flags of all sections that share a page
        // TODO(fancycode): We currently share flags of a trailing large section
        //   with the page of a first small section. This should be optimized.
        if (sectionData.alignedAddress == alignedAddress || (uintptr_t) sectionData.address + sectionData.size > (uintptr_t) alignedAddress) {
            // Section shares page with previous
            if ((section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0 || (sectionData.characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0) {
                sectionData.characteristics = (sectionData.characteristics | section->Characteristics) & ~IMAGE_SCN_MEM_DISCARDABLE;
            } else {
                sectionData.characteristics |= section->Characteristics;
            }
            sectionData.size = (((uintptr_t)sectionAddress) + ((uintptr_t) sectionSize)) - (uintptr_t) sectionData.address;
            continue;
        }

        if (!FinalizeSection(module, &sectionData)) {
            return FALSE;
        }
        sectionData.address = sectionAddress;
        sectionData.alignedAddress = alignedAddress;
        sectionData.size = sectionSize;
        sectionData.characteristics = section->Characteristics;
    }
    sectionData.last = true;
    if (!FinalizeSection(module, &sectionData)) {
        return FALSE;
    }
    return TRUE;
}

static BOOL
ExecuteTLS(MemoryModule::Impl* module)
{
    unsigned char *codeBase = module->codeBase;
    PIMAGE_TLS_DIRECTORY tls;
    PIMAGE_TLS_CALLBACK* callback;

    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_TLS);
    if (directory->VirtualAddress == 0) {
        return TRUE;
    }

    tls = (PIMAGE_TLS_DIRECTORY) (codeBase + directory->VirtualAddress);
    callback = (PIMAGE_TLS_CALLBACK *) tls->AddressOfCallBacks;
    if (callback) {
        while (*callback) {
            (*callback)((LPVOID) codeBase, DLL_PROCESS_ATTACH, nullptr);
            callback++;
        }
    }
    return TRUE;
}

static BOOL
PerformBaseRelocation(MemoryModule::Impl* module, ptrdiff_t delta)
{
    unsigned char *codeBase = module->codeBase;
    PIMAGE_BASE_RELOCATION relocation;

    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (directory->Size == 0) {
        return (delta == 0);
    }

    relocation = (PIMAGE_BASE_RELOCATION) (codeBase + directory->VirtualAddress);
    for (; relocation->VirtualAddress > 0; ) {
        DWORD i;
        unsigned char *dest = codeBase + relocation->VirtualAddress;
        unsigned short *relInfo = (unsigned short*) OffsetPointer(relocation, IMAGE_SIZEOF_BASE_RELOCATION);
        for (i=0; i<((relocation->SizeOfBlock-IMAGE_SIZEOF_BASE_RELOCATION) / 2); i++, relInfo++) {
            // the upper 4 bits define the type of relocation
            int type = *relInfo >> 12;
            // the lower 12 bits define the offset
            int offset = *relInfo & 0xfff;

            switch (type)
            {
            case IMAGE_REL_BASED_ABSOLUTE:
                // skip relocation
                break;

            case IMAGE_REL_BASED_HIGHLOW:
                // change complete 32 bit address
                {
                    DWORD *patchAddrHL = (DWORD *) (dest + offset);
                    *patchAddrHL += (DWORD) delta;
                }
                break;

#ifdef _WIN64
            case IMAGE_REL_BASED_DIR64:
                {
                    ULONGLONG *patchAddr64 = (ULONGLONG *) (dest + offset);
                    *patchAddr64 += (ULONGLONG) delta;
                }
                break;
#endif

            default:
                //printf("Unknown relocation: %d\n", type);
                break;
            }
        }

        // advance to next relocation block
        relocation = (PIMAGE_BASE_RELOCATION) OffsetPointer(relocation, relocation->SizeOfBlock);
    }
    return TRUE;
}

static BOOL
BuildImportTable(MemoryModule::Impl* module)
{
    unsigned char *codeBase = module->codeBase;
    PIMAGE_IMPORT_DESCRIPTOR importDesc;
    BOOL result = TRUE;

    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (directory->Size == 0) {
        return TRUE;
    }

    importDesc = (PIMAGE_IMPORT_DESCRIPTOR) (codeBase + directory->VirtualAddress);
    for (; !IsBadReadPtr(importDesc, sizeof(IMAGE_IMPORT_DESCRIPTOR)) && importDesc->Name; importDesc++) {
        uintptr_t *thunkRef;
        FARPROC *funcRef;
        MemoryModule::CustomModuleHandle handle = module->loadLibrary((LPCSTR) (codeBase + importDesc->Name), module->userdata);
        if (handle == nullptr) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            result = FALSE;
            break;
        }

        bool addedToModules = false;
        try {
            module->modules.push_back(handle);
            addedToModules = true;
        } catch (const std::bad_alloc&) {
            module->freeLibrary(handle, module->userdata);
            SetLastError(ERROR_OUTOFMEMORY);
            result = FALSE;
        }

        if (!result) {
            if (addedToModules) {
                module->modules.pop_back();
            }
            break;
        }

        if (importDesc->OriginalFirstThunk) {
            thunkRef = (uintptr_t *) (codeBase + importDesc->OriginalFirstThunk);
            funcRef = (FARPROC *) (codeBase + importDesc->FirstThunk);
        } else {
            // no hint table
            thunkRef = (uintptr_t *) (codeBase + importDesc->FirstThunk);
            funcRef = (FARPROC *) (codeBase + importDesc->FirstThunk);
        }
        for (; *thunkRef; thunkRef++, funcRef++) {
            if (IMAGE_SNAP_BY_ORDINAL(*thunkRef)) {
                *funcRef = module->getProcAddress(handle, (LPCSTR)IMAGE_ORDINAL(*thunkRef), module->userdata);
            } else {
                PIMAGE_IMPORT_BY_NAME thunkData = (PIMAGE_IMPORT_BY_NAME) (codeBase + (*thunkRef));
                *funcRef = module->getProcAddress(handle, (LPCSTR)&thunkData->Name, module->userdata);
            }
            if (*funcRef == 0) {
                result = FALSE;
                break;
            }
        }

        if (!result) {
            if (addedToModules) {
                module->modules.pop_back();
            }
            module->freeLibrary(handle, module->userdata);
            SetLastError(ERROR_PROC_NOT_FOUND);
            break;
        }
    }

    return result;
}

LPVOID MemoryModule::defaultAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect, void* userdata)
{
        UNREFERENCED_PARAMETER(userdata);
        return VirtualAlloc(address, size, allocationType, protect);
}

BOOL MemoryModule::defaultFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType, void* userdata)
{
        UNREFERENCED_PARAMETER(userdata);
        return VirtualFree(lpAddress, dwSize, dwFreeType);
}

MemoryModule::CustomModuleHandle MemoryModule::defaultLoadLibrary(LPCSTR filename, void *userdata)
{
    HMODULE result;
    UNREFERENCED_PARAMETER(userdata);
    result = LoadLibraryA(filename);
    if (result == nullptr) {
        return nullptr;
    }

    return (CustomModuleHandle) result;
}

FARPROC MemoryModule::defaultGetProcAddress(CustomModuleHandle module, LPCSTR name, void *userdata)
{
    UNREFERENCED_PARAMETER(userdata);
    return (FARPROC) GetProcAddress((HMODULE) module, name);
}

void MemoryModule::defaultFreeLibrary(CustomModuleHandle module, void *userdata)
{
    UNREFERENCED_PARAMETER(userdata);
    FreeLibrary((HMODULE) module);
}

MemoryModule MemoryModule::load(const void* data, size_t size)
{
    return load(data, size, defaultAlloc, defaultFree, defaultLoadLibrary, defaultGetProcAddress, defaultFreeLibrary, nullptr);
}

MemoryModule MemoryModule::load(const void* data, size_t size,
    CustomAllocFunc allocMemory,
    CustomFreeFunc freeMemory,
    CustomLoadLibraryFunc loadLibrary,
    CustomGetProcAddressFunc getProcAddress,
    CustomFreeLibraryFunc freeLibrary,
    void *userdata)
{
    return MemoryModule(loadInternal(data, size, allocMemory, freeMemory, loadLibrary, getProcAddress, freeLibrary, userdata));
}

MemoryModule::Impl* MemoryModule::loadInternal(const void* data, size_t size,
    CustomAllocFunc allocMemory,
    CustomFreeFunc freeMemory,
    CustomLoadLibraryFunc loadLibrary,
    CustomGetProcAddressFunc getProcAddress,
    CustomFreeLibraryFunc freeLibrary,
    void *userdata)
{
    Impl* rawModule = new (std::nothrow) Impl();
    if (!rawModule) {
        SetLastError(ERROR_OUTOFMEMORY);
        return nullptr;
    }
    std::unique_ptr<Impl, ImplDeleter> result(rawModule, ImplDeleter{});
    Impl* module = result.get();
    PIMAGE_DOS_HEADER dos_header;
    PIMAGE_NT_HEADERS old_header;
    unsigned char *code, *headers;
    ptrdiff_t locationDelta;
    SYSTEM_INFO sysInfo;
    PIMAGE_SECTION_HEADER section;
    DWORD i;
    size_t optionalSectionSize;
    size_t lastSectionEnd = 0;
    size_t alignedImageSize;
#ifdef _WIN64
    std::vector<void*> blockedMemory;
#endif

    if (!CheckSize(size, sizeof(IMAGE_DOS_HEADER))) {
        return nullptr;
    }
    dos_header = (PIMAGE_DOS_HEADER)data;
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return nullptr;
    }

    if (!CheckSize(size, dos_header->e_lfanew + sizeof(IMAGE_NT_HEADERS))) {
        return nullptr;
    }
    old_header = (PIMAGE_NT_HEADERS)&((const unsigned char *)(data))[dos_header->e_lfanew];
    if (old_header->Signature != IMAGE_NT_SIGNATURE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return nullptr;
    }

    if (old_header->FileHeader.Machine != HOST_MACHINE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return nullptr;
    }

    if (old_header->OptionalHeader.SectionAlignment & 1) {
        // Only support section alignments that are a multiple of 2
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return nullptr;
    }

    section = IMAGE_FIRST_SECTION(old_header);
    optionalSectionSize = old_header->OptionalHeader.SectionAlignment;
    for (i=0; i<old_header->FileHeader.NumberOfSections; i++, section++) {
        size_t endOfSection;
        if (section->SizeOfRawData == 0) {
            // Section without data in the DLL
            endOfSection = section->VirtualAddress + optionalSectionSize;
        } else {
            endOfSection = section->VirtualAddress + section->SizeOfRawData;
        }

        if (endOfSection > lastSectionEnd) {
            lastSectionEnd = endOfSection;
        }
    }

    GetNativeSystemInfo(&sysInfo);
    alignedImageSize = AlignValueUp(old_header->OptionalHeader.SizeOfImage, sysInfo.dwPageSize);
    if (alignedImageSize != AlignValueUp(lastSectionEnd, sysInfo.dwPageSize)) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return nullptr;
    }

    // reserve memory for image of library
    // XXX: is it correct to commit the complete memory region at once?
    //      calling DllEntry raises an exception if we don't...
    code = (unsigned char *)allocMemory((LPVOID)(old_header->OptionalHeader.ImageBase),
        alignedImageSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE,
        userdata);

    if (code == nullptr) {
        // try to allocate memory at arbitrary position
        code = (unsigned char *)allocMemory(nullptr,
            alignedImageSize,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE,
            userdata);
        if (code == nullptr) {
            SetLastError(ERROR_OUTOFMEMORY);
            return nullptr;
        }
    }

#ifdef _WIN64
    // Memory block may not span 4 GB boundaries.
    while ((((uintptr_t) code) >> 32) < (((uintptr_t) (code + alignedImageSize)) >> 32)) {
        try {
            blockedMemory.push_back(code);
        } catch (const std::bad_alloc&) {
            freeMemory(code, 0, MEM_RELEASE, userdata);
            FreeBlockedMemory(blockedMemory, freeMemory, userdata);
            SetLastError(ERROR_OUTOFMEMORY);
            return nullptr;
        }

        code = (unsigned char *)allocMemory(nullptr,
            alignedImageSize,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE,
            userdata);
        if (code == nullptr) {
            FreeBlockedMemory(blockedMemory, freeMemory, userdata);
            SetLastError(ERROR_OUTOFMEMORY);
            return nullptr;
        }
    }
#endif

    module->alloc = allocMemory;
    module->free = freeMemory;
    module->loadLibrary = loadLibrary;
    module->getProcAddress = getProcAddress;
    module->freeLibrary = freeLibrary;
    module->userdata = userdata;
    module->pageSize = sysInfo.dwPageSize;
    module->codeBase = code;
    module->isDLL = (old_header->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
#ifdef _WIN64
    module->blockedMemory = std::move(blockedMemory);
#endif

    if (!CheckSize(size, old_header->OptionalHeader.SizeOfHeaders)) {
        goto error;
    }

    // commit memory for headers
    headers = (unsigned char *)allocMemory(code,
        old_header->OptionalHeader.SizeOfHeaders,
        MEM_COMMIT,
        PAGE_READWRITE,
        userdata);

    // copy PE header to code
    memcpy(headers, dos_header, old_header->OptionalHeader.SizeOfHeaders);
    module->headers = (PIMAGE_NT_HEADERS)&((const unsigned char *)(headers))[dos_header->e_lfanew];

    // update position
    module->headers->OptionalHeader.ImageBase = (uintptr_t)code;

    // copy sections from DLL file block to new memory location
    if (!CopySections((const unsigned char *) data, size, old_header, module)) {
        goto error;
    }

    // adjust base address of imported data
    locationDelta = (ptrdiff_t)(module->headers->OptionalHeader.ImageBase - old_header->OptionalHeader.ImageBase);
    if (locationDelta != 0) {
        module->isRelocated = PerformBaseRelocation(module, locationDelta);
    } else {
        module->isRelocated = true;
    }

    // load required dlls and adjust function table of imports
    if (!BuildImportTable(module)) {
        goto error;
    }

    // mark memory pages depending on section headers and release
    // sections that are marked as "discardable"
    if (!FinalizeSections(module)) {
        goto error;
    }

    // TLS callbacks are executed BEFORE the main loading
    if (!ExecuteTLS(module)) {
        goto error;
    }

    // get entry point of loaded library
    if (module->headers->OptionalHeader.AddressOfEntryPoint != 0) {
        if (module->isDLL) {
            DllEntryProc DllEntry = (DllEntryProc)(LPVOID)(code + module->headers->OptionalHeader.AddressOfEntryPoint);
            // notify library about attaching to process
            BOOL successfull = (*DllEntry)((HINSTANCE)code, DLL_PROCESS_ATTACH, 0);
            if (!successfull) {
                SetLastError(ERROR_DLL_INIT_FAILED);
                goto error;
            }
            module->initialized = true;
        } else {
            module->exeEntry = (ExeEntryProc)(LPVOID)(code + module->headers->OptionalHeader.AddressOfEntryPoint);
        }
    } else {
        module->exeEntry = nullptr;
    }

    return result.release();

error:
    // cleanup
    return nullptr;
}

FARPROC MemoryModule::getProcAddress(LPCSTR name) const
{
    if (!impl_) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    Impl* module = impl_.get();
    unsigned char* codeBase = module->codeBase;
    DWORD idx = 0;
    PIMAGE_EXPORT_DIRECTORY exports;
    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (directory->Size == 0) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }

    exports = (PIMAGE_EXPORT_DIRECTORY)(codeBase + directory->VirtualAddress);
    if (exports->NumberOfNames == 0 || exports->NumberOfFunctions == 0) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }

    if (HIWORD(name) == 0) {
        if (LOWORD(name) < exports->Base) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return nullptr;
        }

        idx = LOWORD(name) - exports->Base;
    } else if (!exports->NumberOfNames) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    } else {
        if (!module->exportsSorted) {
            DWORD* nameRef = (DWORD*)(codeBase + exports->AddressOfNames);
            WORD* ordinal = (WORD*)(codeBase + exports->AddressOfNameOrdinals);
            module->nameExportsTable.clear();
            try {
                module->nameExportsTable.reserve(exports->NumberOfNames);
                for (DWORD i = 0; i < exports->NumberOfNames; ++i, ++nameRef, ++ordinal) {
                    ExportNameEntry entry;
                    entry.name = (const char*)(codeBase + (*nameRef));
                    entry.idx = *ordinal;
                    module->nameExportsTable.push_back(entry);
                }
                std::sort(module->nameExportsTable.begin(), module->nameExportsTable.end(),
                    [](const ExportNameEntry& lhs, const ExportNameEntry& rhs) {
                        return std::strcmp(lhs.name, rhs.name) < 0;
                    });
                module->exportsSorted = true;
            } catch (const std::bad_alloc&) {
                module->nameExportsTable.clear();
                SetLastError(ERROR_OUTOFMEMORY);
                return nullptr;
            }
        }

        const auto it = std::lower_bound(
            module->nameExportsTable.begin(),
            module->nameExportsTable.end(),
            name,
            [](const ExportNameEntry& entry, LPCSTR value) {
                return std::strcmp(entry.name, value) < 0;
            });

        if (it == module->nameExportsTable.end() || std::strcmp(it->name, name) != 0) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return nullptr;
        }

        idx = it->idx;
    }

    if (idx >= exports->NumberOfFunctions) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }

    return reinterpret_cast<FARPROC>(codeBase + (*(DWORD*)(codeBase + exports->AddressOfFunctions + (idx * sizeof(DWORD)))));
}

static void FreeModule(MemoryModule::Impl* module)
{
    if (!module) {
        return;
    }

    if (module->initialized && module->headers && module->codeBase) {
        DllEntryProc DllEntry = (DllEntryProc)(LPVOID)(module->codeBase + module->headers->OptionalHeader.AddressOfEntryPoint);
        (*DllEntry)((HINSTANCE)module->codeBase, DLL_PROCESS_DETACH, 0);
    }
    module->initialized = false;

    if (module->freeLibrary) {
        for (auto handle : module->modules) {
            if (handle != nullptr) {
                module->freeLibrary(handle, module->userdata);
            }
        }
    }
    module->modules.clear();
    module->nameExportsTable.clear();
    module->exportsSorted = false;

    if (module->codeBase && module->free) {
        module->free(module->codeBase, 0, MEM_RELEASE, module->userdata);
    }
    module->codeBase = nullptr;

#ifdef _WIN64
    if (!module->blockedMemory.empty() && module->free) {
        FreeBlockedMemory(module->blockedMemory, module->free, module->userdata);
        module->blockedMemory.clear();
    }
#endif

    module->headers = nullptr;
    module->exeEntry = nullptr;
    module->isRelocated = false;
}

void MemoryModule::ImplDeleter::operator()(Impl* module) const noexcept
{
    FreeModule(module);
    delete module;
}

MemoryModule::MemoryModule(Impl* impl) noexcept
    : impl_(impl, ImplDeleter{})
{
}

MemoryModule::~MemoryModule() = default;

MemoryModule::MemoryModule(MemoryModule&& other) noexcept = default;

MemoryModule& MemoryModule::operator=(MemoryModule&& other) noexcept = default;

void MemoryModule::reset() noexcept
{
    impl_.reset();
}

int MemoryModule::callEntryPoint()
{
    if (!impl_ || impl_->isDLL || impl_->exeEntry == nullptr || !impl_->isRelocated) {
        return -1;
    }

    return impl_->exeEntry();
}

#define DEFAULT_LANGUAGE        MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)

MemoryModule::ResourceHandle MemoryModule::findResource(LPCTSTR name, LPCTSTR type) const
{
    return findResourceEx(name, type, DEFAULT_LANGUAGE);
}

static PIMAGE_RESOURCE_DIRECTORY_ENTRY _MemorySearchResourceEntry(
    void *root,
    PIMAGE_RESOURCE_DIRECTORY resources,
    LPCTSTR key)
{
    PIMAGE_RESOURCE_DIRECTORY_ENTRY entries = (PIMAGE_RESOURCE_DIRECTORY_ENTRY) (resources + 1);
    PIMAGE_RESOURCE_DIRECTORY_ENTRY result = nullptr;
    DWORD start;
    DWORD end;
    DWORD middle;

    if (!IS_INTRESOURCE(key) && key[0] == TEXT('#')) {
        // special case: resource id given as string
        TCHAR *endpos = nullptr;
        long int tmpkey = (WORD) _tcstol((TCHAR *) &key[1], &endpos, 10);
        if (tmpkey <= 0xffff && lstrlen(endpos) == 0) {
            key = MAKEINTRESOURCE(tmpkey);
        }
    }

    // entries are stored as ordered list of named entries,
    // followed by an ordered list of id entries - we can do
    // a binary search to find faster...
    if (IS_INTRESOURCE(key)) {
        WORD check = (WORD) (uintptr_t) key;
        start = resources->NumberOfNamedEntries;
        end = start + resources->NumberOfIdEntries;

        while (end > start) {
            WORD entryName;
            middle = (start + end) >> 1;
            entryName = (WORD) entries[middle].Name;
            if (check < entryName) {
                end = (end != middle ? middle : middle-1);
            } else if (check > entryName) {
                start = (start != middle ? middle : middle+1);
            } else {
                result = &entries[middle];
                break;
            }
        }
    } else {
        LPCWSTR searchKey;
        size_t searchKeyLen = _tcslen(key);
#if defined(UNICODE)
        searchKey = key;
#else
        // Resource names are always stored using 16bit characters, need to
        // convert string we search for.
#define MAX_LOCAL_KEY_LENGTH 2048
        // In most cases resource names are short, so optimize for that by
        // using a pre-allocated array.
        wchar_t _searchKeySpace[MAX_LOCAL_KEY_LENGTH+1];
        LPWSTR _searchKey;
        if (searchKeyLen > MAX_LOCAL_KEY_LENGTH) {
            size_t _searchKeySize = (searchKeyLen + 1) * sizeof(wchar_t);
            _searchKey = (LPWSTR) malloc(_searchKeySize);
            if (_searchKey == nullptr) {
                SetLastError(ERROR_OUTOFMEMORY);
                return nullptr;
            }
        } else {
            _searchKey = &_searchKeySpace[0];
        }

        mbstowcs(_searchKey, key, searchKeyLen);
        _searchKey[searchKeyLen] = 0;
        searchKey = _searchKey;
#endif
        start = 0;
        end = resources->NumberOfNamedEntries;
        while (end > start) {
            int cmp;
            PIMAGE_RESOURCE_DIR_STRING_U resourceString;
            middle = (start + end) >> 1;
            resourceString = (PIMAGE_RESOURCE_DIR_STRING_U) OffsetPointer(root, entries[middle].Name & 0x7FFFFFFF);
            cmp = _wcsnicmp(searchKey, resourceString->NameString, resourceString->Length);
            if (cmp == 0) {
                // Handle partial match
                if (searchKeyLen > resourceString->Length) {
                    cmp = 1;
                } else if (searchKeyLen < resourceString->Length) {
                    cmp = -1;
                }
            }
            if (cmp < 0) {
                end = (middle != end ? middle : middle-1);
            } else if (cmp > 0) {
                start = (middle != start ? middle : middle+1);
            } else {
                result = &entries[middle];
                break;
            }
        }
#if !defined(UNICODE)
        if (searchKeyLen > MAX_LOCAL_KEY_LENGTH) {
            free(_searchKey);
        }
#undef MAX_LOCAL_KEY_LENGTH
#endif
    }

    return result;
}

MemoryModule::ResourceHandle MemoryModule::findResourceEx(LPCTSTR name, LPCTSTR type, WORD language) const
{
    if (!impl_) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    unsigned char* codeBase = impl_->codeBase;
    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(impl_.get(), IMAGE_DIRECTORY_ENTRY_RESOURCE);
    PIMAGE_RESOURCE_DIRECTORY rootResources;
    PIMAGE_RESOURCE_DIRECTORY nameResources;
    PIMAGE_RESOURCE_DIRECTORY typeResources;
    PIMAGE_RESOURCE_DIRECTORY_ENTRY foundType;
    PIMAGE_RESOURCE_DIRECTORY_ENTRY foundName;
    PIMAGE_RESOURCE_DIRECTORY_ENTRY foundLanguage;
    if (directory->Size == 0) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return nullptr;
    }

    if (language == DEFAULT_LANGUAGE) {
        language = LANGIDFROMLCID(GetThreadLocale());
    }

    rootResources = (PIMAGE_RESOURCE_DIRECTORY)(codeBase + directory->VirtualAddress);
    foundType = _MemorySearchResourceEntry(rootResources, rootResources, type);
    if (foundType == nullptr) {
        SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return nullptr;
    }

    typeResources = (PIMAGE_RESOURCE_DIRECTORY)(codeBase + directory->VirtualAddress + (foundType->OffsetToData & 0x7fffffff));
    foundName = _MemorySearchResourceEntry(rootResources, typeResources, name);
    if (foundName == nullptr) {
        SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        return nullptr;
    }

    nameResources = (PIMAGE_RESOURCE_DIRECTORY)(codeBase + directory->VirtualAddress + (foundName->OffsetToData & 0x7fffffff));
    foundLanguage = _MemorySearchResourceEntry(rootResources, nameResources, (LPCTSTR)(uintptr_t)language);
    if (foundLanguage == nullptr) {
        if (nameResources->NumberOfIdEntries == 0) {
            SetLastError(ERROR_RESOURCE_LANG_NOT_FOUND);
            return nullptr;
        }

        foundLanguage = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(nameResources + 1);
    }

    return codeBase + directory->VirtualAddress + (foundLanguage->OffsetToData & 0x7fffffff);
}

DWORD MemoryModule::sizeofResource(ResourceHandle resource) const
{
    if (!impl_) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    if (resource == nullptr) {
        return 0;
    }

    const auto entry = static_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(resource);
    return entry->Size;
}

LPVOID MemoryModule::loadResource(ResourceHandle resource) const
{
    if (!impl_) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    if (resource == nullptr) {
        return nullptr;
    }

    const auto entry = static_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(resource);
    return impl_->codeBase + entry->OffsetToData;
}

int MemoryModule::loadString(UINT id, LPTSTR buffer, int maxsize) const
{
    return loadStringEx(id, buffer, maxsize, DEFAULT_LANGUAGE);
}

int MemoryModule::loadStringEx(UINT id, LPTSTR buffer, int maxsize, WORD language) const
{
    if (!buffer || maxsize <= 0) {
        return 0;
    }

    if (!impl_) {
        SetLastError(ERROR_INVALID_HANDLE);
        buffer[0] = 0;
        return 0;
    }

    auto resource = findResourceEx(MAKEINTRESOURCE((id >> 4) + 1), RT_STRING, language);
    if (resource == nullptr) {
        buffer[0] = 0;
        return 0;
    }

    auto data = static_cast<PIMAGE_RESOURCE_DIR_STRING_U>(loadResource(resource));
    id = id & 0x0f;
    while (id--) {
        data = (PIMAGE_RESOURCE_DIR_STRING_U)OffsetPointer(data, (data->Length + 1) * sizeof(WCHAR));
    }
    if (data->Length == 0) {
        SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        buffer[0] = 0;
        return 0;
    }

    DWORD size = data->Length;
    if (size >= static_cast<DWORD>(maxsize)) {
        size = static_cast<DWORD>(maxsize);
    } else {
        buffer[size] = 0;
    }
#if defined(UNICODE)
    wcsncpy(buffer, data->NameString, size);
#else
    wcstombs(buffer, data->NameString, size);
#endif
    return static_cast<int>(size);
}

#ifdef TESTSUITE
#include <stdio.h>

#ifndef PRIxPTR
#ifdef _WIN64
#define PRIxPTR "I64x"
#else
#define PRIxPTR "x"
#endif
#endif

static const uintptr_t AlignValueDownTests[][3] = {
    {16, 16, 16},
    {17, 16, 16},
    {32, 16, 32},
    {33, 16, 32},
#ifdef _WIN64
    {0x12345678abcd1000, 0x1000, 0x12345678abcd1000},
    {0x12345678abcd101f, 0x1000, 0x12345678abcd1000},
#endif
    {0, 0, 0},
};

static const uintptr_t AlignValueUpTests[][3] = {
    {16, 16, 16},
    {17, 16, 32},
    {32, 16, 32},
    {33, 16, 48},
#ifdef _WIN64
    {0x12345678abcd1000, 0x1000, 0x12345678abcd1000},
    {0x12345678abcd101f, 0x1000, 0x12345678abcd2000},
#endif
    {0, 0, 0},
};

BOOL MemoryModuleTestsuite() {
    BOOL success = TRUE;
    size_t idx;
    for (idx = 0; AlignValueDownTests[idx][0]; ++idx) {
        const uintptr_t* tests = AlignValueDownTests[idx];
        uintptr_t value = AlignValueDown(tests[0], tests[1]);
        if (value != tests[2]) {
            printf("AlignValueDown failed for 0x%" PRIxPTR "/0x%" PRIxPTR ": expected 0x%" PRIxPTR ", got 0x%" PRIxPTR "\n",
                tests[0], tests[1], tests[2], value);
            success = FALSE;
        }
    }
    for (idx = 0; AlignValueDownTests[idx][0]; ++idx) {
        const uintptr_t* tests = AlignValueUpTests[idx];
        uintptr_t value = AlignValueUp(tests[0], tests[1]);
        if (value != tests[2]) {
            printf("AlignValueUp failed for 0x%" PRIxPTR "/0x%" PRIxPTR ": expected 0x%" PRIxPTR ", got 0x%" PRIxPTR "\n",
                tests[0], tests[1], tests[2], value);
            success = FALSE;
        }
    }
    if (success) {
        printf("OK\n");
    }
    return success;
}
#endif
