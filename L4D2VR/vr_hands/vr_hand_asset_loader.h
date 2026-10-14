#pragma once

#include "vr_hand_types.h"

#include <string>

class VrHandAssetLoader
{
public:
    // Paths use UTF8, matching OpenVR runtime paths and glTF buffer/image URIs.
    static bool LoadGlb(const std::string& path, VrHandMeshAsset& outAsset, std::string& outError);
    static bool LoadStaticGlb(const std::string& path, VrHandMeshAsset& outAsset, std::string& outError);
};
