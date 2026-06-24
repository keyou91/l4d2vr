#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <string>

struct L4D2VR_OpenXrLoaderLoadResult
{
    HMODULE module = nullptr;
    std::string path;
    std::string detail;
    DWORD error = ERROR_SUCCESS;

    bool Loaded() const { return module != nullptr; }
};

L4D2VR_OpenXrLoaderLoadResult L4D2VR_LoadOpenXrLoader();
