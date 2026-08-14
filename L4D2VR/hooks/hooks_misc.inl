// ------------------------------------------------------------
// Multicore viewmodel stabilization helpers
//
// In Source, viewmodels are often drawn with pCustomBoneToWorld (bone matrices in world space).
// In that case, overriding ModelRenderInfo_t.origin/angles does NOT move the model.
//
// For queued rendering (mat_queue_mode!=0) we must instead apply a delta transform to the
// custom bone matrices for the draw call, so the viewmodel uses the controller-anchored pose
// sampled on the render thread (no shared-state writes, no tearing).
// ------------------------------------------------------------
namespace vr_vm_stabilize
{
    struct Mat3x4
    {
        float m[3][4];
    };

    template <typename T>
    inline bool SafeRead(const void* p, T& out)
    {
#if defined(_MSC_VER)
        __try
        {
            out = *reinterpret_cast<const T*>(p);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        // Non-MSVC builds are not expected for this project.
        // Keep it simple: attempt the read.
        out = *reinterpret_cast<const T*>(p);
        return true;
#endif
    }

    inline bool SafeReadBytes(const void* p, void* out, size_t len)
    {
#if defined(_MSC_VER)
        __try
        {
            memcpy(out, p, len);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        memcpy(out, p, len);
        return true;
#endif
    }

    inline Vector GetOrigin(const Mat3x4& a)
    {
        return Vector(a.m[0][3], a.m[1][3], a.m[2][3]);
    }

    inline Mat3x4 Identity()
    {
        Mat3x4 out{};
        out.m[0][0] = 1.0f;
        out.m[1][1] = 1.0f;
        out.m[2][2] = 1.0f;
        return out;
    }

    inline void BuildFromOrgAngles(const Vector& origin, const QAngle& ang, Mat3x4& out)
    {
        Vector f, r, u;
        QAngle::AngleVectors(ang, &f, &r, &u);

        out.m[0][0] = f.x; out.m[0][1] = r.x; out.m[0][2] = u.x; out.m[0][3] = origin.x;
        out.m[1][0] = f.y; out.m[1][1] = r.y; out.m[1][2] = u.y; out.m[1][3] = origin.y;
        out.m[2][0] = f.z; out.m[2][1] = r.z; out.m[2][2] = u.z; out.m[2][3] = origin.z;
    }

    // Invert a rigid transform (rotation + translation only)
    inline void InvertTR(const Mat3x4& in, Mat3x4& out)
    {
        // Transpose rotation
        out.m[0][0] = in.m[0][0]; out.m[0][1] = in.m[1][0]; out.m[0][2] = in.m[2][0];
        out.m[1][0] = in.m[0][1]; out.m[1][1] = in.m[1][1]; out.m[1][2] = in.m[2][1];
        out.m[2][0] = in.m[0][2]; out.m[2][1] = in.m[1][2]; out.m[2][2] = in.m[2][2];

        const float tx = -in.m[0][3];
        const float ty = -in.m[1][3];
        const float tz = -in.m[2][3];

        out.m[0][3] = tx * out.m[0][0] + ty * out.m[0][1] + tz * out.m[0][2];
        out.m[1][3] = tx * out.m[1][0] + ty * out.m[1][1] + tz * out.m[1][2];
        out.m[2][3] = tx * out.m[2][0] + ty * out.m[2][1] + tz * out.m[2][2];
    }

    // Invert a general affine 3x4 transform. Unlike InvertTR, this remains
    // correct when another plugin applies uniform or non-uniform model scale.
    inline bool InvertAffine(const Mat3x4& in, Mat3x4& out)
    {
        double a[3][3]{};
        double maxElement = 0.0;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const double value = static_cast<double>(in.m[row][column]);
                if (!std::isfinite(value))
                    return false;
                a[row][column] = value;
                maxElement = (std::max)(maxElement, std::fabs(value));
            }
            if (!std::isfinite(static_cast<double>(in.m[row][3])))
                return false;
        }

        if (maxElement <= 1.0e-12)
            return false;

        const double determinant =
            a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
            a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
            a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        const double determinantTolerance =
            (std::max)(1.0e-15, maxElement * maxElement * maxElement * 1.0e-9);
        if (!std::isfinite(determinant) ||
            std::fabs(determinant) <= determinantTolerance)
        {
            return false;
        }

        const double inverseDeterminant = 1.0 / determinant;
        double inverse[3][3]{};
        inverse[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inverseDeterminant;
        inverse[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inverseDeterminant;
        inverse[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inverseDeterminant;
        inverse[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inverseDeterminant;
        inverse[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inverseDeterminant;
        inverse[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inverseDeterminant;
        inverse[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inverseDeterminant;
        inverse[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inverseDeterminant;
        inverse[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inverseDeterminant;

        const double translation[3] = {
            static_cast<double>(in.m[0][3]),
            static_cast<double>(in.m[1][3]),
            static_cast<double>(in.m[2][3]),
        };
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                if (!std::isfinite(inverse[row][column]))
                    return false;
                out.m[row][column] = static_cast<float>(inverse[row][column]);
            }

            const double inverseTranslation =
                -(inverse[row][0] * translation[0] +
                    inverse[row][1] * translation[1] +
                    inverse[row][2] * translation[2]);
            if (!std::isfinite(inverseTranslation))
                return false;
            out.m[row][3] = static_cast<float>(inverseTranslation);
        }
        return true;
    }

    inline void Mul(const Mat3x4& a, const Mat3x4& b, Mat3x4& out)
    {
        // Rotation
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            }
        }
        // Translation
        out.m[0][3] = a.m[0][0] * b.m[0][3] + a.m[0][1] * b.m[1][3] + a.m[0][2] * b.m[2][3] + a.m[0][3];
        out.m[1][3] = a.m[1][0] * b.m[0][3] + a.m[1][1] * b.m[1][3] + a.m[1][2] * b.m[2][3] + a.m[1][3];
        out.m[2][3] = a.m[2][0] * b.m[0][3] + a.m[2][1] * b.m[1][3] + a.m[2][2] * b.m[2][3] + a.m[2][3];
    }

    inline void ApplyDelta(const Mat3x4& delta, Mat3x4* bones, int numBones)
    {
        for (int i = 0; i < numBones; ++i)
        {
            Mat3x4 tmp{};
            Mul(delta, bones[i], tmp);
            bones[i] = tmp;
        }
    }
    // L4D2 StudioRender synchronously copies ordinary heap bone matrices into
    // Source RenderData before DrawModelExecute returns, including in q2. Keep
    // scratch alive for the complete hook call, then rewind it. The previous
    // 64-frame heap ring could grow without bound whenever m_RenderFrameSeq
    // stopped advancing (for example while the game window was inactive).
    struct StableBoneScratchStack
    {
        static constexpr size_t kCapacity = 16384;
        std::unique_ptr<Mat3x4[]> storage;
        size_t cursor = 0;
        unsigned int activeScopes = 0;
    };

    inline StableBoneScratchStack& GetStableBoneScratchStack()
    {
        static thread_local StableBoneScratchStack stack;
        return stack;
    }

    class ScopedStableBoneScratch
    {
    public:
        ScopedStableBoneScratch()
            : m_Stack(&GetStableBoneScratchStack()),
            m_Mark(m_Stack->cursor)
        {
            ++m_Stack->activeScopes;
        }

        ~ScopedStableBoneScratch()
        {
            if (!m_Stack)
                return;
            m_Stack->cursor = m_Mark;
            if (m_Stack->activeScopes > 0)
                --m_Stack->activeScopes;
        }

        ScopedStableBoneScratch(const ScopedStableBoneScratch&) = delete;
        ScopedStableBoneScratch& operator=(
            const ScopedStableBoneScratch&) = delete;

    private:
        StableBoneScratchStack* m_Stack = nullptr;
        size_t m_Mark = 0;
    };

    inline Mat3x4* AllocStableBones(int numBones, uint32_t seqEven)
    {
        (void)seqEven;
        if (numBones <= 0 || numBones > 512)
            return nullptr;

        StableBoneScratchStack& stack = GetStableBoneScratchStack();
        if (stack.activeScopes == 0)
            return nullptr;

        if (!stack.storage)
        {
            stack.storage.reset(
                new (std::nothrow) Mat3x4[StableBoneScratchStack::kCapacity]);
            if (!stack.storage)
                return nullptr;
        }

        const size_t count = static_cast<size_t>(numBones);
        if (stack.cursor > StableBoneScratchStack::kCapacity - count)
            return nullptr;

        Mat3x4* const result = stack.storage.get() + stack.cursor;
        stack.cursor += count;
        return result;
    }
    // DrawModelState_t is opaque here, but in Source the first pointer is typically studiohdr_t*.
    // We avoid hard-crashing by SEH-guarding reads and probing common studiohdr_t offsets for numbones.
    inline bool TryGetNumBonesFromDrawState(void* drawState, int& outBones)
    {
        if (!drawState)
            return false;

        void* studioHdr = nullptr;
        if (!SafeRead(drawState, studioHdr) || !studioHdr)
            return false;

        // Common studiohdr_t::numbones offsets across Source branches.
        static const int kOffsets[] = { 0x9C, 0xA0, 0x98, 0x94, 0xA4, 0xA8, 0x90, 0x8C, 0xB0 };
        for (int off : kOffsets)
        {
            int n = 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(studioHdr) + off;
            if (SafeRead(p, n) && n > 0 && n <= 512)
            {
                outBones = n;
                return true;
            }
        }
        return false;
    }

    inline bool TryGetStudioHdrFromDrawState(void* drawState, const uint8_t*& outStudioHdr)
    {
        outStudioHdr = nullptr;
        if (!drawState)
            return false;

        void* studioHdr = nullptr;
        if (!SafeRead(drawState, studioHdr) || !studioHdr)
            return false;

        outStudioHdr = reinterpret_cast<const uint8_t*>(studioHdr);
        return true;
    }

    inline bool TryGetBoneTableLayout(void* drawState, int& outNumBones, int& outBoneIndex, int& outNumBonesOffset)
    {
        outNumBones = 0;
        outBoneIndex = 0;
        outNumBonesOffset = 0;

        const uint8_t* studioHdr = nullptr;
        if (!TryGetStudioHdrFromDrawState(drawState, studioHdr))
            return false;

        int studioLength = 0;
        SafeRead(studioHdr + 0x4C, studioLength);

        static const int kNumBoneOffsets[] = { 0x9C, 0xA0, 0x98, 0x94, 0xA4, 0xA8, 0x90, 0x8C, 0xB0 };
        for (int off : kNumBoneOffsets)
        {
            int n = 0;
            int boneIndex = 0;
            if (!SafeRead(studioHdr + off, n) || !SafeRead(studioHdr + off + 4, boneIndex))
                continue;
            if (n <= 0 || n > 512 || boneIndex <= 0 || boneIndex > 0x200000)
                continue;
            if (studioLength > 0 && boneIndex >= studioLength)
                continue;

            outNumBones = n;
            outBoneIndex = boneIndex;
            outNumBonesOffset = off;
            return true;
        }
        return false;
    }

    inline bool TryReadCStringSafe(const char* ptr, std::string& out, size_t maxLen = 96)
    {
        out.clear();
        if (!ptr)
            return false;

        constexpr size_t kChunkSize = 16;
        char chunk[kChunkSize];

        for (size_t base = 0; base < maxLen; base += kChunkSize)
        {
            const size_t want = (std::min)(kChunkSize, maxLen - base);
            if (!SafeReadBytes(ptr + base, chunk, want))
                return false;

            for (size_t i = 0; i < want; ++i)
            {
                const char c = chunk[i];
                if (c == '\0')
                    return !out.empty();
                const unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 32 || uc > 126)
                    return false;
                out.push_back(c);
            }
        }
        return false;
    }

    inline bool TryGetStudioMaterialNamesFromDrawState(
        void* drawState,
        std::vector<std::string>& outNames,
        std::vector<std::string>* outDirectories = nullptr)
    {
        outNames.clear();
        if (outDirectories)
            outDirectories->clear();

        const uint8_t* studioHdr = nullptr;
        if (!TryGetStudioHdrFromDrawState(drawState, studioHdr))
            return false;

        // L4D2's 32-bit studiohdr_t keeps the raw texture table at these
        // stable offsets for all supported header versions.
        constexpr int kStudioId = 0x54534449; // "IDST"
        constexpr size_t kLengthOffset = 0x4C;
        constexpr size_t kTextureCountOffset = 0xCC;
        constexpr size_t kTextureTableOffset = 0xD0;
        constexpr size_t kTextureDirectoryCountOffset = 0xD4;
        constexpr size_t kTextureDirectoryTableOffset = 0xD8;
        constexpr size_t kTextureEntrySize = 64;
        constexpr int kMaxStudioLength = 64 * 1024 * 1024;
        constexpr int kMaxTextureCount = 256;
        constexpr int kMaxTextureDirectoryCount = 64;

        int id = 0;
        int version = 0;
        int studioLength = 0;
        int textureCount = 0;
        int textureTableOffset = 0;
        int textureDirectoryCount = 0;
        int textureDirectoryTableOffset = 0;
        if (!SafeRead(studioHdr + 0x00, id) ||
            !SafeRead(studioHdr + 0x04, version) ||
            !SafeRead(studioHdr + kLengthOffset, studioLength) ||
            !SafeRead(studioHdr + kTextureCountOffset, textureCount) ||
            !SafeRead(studioHdr + kTextureTableOffset, textureTableOffset) ||
            !SafeRead(
                studioHdr + kTextureDirectoryCountOffset,
                textureDirectoryCount) ||
            !SafeRead(
                studioHdr + kTextureDirectoryTableOffset,
                textureDirectoryTableOffset))
        {
            return false;
        }

        if (id != kStudioId ||
            version < 44 ||
            version > 60 ||
            studioLength <= static_cast<int>(kTextureTableOffset + sizeof(int)) ||
            studioLength > kMaxStudioLength ||
            textureCount <= 0 ||
            textureCount > kMaxTextureCount ||
            textureTableOffset <= 0)
        {
            return false;
        }

        const size_t studioLengthBytes = static_cast<size_t>(studioLength);
        const size_t textureTable = static_cast<size_t>(textureTableOffset);
        const size_t textureCountBytes = static_cast<size_t>(textureCount);
        if (textureTable >= studioLengthBytes ||
            textureCountBytes >
            (studioLengthBytes - textureTable) / kTextureEntrySize)
        {
            return false;
        }

        outNames.reserve(textureCountBytes);
        for (int i = 0; i < textureCount; ++i)
        {
            const size_t textureEntryOffset =
                textureTable +
                static_cast<size_t>(i) * kTextureEntrySize;
            int textureNameOffset = 0;
            if (!SafeRead(
                studioHdr + textureEntryOffset,
                textureNameOffset) ||
                textureNameOffset <= 0)
            {
                continue;
            }

            const size_t relativeNameOffset =
                static_cast<size_t>(textureNameOffset);
            if (relativeNameOffset >=
                studioLengthBytes - textureEntryOffset)
            {
                continue;
            }

            const size_t absoluteNameOffset =
                textureEntryOffset + relativeNameOffset;
            const size_t maxNameLength = (std::min)(
                static_cast<size_t>(128),
                studioLengthBytes - absoluteNameOffset);
            std::string textureName;
            if (!TryReadCStringSafe(
                reinterpret_cast<const char*>(
                    studioHdr + absoluteNameOffset),
                textureName,
                maxNameLength))
            {
                continue;
            }

            outNames.push_back(std::move(textureName));
        }

        if (outDirectories &&
            textureDirectoryCount > 0 &&
            textureDirectoryCount <= kMaxTextureDirectoryCount &&
            textureDirectoryTableOffset > 0)
        {
            const size_t directoryTable =
                static_cast<size_t>(textureDirectoryTableOffset);
            const size_t directoryCount =
                static_cast<size_t>(textureDirectoryCount);
            if (directoryTable < studioLengthBytes &&
                directoryCount <=
                (studioLengthBytes - directoryTable) / sizeof(int))
            {
                outDirectories->reserve(directoryCount);
                for (int i = 0; i < textureDirectoryCount; ++i)
                {
                    int directoryNameOffset = 0;
                    if (!SafeRead(
                        studioHdr +
                        directoryTable +
                        static_cast<size_t>(i) * sizeof(int),
                        directoryNameOffset) ||
                        directoryNameOffset <= 0 ||
                        static_cast<size_t>(directoryNameOffset) >=
                        studioLengthBytes)
                    {
                        continue;
                    }

                    const size_t absoluteNameOffset =
                        static_cast<size_t>(directoryNameOffset);
                    const size_t maxNameLength = (std::min)(
                        static_cast<size_t>(256),
                        studioLengthBytes - absoluteNameOffset);
                    std::string directoryName;
                    if (!TryReadCStringSafe(
                        reinterpret_cast<const char*>(
                            studioHdr + absoluteNameOffset),
                        directoryName,
                        maxNameLength))
                    {
                        continue;
                    }

                    std::replace(
                        directoryName.begin(),
                        directoryName.end(),
                        '\\',
                        '/');
                    if (!directoryName.empty() &&
                        directoryName.back() != '/')
                    {
                        directoryName.push_back('/');
                    }
                    outDirectories->push_back(std::move(directoryName));
                }
            }
        }

        return !outNames.empty();
    }

    inline std::string ToLowerAscii(std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    inline bool TryGetBoneNameAtStride(
        const uint8_t* studioHdr,
        int studioLength,
        int boneIndex,
        int numBones,
        int stride,
        int bone,
        std::string& outName,
        int& outParent)
    {
        outName.clear();
        outParent = -1;
        if (!studioHdr || bone < 0 || bone >= numBones || stride <= 0)
            return false;

        const size_t boneOffset = static_cast<size_t>(boneIndex) +
            (static_cast<size_t>(stride) * static_cast<size_t>(bone));
        if (studioLength > 0 && boneOffset + 8u > static_cast<size_t>(studioLength))
            return false;

        const uint8_t* boneBase = studioHdr + boneOffset;
        int nameOffset = 0;
        int parent = -1;
        if (!SafeRead(boneBase + 0, nameOffset) || !SafeRead(boneBase + 4, parent))
            return false;
        if (nameOffset <= 0)
            return false;
        if (parent < -1 || parent >= numBones)
            return false;

        // mstudiobone_t::sznameindex is relative to the current bone structure.
        // Workshop replacement models can place the string table well beyond 64 KiB.
        // The previous fixed 0x10000 limit rejected valid bone names for most weapons,
        // while the M16 happened to keep a small readable subset close enough to pass.
        const size_t nameAddressOffset = boneOffset + static_cast<size_t>(nameOffset);
        if (studioLength > 0)
        {
            if (nameAddressOffset >= static_cast<size_t>(studioLength))
                return false;
        }
        else if (nameOffset > 0x400000)
        {
            return false;
        }

        std::string name;
        if (!TryReadCStringSafe(reinterpret_cast<const char*>(studioHdr + nameAddressOffset), name))
            return false;

        outName = name;
        outParent = parent;
        return true;
    }

    struct CachedBoneLayout
    {
        int id = 0;
        int version = 0;
        int studioLength = 0;
        bool success = false;
        std::vector<std::string> names;
        std::vector<int> parents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
    };

    inline bool TryCollectBoneNamesFromDrawStateUncached(
        void* drawState,
        const uint8_t* studioHdr,
        std::vector<std::string>& outNames,
        std::vector<int>& outParents,
        int& outNumBones,
        int& outBoneIndex,
        int& outStride,
        int& outNumBonesOffset)
    {
        outNames.clear();
        outParents.clear();
        outNumBones = 0;
        outBoneIndex = 0;
        outStride = 0;
        outNumBonesOffset = 0;

        if (!TryGetBoneTableLayout(drawState, outNumBones, outBoneIndex, outNumBonesOffset))
            return false;

        int studioLength = 0;
        SafeRead(studioHdr + 0x4C, studioLength);

        static const int kStrideCandidates[] = { 216, 224, 208, 200, 192, 184, 176, 232, 240, 256 };
        int bestStride = 0;
        int bestScore = -1;
        std::vector<std::string> bestNames;
        std::vector<int> bestParents;

        for (int stride : kStrideCandidates)
        {
            std::vector<std::string> names(static_cast<size_t>(outNumBones));
            std::vector<int> parents(static_cast<size_t>(outNumBones), -1);
            int validNames = 0;
            int semanticHits = 0;

            for (int bone = 0; bone < outNumBones; ++bone)
            {
                std::string name;
                int parent = -1;
                if (!TryGetBoneNameAtStride(studioHdr, studioLength, outBoneIndex, outNumBones, stride, bone, name, parent))
                    continue;

                names[static_cast<size_t>(bone)] = name;
                parents[static_cast<size_t>(bone)] = parent;
                ++validNames;

                const std::string lower = ToLowerAscii(name);
                if (lower.find("finger") != std::string::npos ||
                    lower.find("hand") != std::string::npos ||
                    lower.find("wrist") != std::string::npos ||
                    lower.find("clip") != std::string::npos ||
                    lower.find("weapon") != std::string::npos ||
                    lower.find("valvebiped") != std::string::npos ||
                    lower.find("bip01") != std::string::npos)
                {
                    semanticHits += 4;
                }
            }

            const int score = validNames * 2 + semanticHits;
            if (score > bestScore)
            {
                bestScore = score;
                bestStride = stride;
                bestNames.swap(names);
                bestParents.swap(parents);
            }
        }

        if (bestStride <= 0 || bestScore < 6)
            return false;

        // Parent links are independent of bone-name readability. Procedural,
        // jiggle, and attachment bones can have names that fail the string
        // probe while still carrying a valid mstudiobone_t::parent field.
        // Re-read every parent after selecting the stride so those unnamed
        // descendants remain part of head/hand hierarchy operations.
        if (static_cast<int>(bestParents.size()) == outNumBones)
        {
            for (int bone = 0; bone < outNumBones; ++bone)
            {
                const size_t boneOffset =
                    static_cast<size_t>(outBoneIndex) +
                    static_cast<size_t>(bestStride) *
                    static_cast<size_t>(bone);
                if (studioLength > 0 &&
                    boneOffset + 8u >
                    static_cast<size_t>(studioLength))
                {
                    continue;
                }

                int parent = -1;
                if (SafeRead(
                    studioHdr + boneOffset + 4u,
                    parent) &&
                    parent >= -1 &&
                    parent < outNumBones &&
                    parent != bone)
                {
                    bestParents[static_cast<size_t>(bone)] =
                        parent;
                }
            }
        }

        outStride = bestStride;
        outNames.swap(bestNames);
        outParents.swap(bestParents);
        return true;
    }

    inline bool TryCollectBoneNamesFromDrawState(
        void* drawState,
        std::vector<std::string>& outNames,
        std::vector<int>& outParents,
        int& outNumBones,
        int& outBoneIndex,
        int& outStride,
        int& outNumBonesOffset)
    {
        outNames.clear();
        outParents.clear();
        outNumBones = 0;
        outBoneIndex = 0;
        outStride = 0;
        outNumBonesOffset = 0;

        const uint8_t* studioHdr = nullptr;
        if (!TryGetStudioHdrFromDrawState(drawState, studioHdr))
            return false;

        int id = 0;
        int version = 0;
        int studioLength = 0;
        SafeRead(studioHdr + 0x00, id);
        SafeRead(studioHdr + 0x04, version);
        SafeRead(studioHdr + 0x4C, studioLength);

        static thread_local std::unordered_map<const void*, CachedBoneLayout>
            s_BoneLayoutCache;
        auto cachedIt = s_BoneLayoutCache.find(studioHdr);
        if (cachedIt != s_BoneLayoutCache.end())
        {
            const CachedBoneLayout& cached = cachedIt->second;
            if (cached.id == id &&
                cached.version == version &&
                cached.studioLength == studioLength)
            {
                if (cached.success)
                {
                    outNames = cached.names;
                    outParents = cached.parents;
                    outNumBones = cached.numBones;
                    outBoneIndex = cached.boneIndex;
                    outStride = cached.stride;
                    outNumBonesOffset = cached.numBonesOffset;
                }
                return cached.success;
            }
        }

        CachedBoneLayout fresh;
        fresh.id = id;
        fresh.version = version;
        fresh.studioLength = studioLength;
        fresh.success = TryCollectBoneNamesFromDrawStateUncached(
            drawState,
            studioHdr,
            outNames,
            outParents,
            outNumBones,
            outBoneIndex,
            outStride,
            outNumBonesOffset);
        if (fresh.success)
        {
            fresh.names = outNames;
            fresh.parents = outParents;
            fresh.numBones = outNumBones;
            fresh.boneIndex = outBoneIndex;
            fresh.stride = outStride;
            fresh.numBonesOffset = outNumBonesOffset;
        }

        const bool success = fresh.success;
        s_BoneLayoutCache[studioHdr] = std::move(fresh);
        return success;
    }
}

namespace
{
    std::mutex s_TrackedConVarTraceMutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_TrackedConVarTraceLastLog;

    inline bool ShouldLogMagazineBoxDiagnostics(const VR* vr)
    {
        return vr && (vr->m_MagazineBoxDebugEnabled || vr->m_VrHandsDebugLog);
    }

    C_BaseEntity* HooksSafeGetClientEntity(Game* game, int entityIndex)
    {
        if (!game || !game->m_ClientEntityList || entityIndex <= 0 || entityIndex > 2048)
            return nullptr;
#ifdef _MSC_VER
        __try
        {
            return game->GetClientEntity(entityIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return game->GetClientEntity(entityIndex);
#endif
    }

    const char* HooksSafeGetNetworkClassName(Game* game, C_BaseEntity* entity)
    {
        if (!game || !entity)
            return nullptr;
#ifdef _MSC_VER
        __try
        {
            return game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
#endif
    }

    inline bool ShouldThrottleTrackedConVarTrace(const std::string& key, float maxHz = 5.0f)
    {
        if (key.empty() || maxHz <= 0.0f)
            return false;

        const auto now = std::chrono::steady_clock::now();
        const auto minInterval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(1.0f / maxHz));

        std::lock_guard<std::mutex> lock(s_TrackedConVarTraceMutex);
        auto& last = s_TrackedConVarTraceLastLog[key];
        if (last.time_since_epoch().count() != 0 && now - last < minInterval)
            return true;

        last = now;
        return false;
    }

    inline bool HooksModelNameIsArmsOrHands(const std::string& modelName)
    {
        return
            (modelName.find("models/weapons/arms/") != std::string::npos) ||
            (modelName.find("/arms/") != std::string::npos) ||
            (modelName.find("v_arms") != std::string::npos) ||
            (modelName.find("models/weapons/hands/") != std::string::npos) ||
            (modelName.find("/hands/") != std::string::npos) ||
            (modelName.find("v_hands") != std::string::npos);
    }

    inline bool HooksNativeViewmodelHandsOnlyActive(VR* vr)
    {
        return vr &&
            (vr->m_NativeViewmodelHandsOnly ||
                vr->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire));
    }

    inline bool HooksNativeViewmodelHandsOnlyHideArmsRequested(VR* vr)
    {
        if (!vr)
            return true;
        const bool emptyHandsPlaceholderActive =
            vr->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
        return vr->m_HideArms && !emptyHandsPlaceholderActive;
    }

    inline bool HooksNativeViewmodelEffectiveArmCroppingEnabled(VR* vr);

    inline bool HooksNativeViewmodelFullArmIkActive(VR* vr)
    {
        return HooksNativeViewmodelHandsOnlyActive(vr) &&
            vr->m_FirstPersonControlReady.load(std::memory_order_acquire) &&
            (!vr->m_NativeViewmodelHandsOnly ||
                vr->IsVrHandsTwoHandedGripPoseActive() ||
                vr->m_NativeViewmodelLeftHandFreezeReady.load(
                    std::memory_order_acquire) != 0u) &&
            !HooksNativeViewmodelHandsOnlyHideArmsRequested(vr) &&
            !HooksNativeViewmodelEffectiveArmCroppingEnabled(vr);
    }

    inline bool HooksNativeViewmodelFullArmControllerPairReady(VR* vr)
    {
        if (!vr)
            return false;

        struct ControllerPairReadiness
        {
            VR* owner = nullptr;
            uint32_t generation = 0;
            bool ready = false;
            bool waitingLogged = false;
            bool readyLogged = false;
            std::chrono::steady_clock::time_point bothValidSince{};
        };

        static std::mutex s_readinessMutex;
        static ControllerPairReadiness s_readiness;

        VRWorldPoseTrackingSnapshot tracking{};
        const bool haveTracking =
            vr->ReadWorldPoseTrackingSnapshot(tracking);
        const bool bothControllersValid =
            haveTracking && tracking.leftHandValid && tracking.rightHandValid;
        const uint32_t generation =
            vr->m_NativeViewmodelLeftHandFreezeGeneration.load(
                std::memory_order_acquire);
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kStableDuration = std::chrono::milliseconds(200);

        std::lock_guard<std::mutex> lock(s_readinessMutex);
        if (s_readiness.owner != vr ||
            s_readiness.generation != generation)
        {
            s_readiness = ControllerPairReadiness{};
            s_readiness.owner = vr;
            s_readiness.generation = generation;
        }

        if (!bothControllersValid)
        {
            s_readiness.ready = false;
            s_readiness.readyLogged = false;
            s_readiness.bothValidSince = {};
            if (!s_readiness.waitingLogged)
            {
                Game::logMsg(
                    "[VR][ViewmodelArmIK] waiting for atomic controller pair generation=%u snapshot=%s leftValid=%s rightValid=%s",
                    generation,
                    haveTracking ? "ready" : "missing",
                    tracking.leftHandValid ? "true" : "false",
                    tracking.rightHandValid ? "true" : "false");
                s_readiness.waitingLogged = true;
            }
            return false;
        }

        if (s_readiness.bothValidSince.time_since_epoch().count() == 0)
            s_readiness.bothValidSince = now;
        if (now - s_readiness.bothValidSince < kStableDuration)
            return false;

        s_readiness.ready = true;
        s_readiness.waitingLogged = false;
        if (!s_readiness.readyLogged)
        {
            Game::logMsg(
                "[VR][ViewmodelArmIK] atomic controller pair stable; enabling both arms generation=%u stableMs=%lld",
                generation,
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - s_readiness.bothValidSince).count()));
            s_readiness.readyLogged = true;
        }
        return s_readiness.ready;
    }

    inline bool HooksModelNameIsViewmodel(const std::string& modelName)
    {
        return
            (modelName.find("models/weapons/v_") != std::string::npos) ||
            (modelName.find("/v_models/") != std::string::npos) ||
            (modelName.find("models/v_models/") != std::string::npos) ||

            // L4D2 melee viewmodels often live under models/weapons/melee/...
            (modelName.find("models/weapons/melee/v_") != std::string::npos) ||
            (modelName.find("models/weapons/melee/") != std::string::npos && modelName.find("/v_") != std::string::npos) ||
            (modelName.find("/melee/v_") != std::string::npos) ||

            // Arms/hands are frequently separate models from the gun.
            HooksModelNameIsArmsOrHands(modelName);
    }

    inline bool HooksModelNameHasLooseViewmodelMarker(const std::string& modelName)
    {
        return
            modelName.rfind("v_", 0) == 0 ||
            modelName.find("/v_") != std::string::npos ||
            modelName.find("\\v_") != std::string::npos;
    }

    inline bool HooksCalibrationBoneNamesLookLikeWeapon(const std::vector<std::string>& boneNames)
    {
        bool hasWeaponBone = false;
        bool hasMagazineBone = false;
        bool hasWeaponAttachment = false;

        for (const std::string& name : boneNames)
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(name);
            if (lowerName.find("weapon_bone") != std::string::npos ||
                lowerName.find("valvebiped.weapon") != std::string::npos ||
                lowerName.find("def_weapon") != std::string::npos)
            {
                hasWeaponBone = true;
            }
            if (lowerName.find("weapon_clip") != std::string::npos ||
                lowerName.find("magazine") != std::string::npos ||
                lowerName.find("_mag") != std::string::npos ||
                lowerName.find("clip") != std::string::npos ||
                lowerName.find("def_c_mag") != std::string::npos)
            {
                hasMagazineBone = true;
            }
            if (lowerName.find("attach_muzzle") != std::string::npos ||
                lowerName.find("attach_shell") != std::string::npos ||
                lowerName.find("shell_eject") != std::string::npos)
            {
                hasWeaponAttachment = true;
            }
        }

        return (hasWeaponBone && (hasMagazineBone || hasWeaponAttachment)) ||
            (hasMagazineBone && hasWeaponAttachment);
    }

    inline void MaybeLogVrHandsViewmodelBoneProbe(
        void* drawState,
        const std::string& modelName,
        int entityIndex,
        const char* className,
        const void* customBoneToWorld,
        bool hasCustomBones)
    {
        if (!drawState || modelName.empty())
            return;

        static std::mutex s_probeMutex;
        static std::unordered_map<std::string, bool> s_probeLoggedByModel;
        {
            std::lock_guard<std::mutex> lock(s_probeMutex);
            if (s_probeLoggedByModel.find(modelName) != s_probeLoggedByModel.end())
                return;
            s_probeLoggedByModel.emplace(modelName, true);
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        const bool ok = vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);

        Game::logMsg(
            "[VR][Hands][VMProbe] model=\"%s\" ent=%d class=%s customBones=%d ok=%d bones=%d boneIndex=0x%X numBonesOff=0x%X stride=%d",
            modelName.c_str(),
            entityIndex,
            (className && *className) ? className : "<null>",
            hasCustomBones ? 1 : 0,
            ok ? 1 : 0,
            numBones,
            boneIndex,
            numBonesOffset,
            stride);

        if (!ok)
            return;

        std::string chunk;
        int chunkIndex = 0;
        auto flushChunk = [&]()
            {
                if (chunk.empty())
                    return;
                Game::logMsg("[VR][Hands][VMProbe] bones[%d] model=\"%s\" %s", chunkIndex++, modelName.c_str(), chunk.c_str());
                chunk.clear();
            };

        for (int i = 0; i < numBones && i < static_cast<int>(boneNames.size()); ++i)
        {
            if (boneNames[static_cast<size_t>(i)].empty())
                continue;

            char item[256]{};
            std::snprintf(
                item,
                sizeof(item),
                "%d:p%d:%s; ",
                i,
                (i < static_cast<int>(boneParents.size())) ? boneParents[static_cast<size_t>(i)] : -1,
                boneNames[static_cast<size_t>(i)].c_str());

            if (chunk.size() + std::strlen(item) > 850)
                flushChunk();
            chunk += item;
        }
        flushChunk();

        if (!customBoneToWorld)
            return;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(customBoneToWorld);
        std::string poseChunk;
        int poseChunkIndex = 0;
        auto flushPoseChunk = [&]()
            {
                if (poseChunk.empty())
                    return;
                Game::logMsg("[VR][Hands][VMProbe] bonePose[%d] model=\"%s\" %s", poseChunkIndex++, modelName.c_str(), poseChunk.c_str());
                poseChunk.clear();
            };

        for (int i = 0; i < numBones; ++i)
        {
            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + i, boneWorld))
                continue;

            const Vector origin = vr_vm_stabilize::GetOrigin(boneWorld);
            const char* boneName =
                (i < static_cast<int>(boneNames.size()) && !boneNames[static_cast<size_t>(i)].empty())
                ? boneNames[static_cast<size_t>(i)].c_str()
                : "<unnamed>";

            char item[320]{};
            std::snprintf(
                item,
                sizeof(item),
                "%d:p%d:%s:o(%.1f %.1f %.1f); ",
                i,
                (i < static_cast<int>(boneParents.size())) ? boneParents[static_cast<size_t>(i)] : -1,
                boneName,
                origin.x,
                origin.y,
                origin.z);

            if (poseChunk.size() + std::strlen(item) > 850)
                flushPoseChunk();
            poseChunk += item;
        }
        flushPoseChunk();
    }

    inline bool HooksViewmodelBoneLabelIsFiniteOrigin(const Vector& value)
    {
        return std::isfinite(value.x) &&
            std::isfinite(value.y) &&
            std::isfinite(value.z) &&
            std::fabs(value.x) < 100000.0f &&
            std::fabs(value.y) < 100000.0f &&
            std::fabs(value.z) < 100000.0f;
    }

    inline bool HooksViewmodelBoneLabelIsZeroOrigin(const Vector& value)
    {
        return std::fabs(value.x) < 0.01f &&
            std::fabs(value.y) < 0.01f &&
            std::fabs(value.z) < 0.01f;
    }

    inline float HooksViewmodelBoneLabelMedian(std::vector<float>& values)
    {
        if (values.empty())
            return 0.0f;

        std::sort(values.begin(), values.end());
        const size_t mid = values.size() / 2u;
        if ((values.size() & 1u) != 0u)
            return values[mid];
        return (values[mid - 1u] + values[mid]) * 0.5f;
    }

    inline bool HooksViewmodelBoneLabelHasWeaponToken(const std::string& lower)
    {
        static const char* kTokens[] =
        {
            "weapon",
            "clip",
            "mag",
            "magazine",
            "bolt",
            "slide",
            "charging",
            "charger",
            "handle",
            "trigger",
            "safety",
            "gun",
            "rifle",
            "smg",
            "pistol",
            "shotgun",
            "barrel",
            "stock",
            "receiver",
            "foregrip"
        };

        for (const char* token : kTokens)
        {
            if (lower.find(token) != std::string::npos)
                return true;
        }
        return false;
    }

    inline bool HooksViewmodelBoneLabelNamePassesFilter(const std::string& lower)
    {
        if (lower.empty())
            return false;

        if (HooksViewmodelBoneLabelHasWeaponToken(lower))
            return true;

        static const char* kRejectTokens[] =
        {
            "finger",
            "thumb",
            "upperarm",
            "forearm",
            "wrist",
            "ulna",
            "driven",
            "valvebiped",
            "bip01",
            "l_hand",
            "r_hand",
            "helper",
            "hlp",
            "ik",
            "camera",
            "attach",
            "attachment",
            "muzzle",
            "shell",
            "eject",
            "flash"
        };

        for (const char* token : kRejectTokens)
        {
            if (lower.find(token) != std::string::npos)
                return false;
        }
        return true;
    }

    inline void MaybeDrawViewmodelBoneLabels(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const char* className,
        const void* boneToWorld)
    {
        auto clearLabels = [&]()
            {
                if (!vr)
                    return;
                std::lock_guard<std::mutex> lock(vr->m_ViewmodelBoneLabelMutex);
                vr->m_ViewmodelBoneLabels.clear();
            };

        const bool calibrationOverlayActive =
            vr && vr->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed);
        if (!vr || calibrationOverlayActive || !vr->m_ViewmodelBoneLabelsEnabled)
        {
            clearLabels();
            return;
        }

        if (!drawState || !boneToWorld || modelName.empty())
            return;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        const bool isViewmodelClass = className &&
            (std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0);
        if (!isViewmodelClass && !HooksModelNameIsViewmodel(lowerModel))
            return;
        if (HooksModelNameIsArmsOrHands(lowerModel))
            return;


        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            clearLabels();
            return;
        }
        if (numBones <= 0 || numBones > 512)
        {
            clearLabels();
            return;
        }

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(boneToWorld);
        std::vector<Vector> origins(static_cast<size_t>(numBones), Vector(0.0f, 0.0f, 0.0f));
        std::vector<bool> validOrigins(static_cast<size_t>(numBones), false);
        std::vector<float> xs;
        std::vector<float> ys;
        std::vector<float> zs;
        xs.reserve(static_cast<size_t>(numBones));
        ys.reserve(static_cast<size_t>(numBones));
        zs.reserve(static_cast<size_t>(numBones));

        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
                continue;

            const Vector origin = vr_vm_stabilize::GetOrigin(boneWorld);
            if (!HooksViewmodelBoneLabelIsFiniteOrigin(origin) || HooksViewmodelBoneLabelIsZeroOrigin(origin))
                continue;

            origins[static_cast<size_t>(bone)] = origin;
            validOrigins[static_cast<size_t>(bone)] = true;
            xs.push_back(origin.x);
            ys.push_back(origin.y);
            zs.push_back(origin.z);
        }

        if (xs.empty())
        {
            clearLabels();
            return;
        }

        const Vector clusterCenter(
            HooksViewmodelBoneLabelMedian(xs),
            HooksViewmodelBoneLabelMedian(ys),
            HooksViewmodelBoneLabelMedian(zs));
        constexpr float kMaxClusterDistance = 220.0f;
        constexpr float kMaxClusterDistanceSqr = kMaxClusterDistance * kMaxClusterDistance;
        constexpr int kMaxLabels = 96;
        std::vector<VR::ProjectedViewmodelBoneLabel> projectedLabels;
        projectedLabels.reserve(kMaxLabels);
        int labelsDrawn = 0;
        constexpr int highlightedBone = -1;

        for (int bone = 0; bone < numBones && labelsDrawn < kMaxLabels; ++bone)
        {
            if (!validOrigins[static_cast<size_t>(bone)])
                continue;

            const Vector origin = origins[static_cast<size_t>(bone)];
            if ((origin - clusterCenter).LengthSqr() > kMaxClusterDistanceSqr)
                continue;

            const bool hasName =
                bone < static_cast<int>(boneNames.size()) &&
                !boneNames[static_cast<size_t>(bone)].empty();
            if (hasName)
            {
                const std::string lowerName =
                    vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
                if (bone != highlightedBone && !HooksViewmodelBoneLabelNamePassesFilter(lowerName))
                    continue;
            }

            const int parent =
                (bone < static_cast<int>(boneParents.size()))
                ? boneParents[static_cast<size_t>(bone)]
                : -1;

            char label[192]{};
            std::snprintf(
                label,
                sizeof(label),
                "%s#%d p%d %s",
                bone == highlightedBone ? "> " : "",
                bone,
                parent,
                hasName ? boneNames[static_cast<size_t>(bone)].c_str() : "<unnamed>");

            VR::ProjectedViewmodelBoneLabel projected{};
            projected.worldPos = Vector(origin.x, origin.y, origin.z + 2.8f);
            projected.label = label;
            projected.hasName = hasName;
            projected.highlighted = (bone == highlightedBone);
            projectedLabels.push_back(std::move(projected));
            ++labelsDrawn;
        }

        {
            std::lock_guard<std::mutex> lock(vr->m_ViewmodelBoneLabelMutex);
            vr->m_ViewmodelBoneLabels = std::move(projectedLabels);
        }
    }

    inline bool MuzzleNameEndsWithToken(const std::string& lower, const char* token)
    {
        const size_t len = std::strlen(token);
        if (lower.size() < len)
            return false;
        if (lower.compare(lower.size() - len, len, token) != 0)
            return false;
        if (lower.size() == len)
            return true;
        const char prev = lower[lower.size() - len - 1];
        return prev == '.' || prev == '_' || prev == ':' || prev == '/' || prev == '\\';
    }

    inline int MuzzlePointNameScore(const std::string& lower)
    {
        if (lower.empty())
            return 0;

        if (lower == "attach_muzzle" || MuzzleNameEndsWithToken(lower, "attach_muzzle"))
            return 120;
        if (lower == "muzzle" || MuzzleNameEndsWithToken(lower, "muzzle"))
            return 110;
        if (lower == "muzzlesmoke" || lower == "muzzle_smoke" || lower == "muzzle smoke" ||
            lower.find("muzzlesmoke") != std::string::npos ||
            lower.find("muzzle_smoke") != std::string::npos)
        {
            return 100;
        }
        if (lower == "flash" || MuzzleNameEndsWithToken(lower, "flash"))
            return 90;
        if (lower.find("muzzle") != std::string::npos &&
            lower.find("shell") == std::string::npos &&
            lower.find("eject") == std::string::npos)
        {
            return 70;
        }
        return 0;
    }

    inline Vector MuzzleMatrixColumn(const vr_vm_stabilize::Mat3x4& matrix, int column)
    {
        return Vector(matrix.m[0][column], matrix.m[1][column], matrix.m[2][column]);
    }

    inline bool BuildMuzzleAnglesFromMatrix(
        const ModelRenderInfo_t& drawInfo,
        const vr_vm_stabilize::Mat3x4& matrix,
        QAngle& outAngles)
    {
        Vector referenceForward{};
        QAngle::AngleVectors(drawInfo.angles, &referenceForward, nullptr, nullptr);
        if (!referenceForward.IsZero())
            VectorNormalize(referenceForward);

        Vector bestForward{};
        float bestDot = -FLT_MAX;
        for (int column = 0; column < 3; ++column)
        {
            Vector axis = MuzzleMatrixColumn(matrix, column);
            if (axis.IsZero())
                continue;
            VectorNormalize(axis);

            const Vector candidates[2] = { axis, axis * -1.0f };
            for (const Vector& candidate : candidates)
            {
                float score = 0.0f;
                if (!referenceForward.IsZero())
                    score = DotProduct(candidate, referenceForward);
                else if (column == 0)
                    score = 0.5f;

                if (score > bestDot)
                {
                    bestDot = score;
                    bestForward = candidate;
                }
            }
        }

        if (bestForward.IsZero())
            return false;

        if (!referenceForward.IsZero() && bestDot < 0.8660254f)
            QAngle::AngleVectors(drawInfo.angles, &bestForward, nullptr, nullptr);

        QAngle::VectorAngles(bestForward, outAngles);
        NormalizeAndClampViewAngles(outAngles);
        return std::isfinite(outAngles.x) && std::isfinite(outAngles.y) && std::isfinite(outAngles.z);
    }

    struct MuzzleSmokeAttachmentInfo
    {
        bool parsed = false;
        bool found = false;
        int attachment = -1;
        int localBone = -1;
        int numAttachments = 0;
        int attachmentIndex = 0;
        std::string name;
        vr_vm_stabilize::Mat3x4 local{};
    };

    inline bool TryResolveMuzzleSmokeAttachment(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        int numBones,
        MuzzleSmokeAttachmentInfo& outInfo)
    {
        outInfo = {};
        if (!drawState || modelName.empty() || numBones <= 0)
            return false;

        const std::string key = vr_vm_stabilize::ToLowerAscii(modelName);
        static std::mutex s_mutex;
        static std::unordered_map<std::string, MuzzleSmokeAttachmentInfo> s_cachedByModel;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_cachedByModel.find(key);
            if (it != s_cachedByModel.end())
            {
                outInfo = it->second;
                return outInfo.parsed;
            }
        }

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr))
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        int numAttachments = 0;
        int attachmentIndex = 0;
        if (!vr_vm_stabilize::SafeRead(studioHdr + 0xF0, numAttachments) ||
            !vr_vm_stabilize::SafeRead(studioHdr + 0xF4, attachmentIndex))
        {
            return false;
        }
        if (numAttachments <= 0 || numAttachments > 256 || attachmentIndex <= 0 || attachmentIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (attachmentIndex >= studioLength ||
                static_cast<size_t>(attachmentIndex) + static_cast<size_t>(numAttachments) * 92u > static_cast<size_t>(studioLength)))
        {
            return false;
        }

        outInfo.parsed = true;
        outInfo.numAttachments = numAttachments;
        outInfo.attachmentIndex = attachmentIndex;

        static constexpr int kAttachmentStride = 92; // mstudioattachment_t in L4D2: name, flags, localbone, matrix3x4, unused[8].
        for (int attachment = 0; attachment < numAttachments; ++attachment)
        {
            const size_t attachmentOffset =
                static_cast<size_t>(attachmentIndex) + static_cast<size_t>(attachment) * kAttachmentStride;
            const uint8_t* attachmentBase = studioHdr + attachmentOffset;

            int nameOffset = 0;
            int localBone = -1;
            if (!vr_vm_stabilize::SafeRead(attachmentBase + 0, nameOffset) ||
                !vr_vm_stabilize::SafeRead(attachmentBase + 8, localBone))
            {
                continue;
            }
            if (nameOffset <= 0 || localBone < 0 || localBone >= numBones)
                continue;

            const size_t nameAddressOffset = attachmentOffset + static_cast<size_t>(nameOffset);
            if (studioLength > 0 && nameAddressOffset >= static_cast<size_t>(studioLength))
                continue;

            std::string name;
            if (!vr_vm_stabilize::TryReadCStringSafe(reinterpret_cast<const char*>(studioHdr + nameAddressOffset), name))
                continue;

            const int score = MuzzlePointNameScore(vr_vm_stabilize::ToLowerAscii(name));
            if (score <= 0)
                continue;

            vr_vm_stabilize::Mat3x4 local{};
            if (!vr_vm_stabilize::SafeRead(attachmentBase + 12, local))
                continue;

            const int oldScore = MuzzlePointNameScore(vr_vm_stabilize::ToLowerAscii(outInfo.name));
            if (!outInfo.found || score > oldScore)
            {
                outInfo.found = true;
                outInfo.attachment = attachment;
                outInfo.localBone = localBone;
                outInfo.name = name;
                outInfo.local = local;
            }
        }

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_cachedByModel[key] = outInfo;
        }

        if (vr && vr->m_BulletVisualsUseMuzzleSmoke)
        {
            if (outInfo.found)
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] attachment model=%s attachment=%d name=%s localBone=%d attachments=%d table=0x%X",
                    modelName.c_str(),
                    outInfo.attachment,
                    outInfo.name.c_str(),
                    outInfo.localBone,
                    outInfo.numAttachments,
                    outInfo.attachmentIndex);
            }
            else
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] no attachment model=%s attachments=%d table=0x%X",
                    modelName.c_str(),
                    outInfo.numAttachments,
                    outInfo.attachmentIndex);
            }
        }

        return outInfo.parsed;
    }

    inline int ResolveMuzzleSmokeBoneIndex(
        VR* vr,
        void* drawState,
        const std::string& modelName)
    {
        if (!drawState || modelName.empty())
            return -1;

        const std::string key = vr_vm_stabilize::ToLowerAscii(modelName);
        static std::mutex s_mutex;
        static std::unordered_map<std::string, int> s_cachedBoneByModel;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_cachedBoneByModel.find(key);
            if (it != s_cachedBoneByModel.end())
                return it->second;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        const bool ok = vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);

        int resolved = -1;
        int resolvedScore = 0;
        if (ok)
        {
            for (int bone = 0; bone < numBones && bone < static_cast<int>(boneNames.size()); ++bone)
            {
                const std::string lower = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
                const int score = MuzzlePointNameScore(lower);
                if (score > resolvedScore)
                {
                    resolved = bone;
                    resolvedScore = score;
                }
            }
        }

        if (ok)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_cachedBoneByModel[key] = resolved;
        }

        if (vr && vr->m_BulletVisualsUseMuzzleSmoke)
        {
            if (resolved >= 0)
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] bone model=%s bone=%d name=%s score=%d bones=%d stride=%d",
                    modelName.c_str(),
                    resolved,
                    (resolved < static_cast<int>(boneNames.size())) ? boneNames[static_cast<size_t>(resolved)].c_str() : "<unknown>",
                    resolvedScore,
                    numBones,
                    stride);
            }
            else
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] not found model=%s ok=%d bones=%d stride=%d",
                    modelName.c_str(),
                    ok ? 1 : 0,
                    numBones,
                    stride);
            }
        }
        return resolved;
    }

    inline void PublishViewmodelMuzzleSmokePose(
        VR* vr,
        const Vector& origin,
        const QAngle& angles)
    {
        if (!vr)
            return;

        uint32_t seq = vr->m_ViewmodelMuzzleSmokePoseSeq.load(std::memory_order_relaxed);
        if (seq & 1u)
            ++seq;
        const uint32_t odd = seq + 1u;
        const uint32_t even = odd + 1u;

        vr->m_ViewmodelMuzzleSmokePoseSeq.store(odd, std::memory_order_release);
        vr->m_ViewmodelMuzzleSmokePosX.store(origin.x, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePosY.store(origin.y, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePosZ.store(origin.z, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeAngX.store(angles.x, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeAngY.store(angles.y, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeAngZ.store(angles.z, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePoseTickMs.store(GetTickCount(), std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeRenderFrameSeq.store(vr->m_RenderFrameSeq.load(std::memory_order_relaxed), std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePoseSeq.store(even, std::memory_order_release);
    }

    inline void MaybeCaptureViewmodelMuzzleSmokePose(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& drawInfo,
        const void* pBonesToWorldFinal)
    {
        if (!vr || !vr->m_IsVREnabled || !vr->m_BulletVisualsUseMuzzleSmoke || !pBonesToWorldFinal)
            return;
        if (modelName.empty() || !HooksModelNameIsViewmodel(modelName) || HooksModelNameIsArmsOrHands(modelName))
            return;

        int numBones = 0;
        if (!vr_vm_stabilize::TryGetNumBonesFromDrawState(drawState, numBones) || numBones <= 0)
            return;

        const auto* bones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pBonesToWorldFinal);

        vr_vm_stabilize::Mat3x4 muzzle{};
        const char* poseSource = "bone";
        const int muzzleBone = ResolveMuzzleSmokeBoneIndex(vr, drawState, modelName);
        if (muzzleBone >= 0 && muzzleBone < numBones)
        {
            if (!vr_vm_stabilize::SafeRead(bones + muzzleBone, muzzle))
                return;
            poseSource = "bone";
        }
        else
        {
            MuzzleSmokeAttachmentInfo attachmentInfo{};
            if (!TryResolveMuzzleSmokeAttachment(vr, drawState, modelName, numBones, attachmentInfo) ||
                !attachmentInfo.found ||
                attachmentInfo.localBone < 0 ||
                attachmentInfo.localBone >= numBones)
            {
                return;
            }

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(bones + attachmentInfo.localBone, boneWorld))
                return;
            vr_vm_stabilize::Mul(boneWorld, attachmentInfo.local, muzzle);
            poseSource = "attachment";
        }

        Vector origin = vr_vm_stabilize::GetOrigin(muzzle);
        QAngle angles{};
        if (!BuildMuzzleAnglesFromMatrix(drawInfo, muzzle, angles))
            return;
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
            !std::isfinite(angles.x) || !std::isfinite(angles.y) || !std::isfinite(angles.z))
        {
            return;
        }

        PublishViewmodelMuzzleSmokePose(vr, origin, angles);

        static std::mutex s_captureLogMutex;
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastCaptureLogByModel;
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(s_captureLogMutex);
            auto& last = s_lastCaptureLogByModel[modelName];
            if (last.time_since_epoch().count() == 0 || now - last > std::chrono::seconds(2))
            {
                last = now;
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] capture source=%s model=%s origin=(%.2f %.2f %.2f) angles=(%.2f %.2f %.2f)",
                    poseSource,
                    modelName.c_str(),
                    origin.x, origin.y, origin.z,
                    angles.x, angles.y, angles.z);
            }
        }
    }

    inline VrHandMatrix4 HooksMat3x4ToVrHandMatrix(const vr_vm_stabilize::Mat3x4& source)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 4; ++column)
                VrHandMath::Set(out, row, column, source.m[row][column]);
        }
        return out;
    }

    inline vr_vm_stabilize::Mat3x4 HooksVrHandMatrixToMat3x4(const VrHandMatrix4& source)
    {
        vr_vm_stabilize::Mat3x4 out{};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 4; ++column)
                out.m[row][column] = VrHandMath::Get(source, row, column);
        }
        return out;
    }

    inline VrHandMatrix4 HooksStripVrHandMatrixScale(const VrHandMatrix4& source)
    {
        VrHandMatrix4 out = source;
        for (int column = 0; column < 3; ++column)
        {
            Vector axis(
                VrHandMath::Get(out, 0, column),
                VrHandMath::Get(out, 1, column),
                VrHandMath::Get(out, 2, column));
            const float length = axis.Length();
            if (!(length > 0.000001f))
                continue;
            axis *= (1.0f / length);
            VrHandMath::Set(out, 0, column, axis.x);
            VrHandMath::Set(out, 1, column, axis.y);
            VrHandMath::Set(out, 2, column, axis.z);
        }
        return out;
    }

    inline bool MagazineInteractionNameContains(const std::string& value, const char* needle)
    {
        return needle && *needle && value.find(needle) != std::string::npos;
    }

    inline bool MagazineInteractionNameEndsWith(const std::string& value, const char* suffix)
    {
        if (!suffix || !*suffix)
            return false;
        const size_t suffixLength = std::strlen(suffix);
        return value.size() >= suffixLength &&
            value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
    }

    inline bool MagazineInteractionNameHasLooseMagToken(const std::string& name)
    {
        size_t pos = 0;
        while ((pos = name.find("mag", pos)) != std::string::npos)
        {
            const bool prefixOk =
                pos == 0 ||
                name[pos - 1] == '_' ||
                name[pos - 1] == '.' ||
                name[pos - 1] == '-' ||
                name[pos - 1] == ':';
            const size_t after = pos + 3;
            const bool suffixOk =
                after >= name.size() ||
                name[after] == '_' ||
                name[after] == '.' ||
                name[after] == '-' ||
                std::isdigit(static_cast<unsigned char>(name[after])) != 0;
            if (prefixOk && suffixOk)
                return true;
            pos = after;
        }
        return false;
    }

    inline bool MagazineInteractionNameIsLegacyValveBipedClip(const std::string& name)
    {
        return name == "valvebiped.weapon_clip" ||
            name == "valvebiped.weapon_magazine" ||
            name == "weapon_clip" ||
            name == "weapon_magazine";
    }

    inline int ScoreMagazineInteractionMagazineBoneName(const std::string& rawName)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return 0;

        const bool hasClip = MagazineInteractionNameContains(name, "clip");
        const bool hasMagazine = MagazineInteractionNameContains(name, "magazine");
        const bool hasLooseMag = MagazineInteractionNameHasLooseMagToken(name);

        // Ignore controls, helpers and visual children. We need the root bone that
        // moves the whole detachable magazine.
        if (MagazineInteractionNameContains(name, "release") ||
            MagazineInteractionNameContains(name, "realease") ||
            MagazineInteractionNameContains(name, "button") ||
            MagazineInteractionNameContains(name, "trigger") ||
            MagazineInteractionNameContains(name, "bullet") ||
            MagazineInteractionNameContains(name, "round") ||
            MagazineInteractionNameContains(name, "shell") ||
            (MagazineInteractionNameContains(name, "ammo") && !hasClip && !hasMagazine && !hasLooseMag) ||
            MagazineInteractionNameContains(name, "helper") ||
            MagazineInteractionNameContains(name, "attach"))
        {
            return 0;
        }

        if (!hasClip && !hasMagazine && !hasLooseMag)
            return 0;

        // Replacement viewmodels frequently retain ValveBiped.weapon_clip as a
        // compatibility helper while the visible magazine mesh is weighted to a
        // custom bone such as Magazine_Main, Magazine or j_mag1. Keep the legacy
        // helper as a fallback, but never let it beat an explicit custom bone.
        if (MagazineInteractionNameIsLegacyValveBipedClip(name))
            return 500;

        int score = 200;
        if (name.rfind("v_weapon.", 0) == 0 || name.rfind("v_weapon_", 0) == 0)
            score += 1600;
        else if (MagazineInteractionNameContains(name, "v_weapon"))
            score += 1300;
        else if (MagazineInteractionNameContains(name, "weapon"))
            score += 650;

        if (MagazineInteractionNameContains(name, "magazine_main") ||
            MagazineInteractionNameContains(name, "magazine.main") ||
            MagazineInteractionNameContains(name, "magazine-main"))
        {
            score += 1350;
        }
        else if (hasMagazine)
        {
            score += 1000;
        }
        else if (hasLooseMag)
        {
            score += 900;
        }
        else if (hasClip)
        {
            score += 700;
        }

        if (MagazineInteractionNameEndsWith(name, "_clip") || MagazineInteractionNameEndsWith(name, ".clip"))
            score += 300;
        else if (MagazineInteractionNameEndsWith(name, "_magazine") || MagazineInteractionNameEndsWith(name, ".magazine"))
            score += 280;

        return score;
    }

    inline int FindMagazineInteractionMagazineBone(
        const std::string& modelName,
        const std::vector<std::string>& boneNames)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreMagazineInteractionMagazineBoneName(boneNames[static_cast<size_t>(bone)]);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0)
        {
            static std::mutex s_logMutex;
            static std::unordered_map<std::string, bool> s_loggedModels;
            std::lock_guard<std::mutex> lock(s_logMutex);
            if (s_loggedModels.emplace(modelName, true).second)
            {
                Game::logMsg(
                    "[VR][MagazineInteraction] name candidate model=%s bone=%d name=%s score=%d",
                    modelName.c_str(),
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline Vector HooksTransformPoint(const vr_vm_stabilize::Mat3x4& matrix, const Vector& value)
    {
        return Vector(
            matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z + matrix.m[0][3],
            matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z + matrix.m[1][3],
            matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z + matrix.m[2][3]);
    }

    inline Vector HooksTransformVector(const vr_vm_stabilize::Mat3x4& matrix, const Vector& value)
    {
        return Vector(
            matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z,
            matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z,
            matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z);
    }

    inline Vector HooksInverseTransformPoint(const vr_vm_stabilize::Mat3x4& matrix, const Vector& value)
    {
        const Vector delta(
            value.x - matrix.m[0][3],
            value.y - matrix.m[1][3],
            value.z - matrix.m[2][3]);
        return Vector(
            delta.x * matrix.m[0][0] + delta.y * matrix.m[1][0] + delta.z * matrix.m[2][0],
            delta.x * matrix.m[0][1] + delta.y * matrix.m[1][1] + delta.z * matrix.m[2][1],
            delta.x * matrix.m[0][2] + delta.y * matrix.m[1][2] + delta.z * matrix.m[2][2]);
    }

    inline int FindMagazineBoxBone(const std::vector<std::string>& boneNames)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreMagazineInteractionMagazineBoneName(boneNames[static_cast<size_t>(bone)]);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }
        return bestBone;
    }

    inline bool MagazineInteractionWeaponIdIsHandgun(int weaponId)
    {
        return weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL) ||
            weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::MAGNUM);
    }

    inline int MagazineInteractionInferWeaponIdFromViewmodelModelName(const std::string& lowerModel)
    {
        if (lowerModel.empty())
            return 0;

        auto has = [&](const char* token) -> bool
            {
                return token && *token && lowerModel.find(token) != std::string::npos;
            };

        if (has("shotgun_chrome") || has("chrome_shotgun"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SHOTGUN_CHROME);
        if (has("pumpshotgun") || has("pump_shotgun"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::PUMPSHOTGUN);
        if (has("autoshotgun") || has("auto_shotgun"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::AUTOSHOTGUN);
        if (has("shotgun_spas") || has("v_shotgun_spas") || has("spas"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SPAS);

        if (has("desert_eagle") || has("pistol_magnum"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::MAGNUM);
        if (has("dual_pistol") || has("dual_pistola") ||
            has("v_pistol.mdl") || has("v_pistola.mdl") || has("v_pistolb.mdl"))
        {
            return static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL);
        }

        if (has("smg_mp5") || has("mp5"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::MP5);
        if (has("silenced_smg") || has("smg_silenced"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::MAC10);
        if (has("v_smg.mdl") || has("smg_uzi"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::UZI);

        if (has("rifle_ak47") || has("ak47"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::AK47);
        if (has("desert_rifle") || has("rifle_desert"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SCAR);
        if (has("sg552"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SG552);
        if (has("v_rifle.mdl"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::M16A1);

        if (has("sniper_military") || has("military_sniper"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SNIPER_MILITARY);
        if (has("huntingrifle") || has("hunting_rifle"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::HUNTING_RIFLE);
        if (has("snip_awp") || has("awp"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::AWP);
        if (has("snip_scout") || has("scout"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SCOUT);

        if (has("m60") || has("machinegun_m60"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::M60);

        return 0;
    }

    inline int ScoreMagazineInteractionShotgunShellBoneName(const std::string& rawName)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return 0;

        if (MagazineInteractionNameContains(name, "finger") ||
            MagazineInteractionNameContains(name, "hand") ||
            MagazineInteractionNameContains(name, "bip01") ||
            MagazineInteractionNameContains(name, "attach") ||
            MagazineInteractionNameContains(name, "muzzle") ||
            MagazineInteractionNameContains(name, "eject") ||
            MagazineInteractionNameContains(name, "release") ||
            MagazineInteractionNameContains(name, "realease") ||
            MagazineInteractionNameContains(name, "magrel") ||
            MagazineInteractionNameContains(name, "button") ||
            MagazineInteractionNameContains(name, "trigger") ||
            MagazineInteractionNameContains(name, "safety") ||
            MagazineInteractionNameContains(name, "bolt") ||
            MagazineInteractionNameContains(name, "slide") ||
            MagazineInteractionNameContains(name, "charger") ||
            MagazineInteractionNameContains(name, "handle") ||
            MagazineInteractionNameContains(name, "barrel") ||
            MagazineInteractionNameContains(name, "stock") ||
            MagazineInteractionNameContains(name, "hammer"))
        {
            return 0;
        }

        const bool hasWeapon = MagazineInteractionNameContains(name, "weapon");
        const bool hasClip = MagazineInteractionNameContains(name, "clip");
        const bool hasBullet = MagazineInteractionNameContains(name, "bullet");
        const bool hasRound = MagazineInteractionNameContains(name, "round");
        const bool hasShell = MagazineInteractionNameContains(name, "shell");
        const bool hasAmmo = MagazineInteractionNameContains(name, "ammo");
        if (!hasClip && !hasBullet && !hasRound && !hasShell && !hasAmmo)
            return 0;

        int score = 300;
        if (name == "valvebiped.weapon_clip_bullets" ||
            name == "weapon_clip_bullets")
        {
            score += 3200;
        }
        else if (name == "valvebiped.weapon_clip" ||
            name == "weapon_clip")
        {
            score += 3000;
        }
        else if (MagazineInteractionNameContains(name, "weapon_clip"))
        {
            score += 2600;
        }
        else if (hasClip)
        {
            score += 1800;
        }

        if (hasWeapon)
            score += 550;
        if (hasBullet || hasRound || hasShell)
            score += 350;
        if (hasAmmo)
            score += 120;
        return score;
    }

    inline int FindMagazineInteractionShotgunShellBone(
        const std::string& modelName,
        const std::vector<std::string>& boneNames,
        bool logDiagnostics)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreMagazineInteractionShotgunShellBoneName(boneNames[static_cast<size_t>(bone)]);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0 && logDiagnostics)
        {
            static std::mutex s_logMutex;
            static std::unordered_map<std::string, int> s_loggedBoneByModel;
            std::lock_guard<std::mutex> lock(s_logMutex);
            auto it = s_loggedBoneByModel.find(modelName);
            if (it == s_loggedBoneByModel.end() || it->second != bestBone)
            {
                s_loggedBoneByModel[modelName] = bestBone;
                Game::logMsg(
                    "[VR][MagazineBox] shotgun shell bone candidate model=%s bone=%d name=%s score=%d",
                    modelName.c_str(),
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline bool MagazineInteractionShotgunShellBoneUsesStockProfileAxes(const std::string& lowerName)
    {
        return lowerName == "valvebiped.weapon_clip_bullets" ||
            lowerName == "weapon_clip_bullets" ||
            lowerName == "valvebiped.weapon_clip" ||
            lowerName == "weapon_clip" ||
            MagazineInteractionNameContains(lowerName, "weapon_clip");
    }

    inline int FindMagazineInteractionShotgunStableAnchorBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int shellBone)
    {
        const int numBones = static_cast<int>(boneNames.size());
        if (numBones <= 0)
            return -1;

        auto isExactBone = [&](int bone, const char* a, const char* b) -> bool
            {
                if (bone < 0 || bone >= numBones)
                    return false;
                const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
                return lowerName == a || lowerName == b;
            };

        auto findExact = [&](const char* a, const char* b) -> int
            {
                for (int bone = 0; bone < numBones; ++bone)
                {
                    if (isExactBone(bone, a, b))
                        return bone;
                }
                return -1;
            };

        // The shotgun shell/clip bones can be animated by the native reload/pump layers.
        // Use the main weapon bone as the stable anchor for cached capture offsets.
        int anchor = findExact("valvebiped.weapon_bone", "weapon_bone");
        if (anchor >= 0)
            return anchor;

        if (shellBone >= 0 && shellBone < static_cast<int>(boneParents.size()))
        {
            int current = shellBone;
            for (int guard = 0; guard < static_cast<int>(boneParents.size()); ++guard)
            {
                const int parent = boneParents[static_cast<size_t>(current)];
                if (parent < 0 || parent >= static_cast<int>(boneParents.size()) || parent == current)
                    break;
                if (isExactBone(parent, "valvebiped.weapon_bone", "weapon_bone"))
                    return parent;
                current = parent;
            }
        }

        anchor = findExact("valvebiped.weapon_clip", "weapon_clip");
        if (anchor >= 0)
            return anchor;

        return -1;
    }

    inline int FindMagazineBoxLegacyClipBone(const std::vector<std::string>& boneNames)
    {
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            if (MagazineInteractionNameIsLegacyValveBipedClip(lowerName))
                return bone;
        }
        return -1;
    }

    inline bool MagazineBoxBoneCanProvideParentBasis(const std::string& lowerName)
    {
        if (lowerName.empty())
            return false;

        if (MagazineInteractionNameContains(lowerName, "finger") ||
            MagazineInteractionNameContains(lowerName, "hand") ||
            MagazineInteractionNameContains(lowerName, "bip01") ||
            MagazineInteractionNameContains(lowerName, "wrist") ||
            MagazineInteractionNameContains(lowerName, "forearm") ||
            MagazineInteractionNameContains(lowerName, "upperarm") ||
            MagazineInteractionNameContains(lowerName, "clavicle") ||
            MagazineInteractionNameContains(lowerName, "spine") ||
            MagazineInteractionNameContains(lowerName, "camera") ||
            MagazineInteractionNameContains(lowerName, "attach") ||
            MagazineInteractionNameContains(lowerName, "muzzle") ||
            MagazineInteractionNameContains(lowerName, "shell") ||
            MagazineInteractionNameContains(lowerName, "trigger") ||
            MagazineInteractionNameContains(lowerName, "release") ||
            MagazineInteractionNameContains(lowerName, "realease") ||
            MagazineInteractionNameContains(lowerName, "safety") ||
            MagazineInteractionNameContains(lowerName, "bolt") ||
            MagazineInteractionNameContains(lowerName, "slide") ||
            MagazineInteractionNameContains(lowerName, "charger") ||
            MagazineInteractionNameContains(lowerName, "hammer") ||
            MagazineInteractionNameContains(lowerName, "clip") ||
            MagazineInteractionNameContains(lowerName, "magazine") ||
            MagazineInteractionNameHasLooseMagToken(lowerName))
        {
            return false;
        }

        return true;
    }

    inline bool MagazineBoxBoneCanProvideOffsetAnchor(const std::string& lowerName)
    {
        if (lowerName.empty())
            return false;

        if (MagazineInteractionNameContains(lowerName, "finger") ||
            MagazineInteractionNameContains(lowerName, "hand") ||
            MagazineInteractionNameContains(lowerName, "bip01") ||
            MagazineInteractionNameContains(lowerName, "wrist") ||
            MagazineInteractionNameContains(lowerName, "forearm") ||
            MagazineInteractionNameContains(lowerName, "upperarm") ||
            MagazineInteractionNameContains(lowerName, "clavicle") ||
            MagazineInteractionNameContains(lowerName, "spine") ||
            MagazineInteractionNameContains(lowerName, "camera") ||
            MagazineInteractionNameContains(lowerName, "attach") ||
            MagazineInteractionNameContains(lowerName, "muzzle") ||
            MagazineInteractionNameContains(lowerName, "shell") ||
            MagazineInteractionNameContains(lowerName, "trigger") ||
            MagazineInteractionNameContains(lowerName, "release") ||
            MagazineInteractionNameContains(lowerName, "realease") ||
            MagazineInteractionNameContains(lowerName, "safety") ||
            MagazineInteractionNameContains(lowerName, "bolt") ||
            MagazineInteractionNameContains(lowerName, "slide") ||
            MagazineInteractionNameContains(lowerName, "charger") ||
            MagazineInteractionNameContains(lowerName, "hammer"))
        {
            return false;
        }

        return true;
    }

    inline int FindMagazineBoxDirectOffsetAnchorBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int magazineBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (magazineBone < 0 || magazineBone >= numBones)
            return -1;

        const int parent = boneParents[static_cast<size_t>(magazineBone)];
        if (parent < 0 || parent >= numBones || parent == magazineBone)
            return -1;

        const std::string lowerName =
            (parent < static_cast<int>(boneNames.size()))
            ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(parent)])
            : std::string();
        return MagazineBoxBoneCanProvideOffsetAnchor(lowerName) ? parent : -1;
    }

    inline int FindMagazineBoxParentBasisBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int magazineBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (magazineBone < 0 || magazineBone >= numBones)
            return -1;

        int current = magazineBone;
        for (int guard = 0; guard < numBones; ++guard)
        {
            const int parent = boneParents[static_cast<size_t>(current)];
            if (parent < 0 || parent >= numBones || parent == current)
                break;

            const std::string lowerName =
                (parent < static_cast<int>(boneNames.size()))
                ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(parent)])
                : std::string();
            if (MagazineBoxBoneCanProvideParentBasis(lowerName))
                return parent;

            current = parent;
        }
        return -1;
    }

    inline int ScoreMagazineInteractionBoltBoneName(const std::string& rawName, int weaponId)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return 0;

        if (MagazineInteractionWeaponIdIsShotgun(weaponId) &&
            (name == "weapon_charger_slide" ||
                name == "valvebiped.weapon_charger_slide"))
        {
            return 5600;
        }

        if (MagazineInteractionWeaponIdIsHandgun(weaponId) &&
            (name == "weapon_charger_slide" ||
                name == "valvebiped.weapon_charger_slide" ||
                name == "weapon_slide" ||
                name == "valvebiped.weapon_slide" ||
                name == "v_weapon.slide" ||
                name == "v_weapon_slide" ||
                name == "slide"))
        {
            return 5600;
        }

        if (MagazineInteractionWeaponIdIsHandgun(weaponId) &&
            MagazineInteractionNameContains(name, "slide") &&
            (MagazineInteractionNameContains(name, "weapon") || name == "slide"))
        {
            return 5200;
        }

        if (MagazineInteractionNameContains(name, "finger") ||
            MagazineInteractionNameContains(name, "hand") ||
            MagazineInteractionNameContains(name, "bip01") ||
            MagazineInteractionNameContains(name, "attach") ||
            MagazineInteractionNameContains(name, "muzzle") ||
            MagazineInteractionNameContains(name, "eject") ||
            MagazineInteractionNameContains(name, "release") ||
            MagazineInteractionNameContains(name, "realease") ||
            MagazineInteractionNameContains(name, "magrel") ||
            MagazineInteractionNameContains(name, "button") ||
            MagazineInteractionNameContains(name, "trigger") ||
            MagazineInteractionNameContains(name, "safety") ||
            MagazineInteractionNameContains(name, "barrel") ||
            MagazineInteractionNameContains(name, "stock") ||
            MagazineInteractionNameContains(name, "hammer") ||
            MagazineInteractionNameContains(name, "bullet") ||
            MagazineInteractionNameContains(name, "round") ||
            MagazineInteractionNameContains(name, "shell") ||
            MagazineInteractionNameContains(name, "ammo") ||
            MagazineInteractionNameContains(name, "clip") ||
            MagazineInteractionNameContains(name, "magazine") ||
            MagazineInteractionNameHasLooseMagToken(name))
        {
            return 0;
        }

        int score = 0;
        if (MagazineInteractionNameContains(name, "bolt_handle") ||
            MagazineInteractionNameContains(name, "bolt.handle") ||
            MagazineInteractionNameContains(name, "bolt-handle"))
        {
            score += 3200;
        }
        if (MagazineInteractionNameContains(name, "bolt"))
            score += 3000;
        if (MagazineInteractionNameEndsWith(name, "_bolt") || MagazineInteractionNameEndsWith(name, ".bolt"))
            score += 650;
        if (MagazineInteractionNameContains(name, "charging_handle") ||
            MagazineInteractionNameContains(name, "charging.handle") ||
            MagazineInteractionNameContains(name, "charging-handle") ||
            MagazineInteractionNameContains(name, "charge_handle") ||
            MagazineInteractionNameContains(name, "charge.handle") ||
            MagazineInteractionNameContains(name, "charge-handle"))
        {
            score += 1600;
        }
        if (MagazineInteractionNameContains(name, "slide"))
            score += 1100;
        if (MagazineInteractionNameContains(name, "charging") ||
            MagazineInteractionNameContains(name, "charger") ||
            MagazineInteractionNameContains(name, "charge"))
        {
            score += 850;
        }
        if (MagazineInteractionNameContains(name, "handle"))
            score += (score > 0) ? 350 : 250;
        if (MagazineInteractionNameContains(name, "cock"))
            score += 650;

        if (score <= 0)
            return 0;

        if (name.rfind("v_weapon.", 0) == 0 || name.rfind("v_weapon_", 0) == 0)
            score += 600;
        else if (MagazineInteractionNameContains(name, "weapon"))
            score += 250;

        if (MagazineInteractionNameEndsWith(name, "_slide") || MagazineInteractionNameEndsWith(name, ".slide"))
            score += 220;
        if (MagazineInteractionNameEndsWith(name, "_handle") || MagazineInteractionNameEndsWith(name, ".handle"))
            score += 120;

        return score;
    }

    inline int FindMagazineInteractionBoltBone(
        int weaponId,
        const std::string& modelName,
        const std::vector<std::string>& boneNames,
        bool logDiagnostics)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreMagazineInteractionBoltBoneName(
                boneNames[static_cast<size_t>(bone)],
                weaponId);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0 && logDiagnostics)
        {
            static std::mutex s_logMutex;
            static std::unordered_map<std::string, int> s_loggedBoneByModel;
            std::lock_guard<std::mutex> lock(s_logMutex);
            auto it = s_loggedBoneByModel.find(modelName);
            if (it == s_loggedBoneByModel.end() || it->second != bestBone)
            {
                s_loggedBoneByModel[modelName] = bestBone;
                Game::logMsg(
                    "[VR][MagazineBolt] name candidate model=%s weaponId=%d bone=%d name=%s score=%d",
                    modelName.c_str(),
                    weaponId,
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline uint32_t HooksFnv1aUpdate(uint32_t hash, const void* data, size_t bytes)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
        {
            hash ^= static_cast<uint32_t>(p[i]);
            hash *= 16777619u;
        }
        return hash;
    }

    inline uint32_t HooksFnv1aUpdateString(uint32_t hash, const std::string& value)
    {
        hash = HooksFnv1aUpdate(hash, value.data(), value.size());
        const uint8_t nul = 0;
        return HooksFnv1aUpdate(hash, &nul, 1);
    }

    inline uint32_t HooksBuildViewmodelBoneSignature(
        const std::string& modelName,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        int boneIndex,
        int stride,
        int numBonesOffset)
    {
        uint32_t hash = 2166136261u;
        hash = HooksFnv1aUpdateString(hash, vr_vm_stabilize::ToLowerAscii(modelName));
        hash = HooksFnv1aUpdate(hash, &numBones, sizeof(numBones));
        hash = HooksFnv1aUpdate(hash, &boneIndex, sizeof(boneIndex));
        hash = HooksFnv1aUpdate(hash, &stride, sizeof(stride));
        hash = HooksFnv1aUpdate(hash, &numBonesOffset, sizeof(numBonesOffset));
        for (int bone = 0; bone < numBones; ++bone)
        {
            const int parent =
                bone < static_cast<int>(boneParents.size())
                ? boneParents[static_cast<size_t>(bone)]
                : -1;
            hash = HooksFnv1aUpdate(hash, &bone, sizeof(bone));
            hash = HooksFnv1aUpdate(hash, &parent, sizeof(parent));
            if (bone < static_cast<int>(boneNames.size()))
                hash = HooksFnv1aUpdateString(hash, boneNames[static_cast<size_t>(bone)]);
            else
                hash = HooksFnv1aUpdateString(hash, "");
        }
        return hash == 0 ? 1u : hash;
    }

    inline uint32_t HooksBuildStudioHdrFingerprint(void* drawState, uint32_t fallback)
    {
        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return fallback != 0 ? fallback : 1u;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);
        if (studioLength <= 0 || studioLength > 8 * 1024 * 1024)
            return fallback != 0 ? fallback : 1u;

        struct CachedStudioHash
        {
            int length = 0;
            uint32_t baseHash = 0;
        };
        static std::mutex s_hashMutex;
        static std::unordered_map<uintptr_t, CachedStudioHash> s_hashByStudioHdr;

        const uintptr_t key = reinterpret_cast<uintptr_t>(studioHdr);
        {
            std::lock_guard<std::mutex> lock(s_hashMutex);
            auto it = s_hashByStudioHdr.find(key);
            if (it != s_hashByStudioHdr.end() &&
                it->second.length == studioLength &&
                it->second.baseHash != 0)
            {
                uint32_t cachedHash = it->second.baseHash;
                cachedHash ^= fallback;
                cachedHash *= 16777619u;
                return cachedHash != 0 ? cachedHash : (fallback != 0 ? fallback : 1u);
            }
        }

        uint32_t hash = 2166136261u;
        hash = HooksFnv1aUpdate(hash, &studioLength, sizeof(studioLength));

        // Hash the resident studio header bytes once per studiohdr pointer. This differentiates
        // workshop replacements that keep the official model path.
        const int fullBytes = std::min(studioLength, 512 * 1024);
        for (int i = 0; i < fullBytes; ++i)
        {
            uint8_t byte = 0;
            if (!vr_vm_stabilize::SafeRead(studioHdr + i, byte))
                break;
            hash ^= byte;
            hash *= 16777619u;
        }

        if (studioLength > fullBytes)
        {
            const int samples = 96;
            const int step = std::max(1, studioLength / samples);
            for (int i = fullBytes; i < studioLength; i += step)
            {
                uint8_t byte = 0;
                if (!vr_vm_stabilize::SafeRead(studioHdr + i, byte))
                    continue;
                hash ^= byte;
                hash *= 16777619u;
            }
        }

        {
            std::lock_guard<std::mutex> lock(s_hashMutex);
            if (s_hashByStudioHdr.size() > 256)
                s_hashByStudioHdr.clear();
            s_hashByStudioHdr[key] = CachedStudioHash{ studioLength, hash };
        }

        hash ^= fallback;
        hash *= 16777619u;
        return hash != 0 ? hash : (fallback != 0 ? fallback : 1u);
    }

    inline bool HooksViewmodelAutoGripIsOfficialStudioFingerprint(uint32_t fingerprint)
    {
        // Generated from the raw IDST entries in the current Steam official
        // left4dead2, DLC1-DLC3, and update pak01 VPKs with
        // HooksBuildStudioHdrFingerprint(..., 1). Historical base and current
        // update variants are both included. Paths are intentionally not used:
        // workshop replacements commonly retain the official model path.
        static constexpr uint32_t kOfficialFingerprints[] = {
            0x01893D41u, 0x08148191u, 0x083E1630u, 0x09F8A0BFu,
            0x0C9149EAu, 0x0E5E9DFDu, 0x106CB6C4u, 0x1800FB20u,
            0x1ADF8CDAu, 0x1B47A09Eu, 0x22B2F8C0u, 0x23748688u,
            0x27729856u, 0x2A955C65u, 0x307F3C46u, 0x31F1EB43u,
            0x36EA35C7u, 0x3771FE31u, 0x39B66AA5u, 0x39FA48ADu,
            0x3BD608BEu, 0x3F11D09Au, 0x43A1B1F1u, 0x446CC588u,
            0x44C3EC2Du, 0x4699A3DBu, 0x4CA9FD2Eu, 0x55393D34u,
            0x5615CCA0u, 0x59BF412Cu, 0x59F57196u, 0x5B7E1C5Du,
            0x5BDF0038u, 0x5D7DB25Bu, 0x5E044BD9u, 0x5E56CCECu,
            0x5F1D9291u, 0x5F352BF6u, 0x611ED869u, 0x62F5048Eu,
            0x692BD699u, 0x69BB33C0u, 0x6A7385CDu, 0x6B226F78u,
            0x6C96556Eu, 0x6DB08646u, 0x7A64D3F4u, 0x7CEF7120u,
            0x7F8364A7u, 0x81493B1Bu, 0x858D5999u, 0x85EA1212u,
            0x8765FEDAu, 0x887D2E02u, 0x89A22725u, 0x8B21EAB0u,
            0x8E71DA2Au, 0x8EE6E8B4u, 0x9ACC5763u, 0x9D3BBEECu,
            0xA08C918Au, 0xA17E11BBu, 0xA2EF1796u, 0xA37D3104u,
            0xA68F9973u, 0xA9ACE85Au, 0xACCBC34Eu, 0xAD3845C9u,
            0xAE2EE66Bu, 0xB203B68Cu, 0xB273F92Du, 0xB5C0E302u,
            0xB93B1918u, 0xBA7809F0u, 0xBC9C7B04u, 0xBD52844Au,
            0xC100602Cu, 0xC1F52FC2u, 0xC293E892u, 0xC54A4455u,
            0xC6235F0Au, 0xC62BD4E2u, 0xCAD5D6B4u, 0xCC704258u,
            0xCD6D6560u, 0xCE3A0A5Eu, 0xD005A5DAu, 0xD20AE5D5u,
            0xD27E6080u, 0xD50E5412u, 0xDAF87D3Bu, 0xDDC7CF8Bu,
            0xE306D393u, 0xE632F52Du, 0xEA99EDF7u, 0xED5432A2u,
            0xEF313D1Bu, 0xF04A32DDu, 0xF07DD8FAu, 0xF5972561u,
            0xF6767A57u, 0xFA7DAD60u, 0xFB3D54DCu, 0xFB3F1A37u,
            0xFBE49085u, 0xFCEBDF2Cu, 0xFDAB7A55u, 0xFE691999u,
            0xFFE2A150u
        };
        const uint32_t* begin = kOfficialFingerprints;
        const uint32_t* end = begin +
            (sizeof(kOfficialFingerprints) / sizeof(kOfficialFingerprints[0]));
        return std::binary_search(begin, end, fingerprint);
    }

    inline std::string HooksBuildMagazineInteractionProfileKey(uint32_t modelFingerprint, uint32_t boneSignature)
    {
        (void)modelFingerprint;
        if (boneSignature == 0)
            return std::string();
        char text[16] = {};
        std::snprintf(text, sizeof(text), "bs%08x", boneSignature);
        return std::string(text);
    }

    inline int HooksNativeViewmodelHandsOnlyBoneSide(const std::string& lowerName);

    struct HooksViewmodelAutoGripLayout
    {
        bool parsed = false;
        bool supported = false;
        bool explicitAttachment = false;
        bool hasFingerBasis = false;
        int numBones = 0;
        int handBone = -1;
        std::array<int, 4> fingerBones{ { -1, -1, -1, -1 } };
        int attachmentBone = -1;
        vr_vm_stabilize::Mat3x4 attachmentLocal{};
        std::string anchorName;
    };

    struct HooksViewmodelAutoGripCacheEntry
    {
        HooksViewmodelAutoGripLayout layout{};
        bool candidateValid = false;
        bool locked = false;
        bool persistenceChecked = false;
        int stableSamples = 0;
        uint32_t lastSampleToken = 0;
        float palmCenterFraction = -1.0f;
        vr_vm_stabilize::Mat3x4 candidateLocal{};
        vr_vm_stabilize::Mat3x4 lockedLocal{};
        std::chrono::steady_clock::time_point lockedAt{};
        std::chrono::steady_clock::time_point lastApplyLog{};
    };

    struct HooksViewmodelAutoGripPersistentEntry
    {
        float palmCenterFraction = 0.45f;
        vr_vm_stabilize::Mat3x4 lockedLocal{};
        std::string modelName;
    };

    struct HooksViewmodelAutoGripCompanionState
    {
        uint32_t weaponFingerprint = 0;
        bool valid = false;
        vr_vm_stabilize::Mat3x4 localCorrection{};
        std::chrono::steady_clock::time_point updatedAt{};
        std::chrono::steady_clock::time_point lastApplyLog{};
    };

    std::mutex s_ViewmodelAutoGripMutex;
    std::unordered_map<uint32_t, HooksViewmodelAutoGripCacheEntry> s_ViewmodelAutoGripByFingerprint;
    std::unordered_map<VR*, std::unordered_map<int, HooksViewmodelAutoGripCompanionState>>
        s_ViewmodelAutoGripCompanionByVr;
    std::mutex s_ViewmodelAutoGripPersistenceMutex;
    bool s_ViewmodelAutoGripPersistenceLoaded = false;
    std::string s_ViewmodelAutoGripPersistencePath;
    std::unordered_map<uint32_t, HooksViewmodelAutoGripPersistentEntry>
        s_ViewmodelAutoGripPersistentByKey;

    inline bool HooksViewmodelAutoGripVectorFinite(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
            std::fabs(value.x) < 100000.0f && std::fabs(value.y) < 100000.0f && std::fabs(value.z) < 100000.0f;
    }

    inline Vector HooksViewmodelAutoGripMatrixAxis(const vr_vm_stabilize::Mat3x4& matrix, int column)
    {
        return Vector(matrix.m[0][column], matrix.m[1][column], matrix.m[2][column]);
    }

    inline bool HooksViewmodelAutoGripBuildRigidMatrix(
        const Vector& origin,
        Vector axisX,
        Vector axisY,
        Vector axisZHint,
        vr_vm_stabilize::Mat3x4& out)
    {
        if (!HooksViewmodelAutoGripVectorFinite(origin) ||
            !HooksViewmodelAutoGripVectorFinite(axisX) ||
            !HooksViewmodelAutoGripVectorFinite(axisY) ||
            !HooksViewmodelAutoGripVectorFinite(axisZHint) ||
            VectorNormalize(axisX) == 0.0f)
        {
            return false;
        }

        axisY -= axisX * DotProduct(axisX, axisY);
        if (VectorNormalize(axisY) == 0.0f)
            return false;

        Vector axisZ = CrossProduct(axisX, axisY);
        if (VectorNormalize(axisZ) == 0.0f)
            return false;
        if (VectorNormalize(axisZHint) != 0.0f && DotProduct(axisZ, axisZHint) < 0.0f)
        {
            axisY *= -1.0f;
            axisZ *= -1.0f;
        }

        out = {};
        out.m[0][0] = axisX.x; out.m[0][1] = axisY.x; out.m[0][2] = axisZ.x; out.m[0][3] = origin.x;
        out.m[1][0] = axisX.y; out.m[1][1] = axisY.y; out.m[1][2] = axisZ.y; out.m[1][3] = origin.y;
        out.m[2][0] = axisX.z; out.m[2][1] = axisY.z; out.m[2][2] = axisZ.z; out.m[2][3] = origin.z;
        return true;
    }

    inline bool HooksViewmodelAutoGripNormalizeRigidMatrix(
        const vr_vm_stabilize::Mat3x4& input,
        vr_vm_stabilize::Mat3x4& out)
    {
        return HooksViewmodelAutoGripBuildRigidMatrix(
            vr_vm_stabilize::GetOrigin(input),
            HooksViewmodelAutoGripMatrixAxis(input, 0),
            HooksViewmodelAutoGripMatrixAxis(input, 1),
            HooksViewmodelAutoGripMatrixAxis(input, 2),
            out);
    }

    inline std::string HooksViewmodelAutoGripPersistencePath(const VR* vr)
    {
        if (vr && !vr->m_ViewmodelAdjustmentSavePath.empty())
        {
            const std::string& adjustmentPath = vr->m_ViewmodelAdjustmentSavePath;
            const size_t slash = adjustmentPath.find_last_of("\\/");
            if (slash != std::string::npos)
                return adjustmentPath.substr(0, slash + 1u) + "viewmodel_auto_grip_cache.txt";
        }
        return "viewmodel_auto_grip_cache.txt";
    }

    inline uint32_t HooksBuildViewmodelAutoGripPersistentKey(
        void* drawState,
        const std::string& modelName)
    {
        uint32_t hash = 2166136261u;
        hash = HooksFnv1aUpdateString(hash, vr_vm_stabilize::ToLowerAscii(modelName));

        const uint8_t* studioHdr = nullptr;
        int studioChecksum = 0;
        int studioLength = 0;
        if (vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) && studioHdr)
        {
            vr_vm_stabilize::SafeRead(studioHdr + 0x08, studioChecksum);
            vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);
        }
        hash = HooksFnv1aUpdate(hash, &studioChecksum, sizeof(studioChecksum));
        hash = HooksFnv1aUpdate(hash, &studioLength, sizeof(studioLength));

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset) &&
            numBones > 0 && numBones <= 512)
        {
            const uint32_t boneSignature = HooksBuildViewmodelBoneSignature(
                modelName,
                boneNames,
                boneParents,
                numBones,
                boneIndex,
                stride,
                numBonesOffset);
            hash = HooksFnv1aUpdate(hash, &boneSignature, sizeof(boneSignature));
        }

        return hash == 0 ? 1u : hash;
    }

    inline void HooksViewmodelAutoGripEnsurePersistenceLoaded(VR* vr)
    {
        const std::string path = HooksViewmodelAutoGripPersistencePath(vr);
        std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripPersistenceMutex);
        if (s_ViewmodelAutoGripPersistenceLoaded &&
            s_ViewmodelAutoGripPersistencePath == path)
        {
            return;
        }

        s_ViewmodelAutoGripPersistenceLoaded = true;
        s_ViewmodelAutoGripPersistencePath = path;
        s_ViewmodelAutoGripPersistentByKey.clear();

        std::ifstream input(path, std::ios::in);
        if (!input)
            return;

        std::string line;
        while (std::getline(input, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            std::istringstream parser(line);
            std::string version;
            std::string keyText;
            HooksViewmodelAutoGripPersistentEntry entry{};
            if (!(parser >> version >> keyText >> entry.palmCenterFraction) || version != "v1" ||
                !std::isfinite(entry.palmCenterFraction) ||
                entry.palmCenterFraction < 0.0f || entry.palmCenterFraction > 1.0f)
            {
                continue;
            }

            uint32_t persistentKey = 0;
            std::istringstream keyParser(keyText);
            keyParser >> std::hex >> persistentKey;
            if (!keyParser || persistentKey == 0)
                continue;

            bool matrixParsed = true;
            for (int row = 0; row < 3 && matrixParsed; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    if (!(parser >> entry.lockedLocal.m[row][column]) ||
                        !std::isfinite(entry.lockedLocal.m[row][column]))
                    {
                        matrixParsed = false;
                        break;
                    }
                }
            }
            if (!matrixParsed)
                continue;

            parser >> std::quoted(entry.modelName);
            vr_vm_stabilize::Mat3x4 normalized{};
            if (!HooksViewmodelAutoGripNormalizeRigidMatrix(entry.lockedLocal, normalized))
                continue;
            entry.lockedLocal = normalized;
            s_ViewmodelAutoGripPersistentByKey[persistentKey] = std::move(entry);
        }

        if (vr && vr->m_ViewmodelAutoGripAlignDebugLog)
        {
            Game::logMsg(
                "[VR][AutoGrip] cache loaded path=\"%s\" entries=%u",
                path.c_str(),
                static_cast<unsigned int>(s_ViewmodelAutoGripPersistentByKey.size()));
        }
    }

    inline bool HooksViewmodelAutoGripTryRestorePersistent(
        VR* vr,
        uint32_t persistentKey,
        float palmCenterFraction,
        bool explicitAttachment,
        vr_vm_stabilize::Mat3x4& outLockedLocal)
    {
        if (!vr || persistentKey == 0)
            return false;
        HooksViewmodelAutoGripEnsurePersistenceLoaded(vr);

        HooksViewmodelAutoGripPersistentEntry entry{};
        {
            std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripPersistenceMutex);
            const auto found = s_ViewmodelAutoGripPersistentByKey.find(persistentKey);
            if (found == s_ViewmodelAutoGripPersistentByKey.end())
                return false;
            entry = found->second;
        }

        if (!explicitAttachment &&
            std::fabs(entry.palmCenterFraction - palmCenterFraction) > 0.0001f)
        {
            return false;
        }
        return HooksViewmodelAutoGripNormalizeRigidMatrix(entry.lockedLocal, outLockedLocal);
    }

    inline bool HooksViewmodelAutoGripStorePersistent(
        VR* vr,
        uint32_t persistentKey,
        float palmCenterFraction,
        const vr_vm_stabilize::Mat3x4& lockedLocal,
        const std::string& modelName)
    {
        if (!vr || persistentKey == 0)
            return false;

        vr_vm_stabilize::Mat3x4 normalized{};
        if (!HooksViewmodelAutoGripNormalizeRigidMatrix(lockedLocal, normalized))
            return false;
        HooksViewmodelAutoGripEnsurePersistenceLoaded(vr);

        std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripPersistenceMutex);
        HooksViewmodelAutoGripPersistentEntry entry{};
        entry.palmCenterFraction = std::clamp(palmCenterFraction, 0.0f, 1.0f);
        entry.lockedLocal = normalized;
        entry.modelName = modelName;
        s_ViewmodelAutoGripPersistentByKey[persistentKey] = std::move(entry);

        std::vector<uint32_t> keys;
        keys.reserve(s_ViewmodelAutoGripPersistentByKey.size());
        for (const auto& [key, unused] : s_ViewmodelAutoGripPersistentByKey)
        {
            (void)unused;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());

        const std::string path = s_ViewmodelAutoGripPersistencePath.empty()
            ? HooksViewmodelAutoGripPersistencePath(vr)
            : s_ViewmodelAutoGripPersistencePath;
        const std::string temporaryPath = path + ".tmp";
        std::ofstream output(temporaryPath, std::ios::out | std::ios::trunc);
        if (!output)
            return false;

        output << "# L4D2VR ViewmodelAutoGrip persistent cache v1\n";
        output << "# version key palmCenterFraction matrix3x4 modelName\n";
        output << std::scientific << std::setprecision(9);
        for (uint32_t key : keys)
        {
            const auto found = s_ViewmodelAutoGripPersistentByKey.find(key);
            if (found == s_ViewmodelAutoGripPersistentByKey.end())
                continue;
            const HooksViewmodelAutoGripPersistentEntry& saved = found->second;
            output << "v1 "
                << std::hex << std::setw(8) << std::setfill('0') << key
                << std::dec << std::setfill(' ') << ' '
                << saved.palmCenterFraction;
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 4; ++column)
                    output << ' ' << saved.lockedLocal.m[row][column];
            }
            output << ' ' << std::quoted(saved.modelName) << '\n';
        }
        output.flush();
        const bool writeSucceeded = output.good();
        output.close();
        if (!writeSucceeded ||
            !MoveFileExA(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileA(temporaryPath.c_str());
            return false;
        }

        if (vr->m_ViewmodelAutoGripAlignDebugLog)
        {
            Game::logMsg(
                "[VR][AutoGrip] cache saved path=\"%s\" key=%08x model=\"%s\" entries=%u",
                path.c_str(),
                persistentKey,
                modelName.c_str(),
                static_cast<unsigned int>(s_ViewmodelAutoGripPersistentByKey.size()));
        }
        return true;
    }

    inline bool HooksViewmodelAutoGripNameEndsWith(const std::string& lowerName, const char* lowerSuffix)
    {
        if (!lowerSuffix || !*lowerSuffix)
            return false;
        const size_t suffixLength = std::strlen(lowerSuffix);
        if (lowerName.size() < suffixLength)
            return false;
        return lowerName.compare(lowerName.size() - suffixLength, suffixLength, lowerSuffix) == 0;
    }

    inline int HooksViewmodelAutoGripFindBone(
        const std::vector<std::string>& boneNames,
        const std::initializer_list<const char*>& suffixes)
    {
        for (size_t bone = 0; bone < boneNames.size(); ++bone)
        {
            const std::string lower = vr_vm_stabilize::ToLowerAscii(boneNames[bone]);
            for (const char* suffix : suffixes)
            {
                if (HooksViewmodelAutoGripNameEndsWith(lower, suffix))
                    return static_cast<int>(bone);
            }
        }
        return -1;
    }

    inline bool HooksViewmodelAutoGripTryFindAttachment(
        void* drawState,
        int numBones,
        int& outBone,
        vr_vm_stabilize::Mat3x4& outLocal,
        std::string& outName,
        bool& outAttachmentTableParsed)
    {
        outBone = -1;
        outLocal = {};
        outName.clear();
        outAttachmentTableParsed = false;

        const uint8_t* studioHdr = nullptr;
        if (!drawState || numBones <= 0 ||
            !vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
        {
            return false;
        }

        int studioLength = 0;
        int numAttachments = 0;
        int attachmentIndex = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);
        if (!vr_vm_stabilize::SafeRead(studioHdr + 0xF0, numAttachments) ||
            !vr_vm_stabilize::SafeRead(studioHdr + 0xF4, attachmentIndex))
        {
            return false;
        }

        if (numAttachments == 0)
        {
            outAttachmentTableParsed = true;
            return false;
        }
        if (numAttachments < 0 || numAttachments > 256 || attachmentIndex <= 0 || attachmentIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (attachmentIndex >= studioLength ||
                static_cast<size_t>(attachmentIndex) + static_cast<size_t>(numAttachments) * 92u > static_cast<size_t>(studioLength)))
        {
            return false;
        }

        outAttachmentTableParsed = true;
        static constexpr int kAttachmentStride = 92;
        for (int attachment = 0; attachment < numAttachments; ++attachment)
        {
            const size_t attachmentOffset =
                static_cast<size_t>(attachmentIndex) + static_cast<size_t>(attachment) * kAttachmentStride;
            const uint8_t* attachmentBase = studioHdr + attachmentOffset;

            int nameOffset = 0;
            int localBone = -1;
            if (!vr_vm_stabilize::SafeRead(attachmentBase + 0, nameOffset) ||
                !vr_vm_stabilize::SafeRead(attachmentBase + 8, localBone) ||
                nameOffset <= 0 || localBone < 0 || localBone >= numBones)
            {
                continue;
            }

            const size_t nameAddressOffset = attachmentOffset + static_cast<size_t>(nameOffset);
            if (studioLength > 0 && nameAddressOffset >= static_cast<size_t>(studioLength))
                continue;

            std::string name;
            if (!vr_vm_stabilize::TryReadCStringSafe(
                reinterpret_cast<const char*>(studioHdr + nameAddressOffset),
                name))
            {
                continue;
            }

            const std::string lower = vr_vm_stabilize::ToLowerAscii(name);
            if (lower != "vr_grip" && lower != "vr_grip_r" &&
                lower != "vr_weapon_grip" && lower != "weapon_vr_grip")
            {
                continue;
            }

            vr_vm_stabilize::Mat3x4 local{};
            if (!vr_vm_stabilize::SafeRead(attachmentBase + 12, local))
                continue;

            outBone = localBone;
            outLocal = local;
            outName = name;
            return true;
        }
        return false;
    }

    inline bool HooksViewmodelAutoGripResolveLayout(
        void* drawState,
        HooksViewmodelAutoGripLayout& outLayout)
    {
        outLayout = {};

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        const bool namesParsed = vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);
        if (!namesParsed && !vr_vm_stabilize::TryGetNumBonesFromDrawState(drawState, numBones))
            return false;
        if (numBones <= 0 || numBones > 512)
            return false;

        outLayout.numBones = numBones;
        bool attachmentTableParsed = false;
        if (HooksViewmodelAutoGripTryFindAttachment(
            drawState,
            numBones,
            outLayout.attachmentBone,
            outLayout.attachmentLocal,
            outLayout.anchorName,
            attachmentTableParsed))
        {
            outLayout.parsed = true;
            outLayout.supported = true;
            outLayout.explicitAttachment = true;
            return true;
        }

        if (!namesParsed)
        {
            outLayout.parsed = attachmentTableParsed;
            return outLayout.parsed;
        }

        outLayout.parsed = true;
        outLayout.handBone = HooksViewmodelAutoGripFindBone(
            boneNames,
            { "valvebiped.bip01_r_hand", "bip01_r_hand", "r_hand", "hand_r" });
        outLayout.fingerBones[0] = HooksViewmodelAutoGripFindBone(
            boneNames,
            { "valvebiped.bip01_r_finger1", "bip01_r_finger1" });
        outLayout.fingerBones[1] = HooksViewmodelAutoGripFindBone(
            boneNames,
            { "valvebiped.bip01_r_finger2", "bip01_r_finger2" });
        outLayout.fingerBones[2] = HooksViewmodelAutoGripFindBone(
            boneNames,
            { "valvebiped.bip01_r_finger3", "bip01_r_finger3" });
        outLayout.fingerBones[3] = HooksViewmodelAutoGripFindBone(
            boneNames,
            { "valvebiped.bip01_r_finger4", "bip01_r_finger4" });

        outLayout.hasFingerBasis = outLayout.handBone >= 0 &&
            std::all_of(
                outLayout.fingerBones.begin(),
                outLayout.fingerBones.end(),
                [](int bone) { return bone >= 0; });
        outLayout.supported = outLayout.handBone >= 0;
        outLayout.anchorName = outLayout.hasFingerBasis
            ? "ValveBiped right palm"
            : outLayout.supported
            ? "ValveBiped right hand (position only)"
            : "none";
        return true;
    }

    inline bool HooksViewmodelAutoGripBuildAnchorWorld(
        const HooksViewmodelAutoGripLayout& layout,
        const vr_vm_stabilize::Mat3x4* bones,
        float palmCenterFraction,
        const vr_vm_stabilize::Mat3x4& entityWorld,
        vr_vm_stabilize::Mat3x4& outAnchorWorld)
    {
        if (!bones || layout.numBones <= 0)
            return false;

        auto readBone = [&](int bone, vr_vm_stabilize::Mat3x4& out) -> bool
            {
                return bone >= 0 && bone < layout.numBones &&
                    vr_vm_stabilize::SafeRead(bones + bone, out);
            };

        if (layout.explicitAttachment)
        {
            vr_vm_stabilize::Mat3x4 boneWorld{};
            vr_vm_stabilize::Mat3x4 attachmentWorld{};
            if (!readBone(layout.attachmentBone, boneWorld))
                return false;
            vr_vm_stabilize::Mul(boneWorld, layout.attachmentLocal, attachmentWorld);
            return HooksViewmodelAutoGripNormalizeRigidMatrix(attachmentWorld, outAnchorWorld);
        }

        vr_vm_stabilize::Mat3x4 handWorld{};
        if (!readBone(layout.handBone, handWorld))
            return false;
        const Vector handOrigin = vr_vm_stabilize::GetOrigin(handWorld);
        if (!HooksViewmodelAutoGripVectorFinite(handOrigin))
            return false;

        if (layout.hasFingerBasis)
        {
            std::array<Vector, 4> fingerOrigins{};
            Vector average(0.0f, 0.0f, 0.0f);
            for (size_t finger = 0; finger < fingerOrigins.size(); ++finger)
            {
                vr_vm_stabilize::Mat3x4 fingerWorld{};
                if (!readBone(layout.fingerBones[finger], fingerWorld))
                    return false;
                fingerOrigins[finger] = vr_vm_stabilize::GetOrigin(fingerWorld);
                if (!HooksViewmodelAutoGripVectorFinite(fingerOrigins[finger]))
                    return false;
                average += fingerOrigins[finger];
            }
            average *= 0.25f;

            const Vector palmForward = average - handOrigin;
            const Vector palmSide = fingerOrigins[0] - fingerOrigins[3];
            const Vector palmNormal = CrossProduct(palmForward, palmSide);
            const Vector palmOrigin = handOrigin + palmForward * std::clamp(palmCenterFraction, 0.0f, 1.0f);
            return HooksViewmodelAutoGripBuildRigidMatrix(
                palmOrigin,
                palmForward,
                palmSide,
                palmNormal,
                outAnchorWorld);
        }

        // A hand-only skeleton is still useful for position correction. Keep the current
        // entity orientation so a non-standard hand-bone axis can never flip the weapon.
        outAnchorWorld = entityWorld;
        outAnchorWorld.m[0][3] = handOrigin.x;
        outAnchorWorld.m[1][3] = handOrigin.y;
        outAnchorWorld.m[2][3] = handOrigin.z;
        return true;
    }

    inline bool HooksViewmodelAutoGripLocalSamplesMatch(
        const vr_vm_stabilize::Mat3x4& left,
        const vr_vm_stabilize::Mat3x4& right)
    {
        const Vector positionDelta = vr_vm_stabilize::GetOrigin(left) - vr_vm_stabilize::GetOrigin(right);
        if (positionDelta.LengthSqr() > (0.35f * 0.35f))
            return false;

        constexpr float kMinAxisDot = 0.9986295f; // cos(3 degrees)
        for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
        {
            Vector leftAxis = HooksViewmodelAutoGripMatrixAxis(left, axisIndex);
            Vector rightAxis = HooksViewmodelAutoGripMatrixAxis(right, axisIndex);
            if (VectorNormalize(leftAxis) == 0.0f || VectorNormalize(rightAxis) == 0.0f ||
                DotProduct(leftAxis, rightAxis) < kMinAxisDot)
            {
                return false;
            }
        }
        return true;
    }

    inline void HooksViewmodelAutoGripRotateBasis(
        Vector& forward,
        Vector& right,
        Vector& up,
        const QAngle& angleOffset)
    {
        forward = VectorRotate(forward, up, angleOffset.y);
        right = VectorRotate(right, up, angleOffset.y);
        forward = VectorRotate(forward, right, angleOffset.x);
        up = VectorRotate(up, right, angleOffset.x);
        right = VectorRotate(right, forward, angleOffset.z);
        up = VectorRotate(up, forward, angleOffset.z);
    }

    inline void PublishViewmodelAutoGripAimPose(
        VR* vr,
        const vr_vm_stabilize::Mat3x4& drawDelta)
    {
        if (!vr)
            return;

        // DrawModelExecute is the point where the final AutoGrip rigid delta is
        // known. Use the matching render-frame controller snapshot so the ray
        // starts exactly where the legacy aim line would have started, then move
        // that ray by the same delta as the visible gun.
        struct RenderSnapshotTLSGuard
        {
            bool previous = false;
            RenderSnapshotTLSGuard()
            {
                previous = VR::t_UseRenderFrameSnapshot;
                VR::t_UseRenderFrameSnapshot = true;
            }
            ~RenderSnapshotTLSGuard()
            {
                VR::t_UseRenderFrameSnapshot = previous;
            }
        } snapshotGuard;

        Vector baseDirection{};
        QAngle::AngleVectors(vr->GetRightControllerAbsAngle(), &baseDirection, nullptr, nullptr);
        if (baseDirection.IsZero() || VectorNormalize(baseDirection) == 0.0f)
            return;

        const Vector baseStart = vr->GetRightControllerViewmodelAbsPos() + baseDirection * 2.0f;
        const Vector correctedStart = HooksTransformPoint(drawDelta, baseStart);
        Vector correctedDirection = HooksTransformVector(drawDelta, baseDirection);
        if (correctedDirection.IsZero() || VectorNormalize(correctedDirection) == 0.0f ||
            !HooksViewmodelAutoGripVectorFinite(correctedStart) ||
            !HooksViewmodelAutoGripVectorFinite(correctedDirection))
        {
            return;
        }

        const uint32_t adjustKeyHash =
            vr->m_ViewmodelAutoGripCurrentAdjustKeyHash.load(std::memory_order_acquire);
        if (adjustKeyHash == 0u)
            return;

        uint32_t seq = vr->m_ViewmodelAutoGripAimPoseSeq.load(std::memory_order_relaxed);
        if (seq & 1u)
            ++seq;
        const uint32_t odd = seq + 1u;
        const uint32_t even = odd + 1u;

        vr->m_ViewmodelAutoGripAimPoseSeq.store(odd, std::memory_order_release);
        vr->m_ViewmodelAutoGripAimPosX.store(correctedStart.x, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPosY.store(correctedStart.y, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPosZ.store(correctedStart.z, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimDirX.store(correctedDirection.x, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimDirY.store(correctedDirection.y, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimDirZ.store(correctedDirection.z, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPoseTickMs.store(GetTickCount(), std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPoseAdjustKeyHash.store(adjustKeyHash, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPoseSeq.store(even, std::memory_order_release);
    }

    inline void InvalidateViewmodelAutoGripAimPose(VR* vr)
    {
        if (!vr)
            return;

        uint32_t seq = vr->m_ViewmodelAutoGripAimPoseSeq.load(std::memory_order_relaxed);
        if (seq & 1u)
            ++seq;
        const uint32_t odd = seq + 1u;
        const uint32_t even = odd + 1u;

        vr->m_ViewmodelAutoGripAimPoseSeq.store(odd, std::memory_order_release);
        vr->m_ViewmodelAutoGripAimPosX.store(0.0f, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPosY.store(0.0f, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPosZ.store(0.0f, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimDirX.store(0.0f, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimDirY.store(0.0f, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimDirZ.store(0.0f, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPoseTickMs.store(0u, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPoseAdjustKeyHash.store(0u, std::memory_order_relaxed);
        vr->m_ViewmodelAutoGripAimPoseSeq.store(even, std::memory_order_release);
    }

    inline float HooksViewmodelAutoGripWrapAngle(float angle)
    {
        angle -= 360.0f * std::floor((angle + 180.0f) / 360.0f);
        return angle;
    }

    inline bool HooksViewmodelAutoGripMatrixAngles(
        const vr_vm_stabilize::Mat3x4& matrix,
        QAngle& outAngles)
    {
        Vector forward = HooksViewmodelAutoGripMatrixAxis(matrix, 0);
        Vector up = HooksViewmodelAutoGripMatrixAxis(matrix, 2);
        if (VectorNormalize(forward) == 0.0f || VectorNormalize(up) == 0.0f)
            return false;
        QAngle::VectorAngles(forward, up, outAngles);
        outAngles.x = HooksViewmodelAutoGripWrapAngle(outAngles.x);
        outAngles.y = HooksViewmodelAutoGripWrapAngle(outAngles.y);
        outAngles.z = HooksViewmodelAutoGripWrapAngle(outAngles.z);
        return std::isfinite(outAngles.x) && std::isfinite(outAngles.y) && std::isfinite(outAngles.z);
    }

    inline bool HooksViewmodelAutoGripBoneDescendsFrom(
        int bone,
        int ancestor,
        const std::vector<int>& boneParents)
    {
        if (bone < 0 || ancestor < 0 || bone >= static_cast<int>(boneParents.size()))
            return false;

        int current = bone;
        for (int guard = 0; guard < static_cast<int>(boneParents.size()); ++guard)
        {
            if (current == ancestor)
                return true;
            if (current < 0 || current >= static_cast<int>(boneParents.size()))
                break;
            const int parent = boneParents[static_cast<size_t>(current)];
            if (parent == current)
                break;
            current = parent;
        }
        return false;
    }

    inline bool ApplyViewmodelAutoGripCompanionAlignment(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        ModelRenderInfo_t& drawInfoStorage,
        const ModelRenderInfo_t*& pDrawInfo,
        void*& pBonesToWorldFinal)
    {
        if (!vr || !drawState || !pDrawInfo || !pBonesToWorldFinal)
            return false;

        HooksViewmodelAutoGripCompanionState companion{};
        const int entityIndex = pDrawInfo->entity_index;
        int companionEntityKey = entityIndex;
        {
            std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
            const auto ownerFound = s_ViewmodelAutoGripCompanionByVr.find(vr);
            if (ownerFound == s_ViewmodelAutoGripCompanionByVr.end())
                return false;
            const auto entityFound = ownerFound->second.find(entityIndex);
            if (entityFound != ownerFound->second.end() && entityFound->second.valid)
            {
                companion = entityFound->second;
            }
            else
            {
                // Bonemerged arms are not guaranteed to reuse the weapon viewmodel's entity index.
                // Fall back to the newest valid correction owned by this VR instance.
                bool foundFallback = false;
                for (const auto& [candidateEntity, candidate] : ownerFound->second)
                {
                    if (!candidate.valid ||
                        candidate.updatedAt.time_since_epoch().count() == 0)
                    {
                        continue;
                    }
                    if (!foundFallback || candidate.updatedAt > companion.updatedAt)
                    {
                        companion = candidate;
                        companionEntityKey = candidateEntity;
                        foundFallback = true;
                    }
                }
                if (!foundFallback)
                    return false;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (companion.updatedAt.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - companion.updatedAt).count() > 0.35f)
        {
            std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
            auto ownerFound = s_ViewmodelAutoGripCompanionByVr.find(vr);
            if (ownerFound != s_ViewmodelAutoGripCompanionByVr.end())
            {
                auto entityFound = ownerFound->second.find(companionEntityKey);
                if (entityFound != ownerFound->second.end() &&
                    entityFound->second.weaponFingerprint == companion.weaponFingerprint)
                {
                    entityFound->second.valid = false;
                }
            }
            return false;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset) ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        const int leftHand = HooksViewmodelAutoGripFindBone(
            boneNames,
            { "valvebiped.bip01_l_hand", "bip01_l_hand", "l_hand", "hand_l" });
        const int rightHand = HooksViewmodelAutoGripFindBone(
            boneNames,
            { "valvebiped.bip01_r_hand", "bip01_r_hand", "r_hand", "hand_r" });
        const bool hasLeftHand = leftHand >= 0 && leftHand < numBones;
        const bool hasRightHand = rightHand >= 0 && rightHand < numBones;
        const bool twoHandGripActive = vr->IsVrHandsTwoHandedGripPoseActive();
        if (!hasRightHand && !(twoHandGripActive && hasLeftHand))
            return false;

        const bool containsBothHands = hasLeftHand && hasRightHand;
        const bool moveWholeCompanion = !containsBothHands || twoHandGripActive;

        int rightBranchRoot = hasRightHand ? rightHand : -1;
        if (containsBothHands && !twoHandGripActive)
        {
            for (int guard = 0; guard < numBones; ++guard)
            {
                const int parent = boneParents[static_cast<size_t>(rightBranchRoot)];
                if (parent < 0 || parent >= numBones || parent == rightBranchRoot)
                    break;
                const std::string lowerParent =
                    vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(parent)]);
                if (HooksNativeViewmodelHandsOnlyBoneSide(lowerParent) != 1)
                    break;
                rightBranchRoot = parent;
            }
        }

        vr_vm_stabilize::Mat3x4 baseEntity{};
        vr_vm_stabilize::BuildFromOrgAngles(pDrawInfo->origin, pDrawInfo->angles, baseEntity);
        vr_vm_stabilize::Mat3x4 targetEntity{};
        vr_vm_stabilize::Mul(baseEntity, companion.localCorrection, targetEntity);
        vr_vm_stabilize::Mat3x4 baseInverse{};
        vr_vm_stabilize::Mat3x4 drawDelta{};
        vr_vm_stabilize::InvertTR(baseEntity, baseInverse);
        vr_vm_stabilize::Mul(targetEntity, baseInverse, drawDelta);

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_acquire) & ~1u;
        if (seqEven == 0)
            seqEven = 2;
        vr_vm_stabilize::Mat3x4* alignedBones =
            vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!alignedBones)
            return false;
        std::memcpy(
            alignedBones,
            pBonesToWorldFinal,
            static_cast<size_t>(numBones) * sizeof(vr_vm_stabilize::Mat3x4));

        if (containsBothHands && !twoHandGripActive)
        {
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!HooksViewmodelAutoGripBoneDescendsFrom(bone, rightBranchRoot, boneParents))
                    continue;
                vr_vm_stabilize::Mat3x4 moved{};
                vr_vm_stabilize::Mul(drawDelta, alignedBones[bone], moved);
                alignedBones[bone] = moved;
            }
        }
        else
        {
            vr_vm_stabilize::ApplyDelta(drawDelta, alignedBones, numBones);
        }
        pBonesToWorldFinal = alignedBones;

        // Outside two-hand grip, preserve the controller-owned left branch. Once two-hand grip
        // is active, move the complete companion so the visible left hand stays attached to the
        // AutoGrip-corrected weapon; a left-only companion is accepted only in that state.
        if (moveWholeCompanion)
        {
            QAngle targetAngles{};
            if (!HooksViewmodelAutoGripMatrixAngles(targetEntity, targetAngles))
                return false;
            drawInfoStorage = *pDrawInfo;
            drawInfoStorage.origin = vr_vm_stabilize::GetOrigin(targetEntity);
            drawInfoStorage.angles = targetAngles;
            vr_vm_stabilize::Mat3x4* stableModelToWorld = vr_vm_stabilize::AllocStableBones(1, seqEven);
            if (stableModelToWorld)
            {
                *stableModelToWorld = targetEntity;
                drawInfoStorage.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(stableModelToWorld);
            }
            pDrawInfo = &drawInfoStorage;
        }

        if (vr->m_ViewmodelAutoGripAlignDebugLog)
        {
            bool logNow = false;
            {
                std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
                auto& state = s_ViewmodelAutoGripCompanionByVr[vr][companionEntityKey];
                if (state.lastApplyLog.time_since_epoch().count() == 0 ||
                    std::chrono::duration<float>(now - state.lastApplyLog).count() >= 0.5f)
                {
                    state.lastApplyLog = now;
                    logNow = true;
                }
            }
            if (logNow)
            {
                Game::logMsg(
                    "[VR][AutoGrip] companion model=\"%s\" fp=%08x mode=%s root=%d bones=%d",
                    modelName.c_str(),
                    companion.weaponFingerprint,
                    (containsBothHands && !twoHandGripActive)
                    ? "right-branch"
                    : (twoHandGripActive && hasLeftHand)
                    ? "two-hand-whole-model"
                    : "whole-model",
                    rightBranchRoot,
                    numBones);
            }
        }
        return true;
    }

    inline bool ApplyViewmodelAutoGripAlignment(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        bool drawEntityIsViewmodelClass,
        ModelRenderInfo_t& drawInfoStorage,
        const ModelRenderInfo_t*& pDrawInfo,
        void*& pBonesToWorldFinal)
    {
        if (!vr || !vr->m_ViewmodelAutoGripAlignEnabled || !vr->m_IsVREnabled ||
            vr->m_MouseModeEnabled || vr->m_IsThirdPersonCamera || !drawState || !pDrawInfo ||
            modelName.empty())
        {
            return false;
        }

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!drawEntityIsViewmodelClass && !HooksModelNameIsViewmodel(lowerModel))
        {
            return false;
        }

        if (HooksModelNameIsArmsOrHands(lowerModel))
        {
            return ApplyViewmodelAutoGripCompanionAlignment(
                vr,
                drawState,
                modelName,
                drawInfoStorage,
                pDrawInfo,
                pBonesToWorldFinal);
        }

        const uint32_t fingerprint = HooksBuildStudioHdrFingerprint(drawState, 1u);
        if (fingerprint == 0 || fingerprint == 1u)
            return false;

        if (HooksViewmodelAutoGripIsOfficialStudioFingerprint(fingerprint))
        {
            // Official weapons/items already use the native controller placement.
            // Drop every recently published MOD companion correction before the
            // arms pass can fall back to it, and make the aim ray use its native
            // controller pose as well. This happens before layout parsing, manual
            // residual clearing, sampling, or persistent-cache access.
            {
                std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
                const auto ownerFound = s_ViewmodelAutoGripCompanionByVr.find(vr);
                if (ownerFound != s_ViewmodelAutoGripCompanionByVr.end())
                {
                    for (auto& [entityIndex, companion] : ownerFound->second)
                    {
                        (void)entityIndex;
                        companion.valid = false;
                        companion.updatedAt = {};
                    }
                }
            }
            InvalidateViewmodelAutoGripAimPose(vr);
            return false;
        }

        const uint32_t persistentKey =
            HooksBuildViewmodelAutoGripPersistentKey(drawState, modelName);

        {
            std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
            if (s_ViewmodelAutoGripCompanionByVr.size() > 8u)
                s_ViewmodelAutoGripCompanionByVr.clear();
            auto& companionByEntity = s_ViewmodelAutoGripCompanionByVr[vr];
            if (companionByEntity.size() > 16u)
                companionByEntity.clear();
            auto& companion = companionByEntity[pDrawInfo->entity_index];
            if (companion.weaponFingerprint != fingerprint)
            {
                companion.weaponFingerprint = fingerprint;
                companion.valid = false;
                companion.updatedAt = {};
            }
        }

        HooksViewmodelAutoGripLayout layout{};
        bool needsLayout = false;
        {
            std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
            auto& entry = s_ViewmodelAutoGripByFingerprint[fingerprint];
            needsLayout = !entry.layout.parsed;
            layout = entry.layout;
        }

        if (needsLayout)
        {
            HooksViewmodelAutoGripLayout resolved{};
            if (!HooksViewmodelAutoGripResolveLayout(drawState, resolved) || !resolved.parsed)
                return false;

            {
                std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
                if (s_ViewmodelAutoGripByFingerprint.size() > 256u)
                    s_ViewmodelAutoGripByFingerprint.clear();
                auto& entry = s_ViewmodelAutoGripByFingerprint[fingerprint];
                entry.layout = resolved;
                layout = entry.layout;
            }

            if (vr->m_ViewmodelAutoGripAlignDebugLog)
            {
                Game::logMsg(
                    "[VR][AutoGrip] layout model=\"%s\" fp=%08x supported=%d anchor=\"%s\" bones=%d fullBasis=%d explicit=%d",
                    modelName.c_str(),
                    fingerprint,
                    resolved.supported ? 1 : 0,
                    resolved.anchorName.c_str(),
                    resolved.numBones,
                    resolved.hasFingerBasis ? 1 : 0,
                    resolved.explicitAttachment ? 1 : 0);
            }
        }

        if (!layout.supported)
            return false;

        uint32_t sampleToken = vr->m_RenderFrameSeq.load(std::memory_order_relaxed) & ~1u;
        if (sampleToken == 0)
            sampleToken = static_cast<uint32_t>(GetTickCount());

        bool locked = false;
        bool justLocked = false;
        bool restoredFromPersistence = false;
        bool shouldTryPersistence = false;
        vr_vm_stabilize::Mat3x4 lockedLocal{};
        std::chrono::steady_clock::time_point lockedAt{};
        {
            std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
            auto& entry = s_ViewmodelAutoGripByFingerprint[fingerprint];
            if (!layout.explicitAttachment &&
                std::fabs(entry.palmCenterFraction - vr->m_ViewmodelAutoGripPalmCenterFraction) > 0.0001f)
            {
                entry.candidateValid = false;
                entry.locked = false;
                entry.persistenceChecked = false;
                entry.stableSamples = 0;
                entry.lastSampleToken = 0;
                entry.palmCenterFraction = vr->m_ViewmodelAutoGripPalmCenterFraction;
            }
            else if (entry.palmCenterFraction < 0.0f)
            {
                entry.palmCenterFraction = vr->m_ViewmodelAutoGripPalmCenterFraction;
            }
            shouldTryPersistence = !entry.locked && !entry.persistenceChecked;
            if (shouldTryPersistence)
                entry.persistenceChecked = true;
            locked = entry.locked;
            if (locked)
            {
                lockedLocal = entry.lockedLocal;
                lockedAt = entry.lockedAt;
            }
        }

        if (shouldTryPersistence)
        {
            vr_vm_stabilize::Mat3x4 persistedLocal{};
            if (HooksViewmodelAutoGripTryRestorePersistent(
                vr,
                persistentKey,
                vr->m_ViewmodelAutoGripPalmCenterFraction,
                layout.explicitAttachment,
                persistedLocal))
            {
                const auto restoredAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(180);
                std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
                auto& entry = s_ViewmodelAutoGripByFingerprint[fingerprint];
                const bool fractionStillMatches = layout.explicitAttachment ||
                    std::fabs(entry.palmCenterFraction - vr->m_ViewmodelAutoGripPalmCenterFraction) <= 0.0001f;
                if (!entry.locked && fractionStillMatches)
                {
                    entry.lockedLocal = persistedLocal;
                    entry.locked = true;
                    entry.lockedAt = restoredAt;
                    entry.candidateValid = false;
                    entry.stableSamples = 0;
                    locked = true;
                    restoredFromPersistence = true;
                    lockedLocal = entry.lockedLocal;
                    lockedAt = entry.lockedAt;
                }
            }
        }

        if (!locked)
        {
            // The persistent cache missed, so this model is about to be calculated for
            // the first time. Ask the main thread to zero and save the current weapon
            // ID's manual residual, and do not sample until that exact request completes.
            const uint32_t adjustKeyHash =
                vr->m_ViewmodelAutoGripCurrentAdjustKeyHash.load(std::memory_order_acquire);
            if (adjustKeyHash == 0u)
                return false;

            const uint64_t clearRequest =
                (static_cast<uint64_t>(adjustKeyHash) << 32u) |
                static_cast<uint64_t>(persistentKey);
            const uint64_t previousRequest =
                vr->m_ViewmodelAutoGripManualClearRequest.exchange(
                    clearRequest,
                    std::memory_order_acq_rel);

            if (previousRequest != clearRequest && vr->m_ViewmodelAutoGripAlignDebugLog)
            {
                Game::logMsg(
                    "[VR][AutoGrip] requested manual clear model=\"%s\" key=%08x weaponHash=%08x",
                    modelName.c_str(),
                    persistentKey,
                    adjustKeyHash);
            }

            if (vr->m_ViewmodelAutoGripManualClearCompleted.load(std::memory_order_acquire) != clearRequest)
                return false;
        }

        if (!locked && pBonesToWorldFinal)
        {
            bool shouldSample = false;
            {
                std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
                auto& entry = s_ViewmodelAutoGripByFingerprint[fingerprint];
                shouldSample = entry.lastSampleToken != sampleToken;
                if (shouldSample)
                    entry.lastSampleToken = sampleToken;
            }

            if (shouldSample)
            {
                vr_vm_stabilize::Mat3x4 entityWorld{};
                vr_vm_stabilize::BuildFromOrgAngles(pDrawInfo->origin, pDrawInfo->angles, entityWorld);
                vr_vm_stabilize::Mat3x4 anchorWorld{};
                vr_vm_stabilize::Mat3x4 entityInverse{};
                vr_vm_stabilize::Mat3x4 sampleLocal{};
                if (HooksViewmodelAutoGripBuildAnchorWorld(
                    layout,
                    reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pBonesToWorldFinal),
                    vr->m_ViewmodelAutoGripPalmCenterFraction,
                    entityWorld,
                    anchorWorld))
                {
                    vr_vm_stabilize::InvertTR(entityWorld, entityInverse);
                    vr_vm_stabilize::Mul(entityInverse, anchorWorld, sampleLocal);
                    vr_vm_stabilize::Mat3x4 normalizedLocal{};
                    if (HooksViewmodelAutoGripNormalizeRigidMatrix(sampleLocal, normalizedLocal))
                    {
                        const auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
                        auto& entry = s_ViewmodelAutoGripByFingerprint[fingerprint];
                        if (!entry.candidateValid ||
                            !HooksViewmodelAutoGripLocalSamplesMatch(entry.candidateLocal, normalizedLocal))
                        {
                            entry.candidateLocal = normalizedLocal;
                            entry.candidateValid = true;
                            entry.stableSamples = 1;
                        }
                        else
                        {
                            ++entry.stableSamples;
                        }

                        constexpr int kStableSamplesRequired = 18;
                        if (entry.stableSamples >= kStableSamplesRequired)
                        {
                            entry.lockedLocal = entry.candidateLocal;
                            entry.locked = true;
                            entry.lockedAt = now;
                            locked = true;
                            justLocked = true;
                            lockedLocal = entry.lockedLocal;
                            lockedAt = entry.lockedAt;
                        }
                    }
                }
            }
        }

        if (!locked)
            return false;

        if (restoredFromPersistence && vr->m_ViewmodelAutoGripAlignDebugLog)
        {
            const Vector localOrigin = vr_vm_stabilize::GetOrigin(lockedLocal);
            Game::logMsg(
                "[VR][AutoGrip] restored model=\"%s\" fp=%08x key=%08x local=(%.3f %.3f %.3f)",
                modelName.c_str(),
                fingerprint,
                persistentKey,
                localOrigin.x,
                localOrigin.y,
                localOrigin.z);
        }

        if (justLocked &&
            !HooksViewmodelAutoGripStorePersistent(
                vr,
                persistentKey,
                vr->m_ViewmodelAutoGripPalmCenterFraction,
                lockedLocal,
                modelName) &&
            vr->m_ViewmodelAutoGripAlignDebugLog)
        {
            Game::logMsg(
                "[VR][AutoGrip] cache save failed key=%08x model=\"%s\"",
                persistentKey,
                modelName.c_str());
        }

        if (justLocked && vr->m_ViewmodelAutoGripAlignDebugLog)
        {
            const Vector localOrigin = vr_vm_stabilize::GetOrigin(lockedLocal);
            Game::logMsg(
                "[VR][AutoGrip] locked model=\"%s\" fp=%08x anchor=\"%s\" local=(%.3f %.3f %.3f)",
                modelName.c_str(),
                fingerprint,
                layout.anchorName.c_str(),
                localOrigin.x,
                localOrigin.y,
                localOrigin.z);
        }

        vr_vm_stabilize::Mat3x4 baseEntity{};
        vr_vm_stabilize::BuildFromOrgAngles(pDrawInfo->origin, pDrawInfo->angles, baseEntity);

        const bool fullRotationAlignment =
            layout.explicitAttachment || (layout.hasFingerBasis && vr->m_ViewmodelAutoGripAlignRotation);

        Vector targetForward{};
        Vector targetRight{};
        Vector targetUp{};
        if (fullRotationAlignment)
        {
            QAngle controllerAngles = vr->GetRightControllerAbsAngle();
            QAngle::AngleVectors(controllerAngles, &targetForward, &targetRight, &targetUp);
            const QAngle rotationOffset(
                vr->m_ViewmodelAngAdjust.x + vr->m_ViewmodelAutoGripTargetRotationOffsetDeg.x,
                vr->m_ViewmodelAngAdjust.y + vr->m_ViewmodelAutoGripTargetRotationOffsetDeg.y,
                vr->m_ViewmodelAngAdjust.z + vr->m_ViewmodelAutoGripTargetRotationOffsetDeg.z);
            HooksViewmodelAutoGripRotateBasis(targetForward, targetRight, targetUp, rotationOffset);
        }
        else
        {
            QAngle::AngleVectors(pDrawInfo->angles, &targetForward, &targetRight, &targetUp);
        }

        if (VectorNormalize(targetForward) == 0.0f ||
            VectorNormalize(targetRight) == 0.0f ||
            VectorNormalize(targetUp) == 0.0f)
        {
            return false;
        }

        const Vector& targetOffset = vr->m_ViewmodelAutoGripTargetOffsetMeters;
        const Vector& manualOffset = vr->m_ViewmodelPosAdjust;
        const Vector desiredGripOrigin = vr->GetRightControllerAbsPos()
            + targetForward * (targetOffset.x * vr->m_VRScale - manualOffset.x)
            + targetRight * (targetOffset.y * vr->m_VRScale - manualOffset.y)
            + targetUp * (targetOffset.z * vr->m_VRScale - manualOffset.z);

        vr_vm_stabilize::Mat3x4 solvedEntity = baseEntity;
        if (fullRotationAlignment)
        {
            Vector palmNormal = CrossProduct(targetForward, targetUp);
            vr_vm_stabilize::Mat3x4 desiredGripWorld{};
            if (!HooksViewmodelAutoGripBuildRigidMatrix(
                desiredGripOrigin,
                targetForward,
                targetUp,
                palmNormal,
                desiredGripWorld))
            {
                return false;
            }

            vr_vm_stabilize::Mat3x4 localInverse{};
            vr_vm_stabilize::InvertTR(lockedLocal, localInverse);
            vr_vm_stabilize::Mul(desiredGripWorld, localInverse, solvedEntity);
        }
        else
        {
            vr_vm_stabilize::Mat3x4 currentGripWorld{};
            vr_vm_stabilize::Mul(baseEntity, lockedLocal, currentGripWorld);
            const Vector correction = desiredGripOrigin - vr_vm_stabilize::GetOrigin(currentGripWorld);
            solvedEntity.m[0][3] += correction.x;
            solvedEntity.m[1][3] += correction.y;
            solvedEntity.m[2][3] += correction.z;
        }

        const Vector baseOrigin = vr_vm_stabilize::GetOrigin(baseEntity);
        const Vector solvedOrigin = vr_vm_stabilize::GetOrigin(solvedEntity);
        const Vector correction = solvedOrigin - baseOrigin;
        const float maxCorrection = std::max(24.0f, vr->m_VRScale * 0.75f);
        if (!HooksViewmodelAutoGripVectorFinite(solvedOrigin) ||
            correction.LengthSqr() > maxCorrection * maxCorrection)
        {
            if (vr->m_ViewmodelAutoGripAlignDebugLog)
            {
                Game::logMsg(
                    "[VR][AutoGrip] rejected model=\"%s\" fp=%08x correction=%.2f max=%.2f",
                    modelName.c_str(),
                    fingerprint,
                    correction.Length(),
                    maxCorrection);
            }
            return false;
        }

        const float blend = std::clamp(
            std::chrono::duration<float>(std::chrono::steady_clock::now() - lockedAt).count() / 0.18f,
            0.0f,
            1.0f);
        Vector finalOrigin = baseOrigin + correction * blend;
        QAngle finalAngles = pDrawInfo->angles;
        if (fullRotationAlignment)
        {
            QAngle solvedAngles{};
            if (!HooksViewmodelAutoGripMatrixAngles(solvedEntity, solvedAngles))
                return false;
            finalAngles.x += HooksViewmodelAutoGripWrapAngle(solvedAngles.x - finalAngles.x) * blend;
            finalAngles.y += HooksViewmodelAutoGripWrapAngle(solvedAngles.y - finalAngles.y) * blend;
            finalAngles.z += HooksViewmodelAutoGripWrapAngle(solvedAngles.z - finalAngles.z) * blend;
        }

        vr_vm_stabilize::Mat3x4 finalEntity{};
        vr_vm_stabilize::BuildFromOrgAngles(finalOrigin, finalAngles, finalEntity);
        vr_vm_stabilize::Mat3x4 baseInverse{};
        vr_vm_stabilize::Mat3x4 drawDelta{};
        vr_vm_stabilize::InvertTR(baseEntity, baseInverse);
        vr_vm_stabilize::Mul(finalEntity, baseInverse, drawDelta);

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_acquire) & ~1u;
        if (seqEven == 0)
            seqEven = 2;

        if (pBonesToWorldFinal)
        {
            vr_vm_stabilize::Mat3x4* alignedBones =
                vr_vm_stabilize::AllocStableBones(layout.numBones, seqEven);
            if (!alignedBones)
                return false;
            std::memcpy(
                alignedBones,
                pBonesToWorldFinal,
                static_cast<size_t>(layout.numBones) * sizeof(vr_vm_stabilize::Mat3x4));
            vr_vm_stabilize::ApplyDelta(drawDelta, alignedBones, layout.numBones);
            pBonesToWorldFinal = alignedBones;
        }

        drawInfoStorage = *pDrawInfo;
        drawInfoStorage.origin = finalOrigin;
        drawInfoStorage.angles = finalAngles;
        vr_vm_stabilize::Mat3x4* stableModelToWorld = vr_vm_stabilize::AllocStableBones(1, seqEven);
        if (stableModelToWorld)
        {
            *stableModelToWorld = finalEntity;
            drawInfoStorage.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(stableModelToWorld);
        }
        pDrawInfo = &drawInfoStorage;

        // Reaching this point means AutoGrip has successfully solved and applied
        // the main weapon model (arms/hands returned through the companion path
        // above). Queued DrawModelExecute does not reliably expose CBaseViewModel's
        // client class, so class-gating here would silently discard every corrected
        // ray and fall back to the old controller line.
        PublishViewmodelAutoGripAimPose(vr, drawDelta);

        vr_vm_stabilize::Mat3x4 localCorrection{};
        vr_vm_stabilize::Mul(baseInverse, finalEntity, localCorrection);
        vr_vm_stabilize::Mat3x4 normalizedLocalCorrection{};
        if (HooksViewmodelAutoGripNormalizeRigidMatrix(localCorrection, normalizedLocalCorrection))
        {
            std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
            auto& companion = s_ViewmodelAutoGripCompanionByVr[vr][pDrawInfo->entity_index];
            companion.weaponFingerprint = fingerprint;
            companion.localCorrection = normalizedLocalCorrection;
            companion.updatedAt = std::chrono::steady_clock::now();
            companion.valid = true;
        }

        if (vr->m_ViewmodelAutoGripAlignDebugLog)
        {
            bool logNow = false;
            {
                std::lock_guard<std::mutex> lock(s_ViewmodelAutoGripMutex);
                auto& entry = s_ViewmodelAutoGripByFingerprint[fingerprint];
                const auto now = std::chrono::steady_clock::now();
                if (entry.lastApplyLog.time_since_epoch().count() == 0 ||
                    std::chrono::duration<float>(now - entry.lastApplyLog).count() >= 0.5f)
                {
                    entry.lastApplyLog = now;
                    logNow = true;
                }
            }
            if (logNow)
            {
                Game::logMsg(
                    "[VR][AutoGrip] apply model=\"%s\" fp=%08x mode=%s blend=%.2f correction=(%.2f %.2f %.2f)",
                    modelName.c_str(),
                    fingerprint,
                    fullRotationAlignment ? "6dof" : "position",
                    blend,
                    correction.x,
                    correction.y,
                    correction.z);
            }
        }
        return true;
    }

    inline bool HooksCalibrationOriginIsValid(const Vector& value)
    {
        return std::isfinite(value.x) &&
            std::isfinite(value.y) &&
            std::isfinite(value.z) &&
            std::fabs(value.x) < 100000.0f &&
            std::fabs(value.y) < 100000.0f &&
            std::fabs(value.z) < 100000.0f;
    }

    inline void PublishMagazineInteractionCalibrationSnapshotFromDraw(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const char* className,
        bool sourceIsViewmodelClass,
        int entityIndex,
        const void* boneToWorld)
    {
        if (!vr)
            return;
        if (!vr->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed))
            return;
        if (!drawState || !boneToWorld || modelName.empty())
            return;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        const bool sourceIsViewmodelPath = HooksModelNameIsViewmodel(lowerModel);
        const bool sourceHasLooseViewmodelMarker = HooksModelNameHasLooseViewmodelMarker(lowerModel);
        const bool sourceIsArmsOrHands = HooksModelNameIsArmsOrHands(lowerModel);
        const int inferredModelWeaponId = MagazineInteractionInferWeaponIdFromViewmodelModelName(lowerModel);
        const int currentWeaponId = vr->m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed);
        const uint32_t renderFrameSeq = vr->m_RenderFrameSeq.load(std::memory_order_relaxed);
        if (sourceIsArmsOrHands)
            return;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
            return;
        if (numBones <= 0 || numBones > 512)
            return;

        const bool sourceLooksLikeWeaponBones = HooksCalibrationBoneNamesLookLikeWeapon(boneNames);
        if (!sourceIsViewmodelClass &&
            !sourceIsViewmodelPath &&
            !sourceHasLooseViewmodelMarker &&
            !sourceLooksLikeWeaponBones)
            return;

        const int weaponId = inferredModelWeaponId > 0
            ? inferredModelWeaponId
            : currentWeaponId > 0
            ? currentWeaponId
            : vr->m_MagazineInteractionWeaponId;

        std::vector<MagazineInteractionCalibrationBone> bones;
        bones.reserve(static_cast<size_t>(numBones));
        int recommendedMagazineBone = -1;
        int recommendedMagazineScore = 0;
        int recommendedBoltBone = -1;
        int recommendedBoltScore = 0;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(boneToWorld);
        for (int bone = 0; bone < numBones; ++bone)
        {
            MagazineInteractionCalibrationBone entry{};
            entry.index = bone;
            entry.parent =
                bone < static_cast<int>(boneParents.size())
                ? boneParents[static_cast<size_t>(bone)]
                : -1;
            entry.name =
                bone < static_cast<int>(boneNames.size())
                ? boneNames[static_cast<size_t>(bone)]
                : std::string();

            entry.magazineScore = MagazineInteractionWeaponIdIsShotgun(weaponId)
                ? ScoreMagazineInteractionShotgunShellBoneName(entry.name)
                : ScoreMagazineInteractionMagazineBoneName(entry.name);
            entry.boltScore = ScoreMagazineInteractionBoltBoneName(entry.name, weaponId);

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
            {
                const Vector origin = vr_vm_stabilize::GetOrigin(boneWorld);
                if (HooksCalibrationOriginIsValid(origin))
                {
                    entry.origin = origin;
                    entry.validOrigin = true;
                }
            }

            if (entry.magazineScore > recommendedMagazineScore)
            {
                recommendedMagazineScore = entry.magazineScore;
                recommendedMagazineBone = bone;
            }
            if (entry.boltScore > recommendedBoltScore)
            {
                recommendedBoltScore = entry.boltScore;
                recommendedBoltBone = bone;
            }

            bones.push_back(std::move(entry));
        }

        const uint32_t boneSignature = HooksBuildViewmodelBoneSignature(
            modelName,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);
        const uint32_t modelFingerprint = HooksBuildStudioHdrFingerprint(drawState, boneSignature);
        int sourceScore = 0;
        if (sourceIsViewmodelClass)
            sourceScore += 1000;
        if (sourceIsViewmodelPath)
            sourceScore += 300;
        if (sourceHasLooseViewmodelMarker && !sourceIsViewmodelPath)
            sourceScore += 150;
        if (sourceLooksLikeWeaponBones)
            sourceScore += 150;
        if (currentWeaponId > 0 && inferredModelWeaponId > 0 && currentWeaponId == inferredModelWeaponId)
            sourceScore += 500;
        if (recommendedMagazineBone >= 0)
            sourceScore += 50;
        if (recommendedBoltBone >= 0)
            sourceScore += 25;
        sourceScore += std::min(numBones, 160);

        vr->PublishMagazineInteractionCalibrationSnapshot(
            modelName.c_str(),
            className,
            modelFingerprint,
            boneSignature,
            renderFrameSeq,
            entityIndex,
            weaponId,
            inferredModelWeaponId,
            sourceScore,
            numBones,
            recommendedMagazineBone,
            recommendedBoltBone,
            sourceIsViewmodelClass,
            bones);
    }

    inline Vector HooksNormalizeVector(const Vector& value, const Vector& fallback)
    {
        const float length = value.Length();
        if (!(length > 0.000001f))
            return fallback;
        return value * (1.0f / length);
    }

    inline int ResolveMagazineInteractionWeaponIdForConfig(const VR* vr, int preferredWeaponId = 0)
    {
        if (preferredWeaponId > 0)
            return preferredWeaponId;

        if (!vr)
            return 0;

        if (vr->m_MagazineInteractionWeaponId > 0)
            return vr->m_MagazineInteractionWeaponId;

        return vr->m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed);
    }

    inline std::string ResolveMagazineInteractionProfileKeyForConfig(
        const VR* vr,
        const std::string& preferredProfileKey = std::string())
    {
        if (!preferredProfileKey.empty())
            return preferredProfileKey;
        if (!vr)
            return std::string();

        return HooksBuildMagazineInteractionProfileKey(
            vr->m_MagazineInteractionCurrentModelFingerprint.load(std::memory_order_relaxed),
            vr->m_MagazineInteractionCurrentBoneSignature.load(std::memory_order_relaxed));
    }

    template <typename TValue>
    inline bool FindMagazineInteractionProfileOverride(
        const VR* vr,
        const std::string& preferredProfileKey,
        const std::unordered_map<std::string, TValue>& profileOverrides,
        TValue& outValue)
    {
        const std::string profileKey = ResolveMagazineInteractionProfileKeyForConfig(vr, preferredProfileKey);
        if (profileKey.empty())
            return false;

        const auto profileIt = profileOverrides.find(profileKey);
        if (profileIt == profileOverrides.end())
            return false;

        outValue = profileIt->second;
        return true;
    }

    template <typename TValue>
    inline bool FindMagazineInteractionProfileOrWeaponOverride(
        const VR* vr,
        int preferredWeaponId,
        const std::string& preferredProfileKey,
        const std::unordered_map<std::string, TValue>& profileOverrides,
        const std::unordered_map<int, TValue>& weaponOverrides,
        TValue& outValue)
    {
        if (FindMagazineInteractionProfileOverride(vr, preferredProfileKey, profileOverrides, outValue))
            return true;

        const int weaponId = ResolveMagazineInteractionWeaponIdForConfig(vr, preferredWeaponId);
        if (weaponId <= 0)
            return false;

        const auto weaponIt = weaponOverrides.find(weaponId);
        if (weaponIt == weaponOverrides.end())
            return false;

        outValue = weaponIt->second;
        return true;
    }

    inline Vector ResolveMagazineInteractionMagazineBoxHalfExtentsMeters(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionMagazineBoxHalfExtentsMeters;
        FindMagazineInteractionProfileOrWeaponOverride(
            vr,
            0,
            std::string(),
            vr->m_MagazineInteractionMagazineBoxHalfExtentsMetersProfileOverrides,
            vr->m_MagazineInteractionMagazineBoxHalfExtentsMetersOverrides,
            value);
        value.x = std::clamp(value.x, 0.0f, 0.50f);
        value.y = std::clamp(value.y, 0.0f, 0.50f);
        value.z = std::clamp(value.z, 0.0f, 0.50f);
        return value;
    }

    inline Vector ResolveMagazineInteractionMagazineBoxLocalOffsetMeters(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionMagazineBoxLocalOffsetMeters;
        FindMagazineInteractionProfileOrWeaponOverride(
            vr,
            0,
            std::string(),
            vr->m_MagazineInteractionMagazineBoxLocalOffsetMetersProfileOverrides,
            vr->m_MagazineInteractionMagazineBoxLocalOffsetMetersOverrides,
            value);
        value.x = std::clamp(value.x, -0.50f, 0.50f);
        value.y = std::clamp(value.y, -0.50f, 0.50f);
        value.z = std::clamp(value.z, -0.50f, 0.50f);
        return value;
    }

    inline Vector ResolveMagazineInteractionMagazineBoxLocalRotationOffsetDeg(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionMagazineBoxLocalRotationOffsetDeg;
        FindMagazineInteractionProfileOrWeaponOverride(
            vr,
            0,
            std::string(),
            vr->m_MagazineInteractionMagazineBoxLocalRotationOffsetDegProfileOverrides,
            vr->m_MagazineInteractionMagazineBoxLocalRotationOffsetDegOverrides,
            value);
        value.x = std::clamp(value.x, -180.0f, 180.0f);
        value.y = std::clamp(value.y, -180.0f, 180.0f);
        value.z = std::clamp(value.z, -180.0f, 180.0f);
        return value;
    }

    inline Vector ResolveMagazineInteractionSocketCaptureBoxHalfExtentsMeters(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionSocketCaptureBoxHalfExtentsMeters;
        FindMagazineInteractionProfileOrWeaponOverride(
            vr,
            0,
            std::string(),
            vr->m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersProfileOverrides,
            vr->m_MagazineInteractionSocketCaptureBoxHalfExtentsMetersOverrides,
            value);
        value.x = std::clamp(value.x, 0.0f, 0.50f);
        value.y = std::clamp(value.y, 0.0f, 0.50f);
        value.z = std::clamp(value.z, 0.0f, 0.50f);
        return value;
    }

    inline Vector ResolveMagazineInteractionSocketCaptureBoxLocalOffsetMeters(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionSocketCaptureBoxLocalOffsetMeters;
        FindMagazineInteractionProfileOrWeaponOverride(
            vr,
            0,
            std::string(),
            vr->m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersProfileOverrides,
            vr->m_MagazineInteractionSocketCaptureBoxLocalOffsetMetersOverrides,
            value);
        value.x = std::clamp(value.x, -0.50f, 0.50f);
        value.y = std::clamp(value.y, -0.50f, 0.50f);
        value.z = std::clamp(value.z, -0.50f, 0.50f);
        return value;
    }

    inline Vector ResolveMagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg;
        FindMagazineInteractionProfileOrWeaponOverride(
            vr,
            0,
            std::string(),
            vr->m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegProfileOverrides,
            vr->m_MagazineInteractionSocketCaptureBoxLocalRotationOffsetDegOverrides,
            value);
        value.x = std::clamp(value.x, -180.0f, 180.0f);
        value.y = std::clamp(value.y, -180.0f, 180.0f);
        value.z = std::clamp(value.z, -180.0f, 180.0f);
        return value;
    }

    inline Vector ResolveMagazineInteractionBoltBoxHalfExtentsMeters(
        const VR* vr,
        int preferredWeaponId,
        const std::string& preferredProfileKey)
    {
        if (!vr)
            return Vector(0.045f, 0.035f, 0.035f);

        Vector value = vr->m_MagazineInteractionBoltBoxHalfExtentsMeters;
        const bool usedProfileOverride = FindMagazineInteractionProfileOverride(
            vr,
            preferredProfileKey,
            vr->m_MagazineInteractionBoltBoxHalfExtentsMetersProfileOverrides,
            value);

        const int weaponId = ResolveMagazineInteractionWeaponIdForConfig(vr, preferredWeaponId);
        if (!usedProfileOverride && weaponId > 0)
        {
            const auto halfIt = vr->m_MagazineInteractionBoltBoxHalfExtentsMetersOverrides.find(weaponId);
            if (halfIt != vr->m_MagazineInteractionBoltBoxHalfExtentsMetersOverrides.end())
                value = halfIt->second;
        }

        value.x = std::clamp(value.x, 0.005f, 0.25f);
        value.y = std::clamp(value.y, 0.005f, 0.25f);
        value.z = std::clamp(value.z, 0.005f, 0.25f);
        return value;
    }

    inline Vector ResolveMagazineInteractionBoltPullAxisLocal(
        const VR* vr,
        int preferredWeaponId,
        const std::string& preferredProfileKey,
        bool& outUsedOverride)
    {
        outUsedOverride = false;
        if (!vr)
            return Vector(0.0f, 1.0f, 0.0f);

        Vector profileAxis{};
        if (FindMagazineInteractionProfileOverride(
            vr,
            preferredProfileKey,
            vr->m_MagazineInteractionBoltPullAxisLocalProfileOverrides,
            profileAxis))
        {
            outUsedOverride = true;
            return profileAxis;
        }

        const int weaponId = ResolveMagazineInteractionWeaponIdForConfig(vr, preferredWeaponId);
        if (weaponId > 0)
        {
            const auto axisIt = vr->m_MagazineInteractionBoltPullAxisLocalOverrides.find(weaponId);
            if (axisIt != vr->m_MagazineInteractionBoltPullAxisLocalOverrides.end())
            {
                outUsedOverride = true;
                return axisIt->second;
            }
        }

        return vr->m_MagazineInteractionBoltPullAxisLocal;
    }

    inline Vector ResolveMagazineInteractionBoltBoxLocalOffsetMeters(
        const VR* vr,
        int preferredWeaponId,
        const std::string& preferredProfileKey)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        Vector value = vr->m_MagazineInteractionBoltBoxLocalOffsetMeters;
        const bool usedProfileOverride = FindMagazineInteractionProfileOverride(
            vr,
            preferredProfileKey,
            vr->m_MagazineInteractionBoltBoxLocalOffsetMetersProfileOverrides,
            value);

        const int weaponId = ResolveMagazineInteractionWeaponIdForConfig(vr, preferredWeaponId);
        if (!usedProfileOverride && weaponId > 0)
        {
            const auto offsetIt = vr->m_MagazineInteractionBoltBoxLocalOffsetMetersOverrides.find(weaponId);
            if (offsetIt != vr->m_MagazineInteractionBoltBoxLocalOffsetMetersOverrides.end())
                value = offsetIt->second;
        }

        value.x = std::clamp(value.x, -0.25f, 0.25f);
        value.y = std::clamp(value.y, -0.25f, 0.25f);
        value.z = std::clamp(value.z, -0.25f, 0.25f);
        return value;
    }

    inline float ResolveMagazineInteractionBoltPullDistanceMeters(
        const VR* vr,
        int preferredWeaponId = 0,
        const std::string& preferredProfileKey = std::string())
    {
        if (!vr)
            return 0.055f;

        float value = vr->m_MagazineInteractionBoltPullDistanceMeters;
        const bool usedProfileOverride = FindMagazineInteractionProfileOverride(
            vr,
            preferredProfileKey,
            vr->m_MagazineInteractionBoltPullDistanceMetersProfileOverrides,
            value);

        const int weaponId = ResolveMagazineInteractionWeaponIdForConfig(vr, preferredWeaponId);
        if (!usedProfileOverride && weaponId > 0)
        {
            const auto it = vr->m_MagazineInteractionBoltPullDistanceMetersOverrides.find(weaponId);
            if (it != vr->m_MagazineInteractionBoltPullDistanceMetersOverrides.end())
                value = it->second;
        }
        return std::clamp(value, 0.0f, 0.25f);
    }

    inline Vector BuildMagazineInteractionBoltPullAxisWorld(
        VR* vr,
        const std::string& modelName,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const std::vector<int>& boneParents,
        int weaponId,
        int boltBone,
        const vr_vm_stabilize::Mat3x4& boltWorld,
        const std::string& profileKey,
        const void* pModelToWorld)
    {
        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        bool usedAxisOverride = false;
        Vector profileLocalAxis{};
        const bool usedProfileAxisOverride = FindMagazineInteractionProfileOverride(
            vr,
            profileKey,
            vr->m_MagazineInteractionBoltPullAxisLocalProfileOverrides,
            profileLocalAxis);
        Vector configuredLocalAxis = ResolveMagazineInteractionBoltPullAxisLocal(vr, weaponId, profileKey, usedAxisOverride);
        const bool legacyM16Axis =
            !usedAxisOverride &&
            lowerModel.find("models/v_models/v_rifle.mdl") != std::string::npos &&
            std::fabs(configuredLocalAxis.x + 1.0f) < 0.001f &&
            std::fabs(configuredLocalAxis.y) < 0.001f &&
            std::fabs(configuredLocalAxis.z) < 0.001f;
        if (legacyM16Axis)
            configuredLocalAxis = Vector(0.0f, 1.0f, 0.0f);

        const Vector localAxis = HooksNormalizeVector(
            configuredLocalAxis,
            Vector(0.0f, 1.0f, 0.0f));

        auto logAxis = [&](const char* source, const Vector& axis)
            {
                if (!ShouldLogMagazineBoxDiagnostics(vr))
                    return;

                static std::mutex s_axisLogMutex;
                static std::unordered_set<std::string> s_loggedAxisSources;
                char key[256];
                std::snprintf(
                    key,
                    sizeof(key),
                    "%s|%s|%.2f,%.2f,%.2f|%d|%d",
                    lowerModel.c_str(),
                    source ? source : "unknown",
                    localAxis.x,
                    localAxis.y,
                    localAxis.z,
                    legacyM16Axis ? 1 : 0,
                    usedAxisOverride ? 1 : 0);
                {
                    std::lock_guard<std::mutex> lock(s_axisLogMutex);
                    if (!s_loggedAxisSources.insert(key).second)
                        return;
                }
                Game::logMsg(
                    "[VR][MagazineBolt] pull axis model=%s source=%s local=(%.2f %.2f %.2f) axis=(%.3f %.3f %.3f)%s%s",
                    modelName.c_str(),
                    source ? source : "unknown",
                    localAxis.x,
                    localAxis.y,
                    localAxis.z,
                    axis.x,
                    axis.y,
                    axis.z,
                    legacyM16Axis ? " legacyM16Axis=1" : "",
                    usedAxisOverride ? " override=1" : "");
            };

        auto axisFromMatrix = [&](const vr_vm_stabilize::Mat3x4& matrix) -> Vector
            {
                Vector axis = HooksTransformVector(matrix, localAxis);
                axis = HooksNormalizeVector(axis, Vector(0.0f, 0.0f, 0.0f));
                return (axis.Length() > 0.0001f) ? axis : Vector(0.0f, 0.0f, 0.0f);
            };

        if (usedProfileAxisOverride)
        {
            const Vector profileBoltAxis = axisFromMatrix(boltWorld) * -1.0f;
            if (profileBoltAxis.Length() > 0.0001f)
            {
                logAxis("profile-bolt-local", profileBoltAxis);
                return profileBoltAxis;
            }

            if (pModelToWorld)
            {
                vr_vm_stabilize::Mat3x4 modelWorld{};
                if (vr_vm_stabilize::SafeRead(
                    reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pModelToWorld),
                    modelWorld))
                {
                    const Vector axis = axisFromMatrix(modelWorld);
                    if (axis.Length() > 0.0001f)
                    {
                        logAxis("profile-model-local", axis);
                        return axis;
                    }
                }
            }
        }

        if (sourceBones &&
            boltBone >= 0 &&
            boltBone < numBones &&
            static_cast<int>(boneParents.size()) >= numBones)
        {
            const int parentBone = boneParents[static_cast<size_t>(boltBone)];
            if (parentBone >= 0 && parentBone < numBones && parentBone != boltBone)
            {
                vr_vm_stabilize::Mat3x4 parentWorld{};
                if (vr_vm_stabilize::SafeRead(sourceBones + parentBone, parentWorld))
                {
                    const Vector axis = axisFromMatrix(parentWorld);
                    if (axis.Length() > 0.0001f)
                    {
                        logAxis("bolt-parent-local", axis);
                        return axis;
                    }
                }
            }
        }

        if (pModelToWorld)
        {
            vr_vm_stabilize::Mat3x4 modelWorld{};
            if (vr_vm_stabilize::SafeRead(
                reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pModelToWorld),
                modelWorld))
            {
                const Vector axis = axisFromMatrix(modelWorld);
                if (axis.Length() > 0.0001f)
                {
                    logAxis("model-local", axis);
                    return axis;
                }
            }
        }

        const Vector fallbackAxis = axisFromMatrix(boltWorld);
        if (fallbackAxis.Length() > 0.0001f)
        {
            logAxis("bolt-local", fallbackAxis);
            return fallbackAxis;
        }
        return Vector(0.0f, 1.0f, 0.0f);
    }

    inline bool HooksBoneIsDescendantOf(
        const std::vector<int>& boneParents,
        int bone,
        int ancestorBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (bone < 0 || bone >= numBones || ancestorBone < 0 || ancestorBone >= numBones)
            return false;

        int current = bone;
        for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
        {
            if (current == ancestorBone)
                return true;
            current = boneParents[static_cast<size_t>(current)];
        }
        return false;
    }

    inline int HooksBoneDescendantDepth(
        const std::vector<int>& boneParents,
        int bone,
        int ancestorBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (bone < 0 || bone >= numBones || ancestorBone < 0 || ancestorBone >= numBones)
            return -1;

        int current = bone;
        for (int depth = 0; depth < numBones && current >= 0 && current < numBones; ++depth)
        {
            if (current == ancestorBone)
                return depth;
            current = boneParents[static_cast<size_t>(current)];
        }
        return -1;
    }

    inline bool HooksMagazineBoxCanSampleBoneName(const std::string& lowerName, int descendantDepth)
    {
        if (lowerName.empty())
            return false;

        if (MagazineInteractionNameContains(lowerName, "finger") ||
            MagazineInteractionNameContains(lowerName, "hand") ||
            MagazineInteractionNameContains(lowerName, "bip01") ||
            MagazineInteractionNameContains(lowerName, "attach") ||
            MagazineInteractionNameContains(lowerName, "muzzle") ||
            MagazineInteractionNameContains(lowerName, "eject") ||
            MagazineInteractionNameContains(lowerName, "release") ||
            MagazineInteractionNameContains(lowerName, "realease") ||
            MagazineInteractionNameContains(lowerName, "magrel") ||
            MagazineInteractionNameContains(lowerName, "button") ||
            MagazineInteractionNameContains(lowerName, "trigger") ||
            MagazineInteractionNameContains(lowerName, "safety") ||
            MagazineInteractionNameContains(lowerName, "bolt") ||
            MagazineInteractionNameContains(lowerName, "slide") ||
            MagazineInteractionNameContains(lowerName, "charger") ||
            MagazineInteractionNameContains(lowerName, "handle") ||
            MagazineInteractionNameContains(lowerName, "barrel") ||
            MagazineInteractionNameContains(lowerName, "stock") ||
            MagazineInteractionNameContains(lowerName, "hammer"))
        {
            return false;
        }

        if (ScoreMagazineInteractionMagazineBoneName(lowerName) > 0)
            return true;

        const bool hasClipOrMag =
            MagazineInteractionNameContains(lowerName, "clip") ||
            MagazineInteractionNameContains(lowerName, "mag");
        const bool hasAmmoPart =
            MagazineInteractionNameContains(lowerName, "bullet") ||
            MagazineInteractionNameContains(lowerName, "round") ||
            MagazineInteractionNameContains(lowerName, "ammo");
        return descendantDepth <= 2 && hasClipOrMag && hasAmmoPart;
    }

    inline int HooksDominantAxis(const Vector& value)
    {
        const float ax = std::fabs(value.x);
        const float ay = std::fabs(value.y);
        const float az = std::fabs(value.z);
        if (ay >= ax && ay >= az)
            return 1;
        if (az >= ax && az >= ay)
            return 2;
        return 0;
    }

    inline float HooksVectorComponent(const Vector& value, int axis)
    {
        switch (std::clamp(axis, 0, 2))
        {
        case 0:
            return value.x;
        case 1:
            return value.y;
        default:
            return value.z;
        }
    }

    inline Vector HooksMatrixAxis(const vr_vm_stabilize::Mat3x4& matrix, int axis)
    {
        const int clampedAxis = std::clamp(axis, 0, 2);
        return Vector(
            matrix.m[0][clampedAxis],
            matrix.m[1][clampedAxis],
            matrix.m[2][clampedAxis]);
    }

    inline bool HooksMatrixLooksUsable(const vr_vm_stabilize::Mat3x4& matrix)
    {
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                if (!std::isfinite(matrix.m[row][col]))
                    return false;
            }
        }
        const Vector origin = vr_vm_stabilize::GetOrigin(matrix);
        return std::isfinite(origin.x) &&
            std::isfinite(origin.y) &&
            std::isfinite(origin.z) &&
            HooksMatrixAxis(matrix, 0).LengthSqr() > 0.000001f &&
            HooksMatrixAxis(matrix, 1).LengthSqr() > 0.000001f &&
            HooksMatrixAxis(matrix, 2).LengthSqr() > 0.000001f;
    }

    inline void HooksSetMatrixAxis(vr_vm_stabilize::Mat3x4& matrix, int axis, const Vector& value)
    {
        const int clampedAxis = std::clamp(axis, 0, 2);
        matrix.m[0][clampedAxis] = value.x;
        matrix.m[1][clampedAxis] = value.y;
        matrix.m[2][clampedAxis] = value.z;
    }

    inline void ApplyMagazineInteractionMagazineBoxCalibrationAdjustments(
        VR* vr,
        vr_vm_stabilize::Mat3x4& boxWorld,
        Vector& mins,
        Vector& maxs)
    {
        if (!vr)
            return;

        const float scale = (std::isfinite(vr->m_VRScale) && vr->m_VRScale > 0.001f) ? vr->m_VRScale : 43.2f;
        const Vector configuredHalfMeters = ResolveMagazineInteractionMagazineBoxHalfExtentsMeters(vr);
        const Vector localOffsetMeters = ResolveMagazineInteractionMagazineBoxLocalOffsetMeters(vr);
        const Vector rotationDeg = ResolveMagazineInteractionMagazineBoxLocalRotationOffsetDeg(vr);

        const Vector sourceHalf = (maxs - mins) * 0.5f;
        Vector center = (mins + maxs) * 0.5f;
        center = center + localOffsetMeters * scale;

        const Vector half(
            configuredHalfMeters.x > 0.0001f ? configuredHalfMeters.x * scale : std::max(0.001f, sourceHalf.x),
            configuredHalfMeters.y > 0.0001f ? configuredHalfMeters.y * scale : std::max(0.001f, sourceHalf.y),
            configuredHalfMeters.z > 0.0001f ? configuredHalfMeters.z * scale : std::max(0.001f, sourceHalf.z));
        mins = center - half;
        maxs = center + half;

        if (std::fabs(rotationDeg.x) <= 0.0001f &&
            std::fabs(rotationDeg.y) <= 0.0001f &&
            std::fabs(rotationDeg.z) <= 0.0001f)
        {
            return;
        }

        Vector axisX = HooksNormalizeVector(HooksMatrixAxis(boxWorld, 0), Vector(1.0f, 0.0f, 0.0f));
        Vector axisY = HooksNormalizeVector(HooksMatrixAxis(boxWorld, 1), Vector(0.0f, 1.0f, 0.0f));
        Vector axisZ = HooksNormalizeVector(HooksMatrixAxis(boxWorld, 2), Vector(0.0f, 0.0f, 1.0f));

        if (std::fabs(rotationDeg.x) > 0.0001f)
        {
            axisY = VectorRotate(axisY, axisX, rotationDeg.x);
            axisZ = VectorRotate(axisZ, axisX, rotationDeg.x);
        }
        if (std::fabs(rotationDeg.y) > 0.0001f)
        {
            axisX = VectorRotate(axisX, axisY, rotationDeg.y);
            axisZ = VectorRotate(axisZ, axisY, rotationDeg.y);
        }
        if (std::fabs(rotationDeg.z) > 0.0001f)
        {
            axisX = VectorRotate(axisX, axisZ, rotationDeg.z);
            axisY = VectorRotate(axisY, axisZ, rotationDeg.z);
        }

        axisX = HooksNormalizeVector(axisX, Vector(1.0f, 0.0f, 0.0f));
        axisY = HooksNormalizeVector(axisY, Vector(0.0f, 1.0f, 0.0f));
        axisZ = HooksNormalizeVector(CrossProduct(axisX, axisY), axisZ);
        axisY = HooksNormalizeVector(CrossProduct(axisZ, axisX), axisY);
        HooksSetMatrixAxis(boxWorld, 0, axisX);
        HooksSetMatrixAxis(boxWorld, 1, axisY);
        HooksSetMatrixAxis(boxWorld, 2, axisZ);
    }

    inline vr_vm_stabilize::Mat3x4 HooksBuildLocalMeterTransform(
        float sourceUnitsPerMeter,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = localRotationOffsetDeg.x * kDegToRad;
        const float ry = localRotationOffsetDeg.y * kDegToRad;
        const float rz = localRotationOffsetDeg.z * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);

        vr_vm_stabilize::Mat3x4 local{};
        local.m[0][0] = cz * cy;
        local.m[0][1] = cz * sy * sx - sz * cx;
        local.m[0][2] = cz * sy * cx + sz * sx;
        local.m[1][0] = sz * cy;
        local.m[1][1] = sz * sy * sx + cz * cx;
        local.m[1][2] = sz * sy * cx - cz * sx;
        local.m[2][0] = -sy;
        local.m[2][1] = cy * sx;
        local.m[2][2] = cy * cx;
        local.m[0][3] = localPositionOffsetMeters.x * sourceUnitsPerMeter;
        local.m[1][3] = localPositionOffsetMeters.y * sourceUnitsPerMeter;
        local.m[2][3] = localPositionOffsetMeters.z * sourceUnitsPerMeter;
        return local;
    }

    inline void ApplyMagazineInteractionSocketCaptureCalibrationAdjustments(
        VR* vr,
        vr_vm_stabilize::Mat3x4& boxWorld,
        Vector& mins,
        Vector& maxs)
    {
        if (!vr)
            return;

        const float scale = (std::isfinite(vr->m_VRScale) && vr->m_VRScale > 0.001f) ? vr->m_VRScale : 43.2f;
        const Vector configuredHalfMeters = ResolveMagazineInteractionSocketCaptureBoxHalfExtentsMeters(vr);
        const Vector localOffsetMeters = ResolveMagazineInteractionSocketCaptureBoxLocalOffsetMeters(vr);
        const Vector rotationDeg = ResolveMagazineInteractionSocketCaptureBoxLocalRotationOffsetDeg(vr);

        const Vector sourceHalf = (maxs - mins) * 0.5f;
        const Vector centerWorld = HooksTransformPoint(boxWorld, (mins + maxs) * 0.5f);
        boxWorld.m[0][3] = centerWorld.x;
        boxWorld.m[1][3] = centerWorld.y;
        boxWorld.m[2][3] = centerWorld.z;

        const vr_vm_stabilize::Mat3x4 local =
            HooksBuildLocalMeterTransform(scale, localOffsetMeters, rotationDeg);
        vr_vm_stabilize::Mat3x4 adjustedWorld{};
        vr_vm_stabilize::Mul(boxWorld, local, adjustedWorld);
        boxWorld = adjustedWorld;

        const Vector half(
            configuredHalfMeters.x > 0.0001f ? configuredHalfMeters.x * scale : std::max(0.001f, sourceHalf.x),
            configuredHalfMeters.y > 0.0001f ? configuredHalfMeters.y * scale : std::max(0.001f, sourceHalf.y),
            configuredHalfMeters.z > 0.0001f ? configuredHalfMeters.z * scale : std::max(0.001f, sourceHalf.z));
        mins = Vector(-half.x, -half.y, -half.z);
        maxs = Vector(half.x, half.y, half.z);
    }

    inline void ApplyMagazineInteractionBoltBoxCalibrationAdjustments(
        VR* vr,
        vr_vm_stabilize::Mat3x4& boxWorld,
        Vector& mins,
        Vector& maxs)
    {
        if (!vr)
            return;

        const float scale = (std::isfinite(vr->m_VRScale) && vr->m_VRScale > 0.001f) ? vr->m_VRScale : 43.2f;
        const Vector halfMeters = ResolveMagazineInteractionBoltBoxHalfExtentsMeters(vr, 0, std::string());
        const Vector offsetMeters = ResolveMagazineInteractionBoltBoxLocalOffsetMeters(vr, 0, std::string());

        const Vector center = offsetMeters * scale;
        const Vector half = halfMeters * scale;
        mins = center - half;
        maxs = center + half;
        (void)boxWorld;
    }

    inline bool HooksProjectBasisReference(
        const Vector& candidate,
        const Vector& lockedAxis,
        Vector& outReference)
    {
        Vector projected = candidate - lockedAxis * DotProduct(candidate, lockedAxis);
        projected = HooksNormalizeVector(projected, Vector(0.0f, 0.0f, 0.0f));
        if (projected.Length() <= 0.0001f)
            return false;

        outReference = projected;
        return true;
    }

    inline bool BuildMagazineBoxBasisFromParentOffset(
        const vr_vm_stabilize::Mat3x4& magazineWorld,
        const vr_vm_stabilize::Mat3x4& parentWorld,
        const void* pModelToWorld,
        const Vector& insertionAxisLocal,
        int lengthAxis,
        vr_vm_stabilize::Mat3x4& outWorld)
    {
        const int clampedLengthAxis = std::clamp(lengthAxis, 0, 2);
        Vector extractionAxis = vr_vm_stabilize::GetOrigin(magazineWorld) -
            vr_vm_stabilize::GetOrigin(parentWorld);
        extractionAxis = HooksNormalizeVector(extractionAxis, Vector(0.0f, 0.0f, 0.0f));
        if (extractionAxis.Length() <= 0.0001f)
            return false;

        float localSign = HooksVectorComponent(insertionAxisLocal, clampedLengthAxis);
        if (std::fabs(localSign) <= 0.0001f)
            localSign = (clampedLengthAxis == 1) ? -1.0f : 1.0f;
        const Vector lockedAxis = extractionAxis * ((localSign < 0.0f) ? -1.0f : 1.0f);

        vr_vm_stabilize::Mat3x4 modelWorld{};
        const bool hasModelWorld =
            pModelToWorld &&
            vr_vm_stabilize::SafeRead(reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pModelToWorld), modelWorld);

        Vector referenceAxis(0.0f, 0.0f, 0.0f);
        const int preferredReferenceAxis = (clampedLengthAxis == 0) ? 1 : 0;
        const int axisOrder[3] =
        {
            preferredReferenceAxis,
            (preferredReferenceAxis + 1) % 3,
            (preferredReferenceAxis + 2) % 3
        };

        auto tryMatrixAxes = [&](const vr_vm_stabilize::Mat3x4& matrix) -> bool
            {
                for (int axis : axisOrder)
                {
                    if (axis == clampedLengthAxis)
                        continue;
                    if (HooksProjectBasisReference(HooksMatrixAxis(matrix, axis), lockedAxis, referenceAxis))
                        return true;
                }
                return false;
            };

        if (!(hasModelWorld && tryMatrixAxes(modelWorld)) &&
            !tryMatrixAxes(parentWorld) &&
            !tryMatrixAxes(magazineWorld))
        {
            const Vector fallbackAxes[3] =
            {
                Vector(0.0f, 0.0f, 1.0f),
                Vector(0.0f, 1.0f, 0.0f),
                Vector(1.0f, 0.0f, 0.0f)
            };
            bool foundFallback = false;
            for (const Vector& fallbackAxis : fallbackAxes)
            {
                if (HooksProjectBasisReference(fallbackAxis, lockedAxis, referenceAxis))
                {
                    foundFallback = true;
                    break;
                }
            }
            if (!foundFallback)
                return false;
        }

        Vector axisX(1.0f, 0.0f, 0.0f);
        Vector axisY(0.0f, 1.0f, 0.0f);
        Vector axisZ(0.0f, 0.0f, 1.0f);
        if (clampedLengthAxis == 0)
        {
            axisX = lockedAxis;
            axisY = referenceAxis;
            axisZ = HooksNormalizeVector(CrossProduct(axisX, axisY), Vector(0.0f, 0.0f, 0.0f));
            axisY = HooksNormalizeVector(CrossProduct(axisZ, axisX), Vector(0.0f, 0.0f, 0.0f));
        }
        else if (clampedLengthAxis == 1)
        {
            axisY = lockedAxis;
            axisX = referenceAxis;
            axisZ = HooksNormalizeVector(CrossProduct(axisX, axisY), Vector(0.0f, 0.0f, 0.0f));
            axisX = HooksNormalizeVector(CrossProduct(axisY, axisZ), Vector(0.0f, 0.0f, 0.0f));
        }
        else
        {
            axisZ = lockedAxis;
            axisX = referenceAxis;
            axisY = HooksNormalizeVector(CrossProduct(axisZ, axisX), Vector(0.0f, 0.0f, 0.0f));
            axisX = HooksNormalizeVector(CrossProduct(axisY, axisZ), Vector(0.0f, 0.0f, 0.0f));
        }

        if (axisX.Length() <= 0.0001f || axisY.Length() <= 0.0001f || axisZ.Length() <= 0.0001f)
            return false;

        outWorld = magazineWorld;
        HooksSetMatrixAxis(outWorld, 0, axisX);
        HooksSetMatrixAxis(outWorld, 1, axisY);
        HooksSetMatrixAxis(outWorld, 2, axisZ);
        return true;
    }

    inline bool TryGetOfficialMagazineBoxProfile(
        const std::string& lowerModel,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount,
        int& outLengthAxis)
    {
        outSampleCount = 1;
        outLengthAxis = 1;

        auto useProfile = [&](const char* modelPath, const Vector& mins, const Vector& maxs, int axis) -> bool
            {
                if (lowerModel.find(modelPath) == std::string::npos)
                    return false;
                outMins = mins - padding;
                outMaxs = maxs + padding;
                outLengthAxis = axis;
                return true;
            };

        if (useProfile("models/v_models/v_pistol.mdl", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;
        if (useProfile("models/v_models/v_pistola.mdl", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;
        if (useProfile("models/v_models/v_pistolb.mdl", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;
        if (useProfile("models/v_models/v_dual_pistola.mdl", Vector(-0.66f, -4.66f, -0.42f), Vector(0.72f, 1.35f, 1.99f), 1)) return true;
        if (useProfile("models/v_models/v_dual_pistol.mdl", Vector(-0.66f, -4.66f, -0.42f), Vector(0.72f, 1.35f, 1.99f), 1)) return true;
        if (useProfile("models/v_models/v_dual_pistols.mdl", Vector(-0.66f, -4.66f, -0.42f), Vector(0.72f, 1.35f, 1.99f), 1)) return true;
        if (useProfile("models/v_models/v_desert_eagle.mdl", Vector(-0.83f, -5.93f, -0.98f), Vector(0.56f, 0.59f, 3.11f), 1)) return true;
        if (useProfile("models/v_models/v_pistol_magnum.mdl", Vector(-0.83f, -5.93f, -0.98f), Vector(0.56f, 0.59f, 3.11f), 1)) return true;
        if (useProfile("models/v_models/v_pistol", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;

        if (useProfile("models/v_models/v_smg.mdl", Vector(-1.17f, -8.29f, -0.86f), Vector(1.17f, 0.09f, 0.86f), 1)) return true;
        if (useProfile("models/v_models/v_silenced_smg.mdl", Vector(-0.68f, -7.70f, -0.79f), Vector(0.52f, 1.05f, 0.78f), 1)) return true;
        if (useProfile("models/v_models/v_smg_mp5.mdl", Vector(-0.75f, -8.80f, -1.05f), Vector(0.75f, 1.10f, 1.05f), 1)) return true;

        if (useProfile("models/v_models/v_rifle.mdl", Vector(-0.90f, -8.40f, -1.10f), Vector(0.90f, 1.25f, 1.15f), 1)) return true;
        if (useProfile("models/v_models/v_rifle_ak47.mdl", Vector(-0.65f, -9.53f, -0.69f), Vector(0.63f, 1.34f, 7.11f), 1)) return true;
        if (useProfile("models/v_models/v_desert_rifle.mdl", Vector(-1.81f, -4.73f, -0.73f), Vector(1.69f, 1.61f, 0.72f), 1)) return true;
        if (useProfile("models/v_models/v_rif_sg552.mdl", Vector(-1.05f, -8.70f, -1.30f), Vector(1.05f, 1.25f, 1.30f), 1)) return true;

        if (useProfile("models/v_models/v_huntingrifle.mdl", Vector(-1.17f, -3.11f, -0.86f), Vector(1.17f, 2.23f, 0.86f), 1)) return true;
        if (useProfile("models/v_models/v_sniper_military.mdl", Vector(-0.79f, -5.30f, -0.09f), Vector(0.86f, 2.80f, 5.01f), 1)) return true;
        if (useProfile("models/v_models/v_snip_scout.mdl", Vector(-0.96f, -1.84f, -1.21f), Vector(0.47f, 0.18f, 4.64f), 2)) return true;
        if (useProfile("models/v_models/v_snip_awp.mdl", Vector(-0.66f, -2.56f, -0.97f), Vector(0.67f, 0.92f, 2.37f), 1)) return true;

        if (useProfile("models/v_models/v_pumpshotgun.mdl", Vector(-1.17f, -2.05f, -0.09f), Vector(1.17f, 3.30f, 2.30f), 1)) return true;
        if (useProfile("models/v_models/v_shotgun_chrome.mdl", Vector(-1.17f, -2.05f, -0.09f), Vector(1.17f, 3.30f, 2.30f), 1)) return true;
        if (useProfile("models/v_models/v_autoshotgun.mdl", Vector(-1.17f, -8.29f, -0.86f), Vector(1.17f, 0.09f, 0.86f), 1)) return true;
        if (useProfile("models/v_models/v_shotgun_spas.mdl", Vector(-0.61f, -0.96f, -1.50f), Vector(0.62f, 0.80f, 1.57f), 2)) return true;

        if (useProfile("models/v_models/v_m60.mdl", Vector(-4.20f, -3.20f, -6.40f), Vector(4.20f, 5.40f, 1.60f), 2)) return true;
        if (useProfile("models/v_models/v_grenade_launcher.mdl", Vector(-1.34f, -0.96f, -0.24f), Vector(1.03f, 1.43f, 5.60f), 2)) return true;

        return false;
    }

    inline bool MagazineBoxOfficialProfileExists(const std::string& lowerModel)
    {
        Vector mins;
        Vector maxs;
        int sampleCount = 0;
        int lengthAxis = 0;
        return TryGetOfficialMagazineBoxProfile(
            lowerModel,
            Vector(0.0f, 0.0f, 0.0f),
            mins,
            maxs,
            sampleCount,
            lengthAxis);
    }

    inline int FindMagazineBoxOfficialProfileFallbackBone(
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        bool logDiagnostics)
    {
        if (!MagazineBoxOfficialProfileExists(lowerModel))
            return -1;

        auto canUseFallbackName = [&](const std::string& lowerName)
            {
                if (lowerName.empty())
                    return false;

                if (MagazineInteractionNameContains(lowerName, "finger") ||
                    MagazineInteractionNameContains(lowerName, "hand") ||
                    MagazineInteractionNameContains(lowerName, "bip01") ||
                    MagazineInteractionNameContains(lowerName, "attach") ||
                    MagazineInteractionNameContains(lowerName, "muzzle") ||
                    MagazineInteractionNameContains(lowerName, "eject") ||
                    MagazineInteractionNameContains(lowerName, "release") ||
                    MagazineInteractionNameContains(lowerName, "realease") ||
                    MagazineInteractionNameContains(lowerName, "magrel") ||
                    MagazineInteractionNameContains(lowerName, "button") ||
                    MagazineInteractionNameContains(lowerName, "trigger") ||
                    MagazineInteractionNameContains(lowerName, "safety") ||
                    MagazineInteractionNameContains(lowerName, "bolt") ||
                    MagazineInteractionNameContains(lowerName, "slide") ||
                    MagazineInteractionNameContains(lowerName, "charger") ||
                    MagazineInteractionNameContains(lowerName, "handle") ||
                    MagazineInteractionNameContains(lowerName, "barrel") ||
                    MagazineInteractionNameContains(lowerName, "stock") ||
                    MagazineInteractionNameContains(lowerName, "hammer") ||
                    MagazineInteractionNameContains(lowerName, "bullet") ||
                    MagazineInteractionNameContains(lowerName, "round") ||
                    MagazineInteractionNameContains(lowerName, "shell"))
                {
                    return false;
                }

                return MagazineInteractionNameContains(lowerName, "clip") ||
                    MagazineInteractionNameContains(lowerName, "magazine") ||
                    MagazineInteractionNameHasLooseMagToken(lowerName);
            };

        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            if (!canUseFallbackName(lowerName))
                continue;

            int score = 250;
            if (lowerName == "valvebiped.weapon_clip" ||
                lowerName == "valvebiped.weapon_magazine" ||
                lowerName == "weapon_clip" ||
                lowerName == "weapon_magazine")
            {
                score += 1800;
            }
            else if (lowerName == "valvebiped.clip" ||
                lowerName == "valvebiped.magazine" ||
                lowerName == "clip" ||
                lowerName == "mag" ||
                lowerName == "magazine")
            {
                score += 1400;
            }

            if (MagazineInteractionNameContains(lowerName, "weapon"))
                score += 500;
            if (MagazineInteractionNameContains(lowerName, "magazine"))
                score += 450;
            if (MagazineInteractionNameHasLooseMagToken(lowerName))
                score += 350;
            if (MagazineInteractionNameContains(lowerName, "clip"))
                score += 300;
            if (MagazineInteractionNameContains(lowerName, "ammo"))
                score -= 100;

            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0 && logDiagnostics)
        {
            static std::mutex s_fallbackLogMutex;
            static std::unordered_map<std::string, int> s_loggedFallbackByModel;
            std::lock_guard<std::mutex> lock(s_fallbackLogMutex);
            auto it = s_loggedFallbackByModel.find(lowerModel);
            if (it == s_loggedFallbackByModel.end() || it->second != bestBone)
            {
                s_loggedFallbackByModel[lowerModel] = bestBone;
                Game::logMsg(
                    "[VR][MagazineBox] official-profile fallback magazine bone model=%s bone=%d name=%s score=%d",
                    lowerModel.c_str(),
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline bool HooksVectorComponentsAreFinite(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    inline void HooksAccumulateBounds(Vector& mins, Vector& maxs, const Vector& value)
    {
        mins.x = std::min(mins.x, value.x);
        mins.y = std::min(mins.y, value.y);
        mins.z = std::min(mins.z, value.z);
        maxs.x = std::max(maxs.x, value.x);
        maxs.y = std::max(maxs.y, value.y);
        maxs.z = std::max(maxs.z, value.z);
    }

    inline bool HooksMagazineBoxCanUseHitboxBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int hitboxBone,
        int magazineBone,
        const std::string& lowerHitboxName)
    {
        const int depth = HooksBoneDescendantDepth(boneParents, hitboxBone, magazineBone);
        if (depth < 0 || depth > 6)
            return false;
        if (depth == 0)
            return true;

        const std::string lowerBoneName =
            hitboxBone < static_cast<int>(boneNames.size())
            ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(hitboxBone)])
            : std::string();
        if (HooksMagazineBoxCanSampleBoneName(lowerBoneName, depth))
            return true;
        if (!lowerHitboxName.empty() && HooksMagazineBoxCanSampleBoneName(lowerHitboxName, depth))
            return true;

        return false;
    }

    inline bool BuildMagazineBoxLocalBoundsFromHitboxes(
        void* drawState,
        int requestedHitboxSet,
        int numBonesOffset,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int magazineBone,
        const vr_vm_stabilize::Mat3x4& magazineWorld,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount,
        int& outLengthAxis)
    {
        outSampleCount = 0;
        outLengthAxis = 0;
        if (!drawState || !sourceBones || numBones <= 0 || magazineBone < 0 || magazineBone >= numBones)
            return false;

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        int numHitboxSets = 0;
        int hitboxSetIndex = 0;
        const int hitboxSetsOffset = (numBonesOffset > 0) ? (numBonesOffset + 16) : 0xAC;
        const int hitboxSetIndexOffset = hitboxSetsOffset + 4;
        if (!vr_vm_stabilize::SafeRead(studioHdr + hitboxSetsOffset, numHitboxSets) ||
            !vr_vm_stabilize::SafeRead(studioHdr + hitboxSetIndexOffset, hitboxSetIndex))
        {
            return false;
        }
        if (numHitboxSets <= 0 || numHitboxSets > 64 || hitboxSetIndex <= 0 || hitboxSetIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (hitboxSetIndex >= studioLength ||
                hitboxSetIndex + numHitboxSets * 12 > studioLength))
        {
            return false;
        }

        const int firstSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? requestedHitboxSet : 0;
        const int endSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? (requestedHitboxSet + 1) : numHitboxSets;

        static constexpr int kHitboxSetStride = 12;
        static constexpr int kHitboxStrideCandidates[] = { 68, 72, 64, 80, 88 };
        int bestCount = 0;
        int bestAxis = 0;
        Vector bestMins(0.0f, 0.0f, 0.0f);
        Vector bestMaxs(0.0f, 0.0f, 0.0f);

        for (int stride : kHitboxStrideCandidates)
        {
            int sampleCount = 0;
            Vector boundsMins(FLT_MAX, FLT_MAX, FLT_MAX);
            Vector boundsMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);

            for (int set = firstSet; set < endSet; ++set)
            {
                const size_t setOffset = static_cast<size_t>(hitboxSetIndex) +
                    static_cast<size_t>(set) * static_cast<size_t>(kHitboxSetStride);
                if (studioLength > 0 && setOffset + kHitboxSetStride > static_cast<size_t>(studioLength))
                    continue;

                const uint8_t* setBase = studioHdr + setOffset;
                int numHitboxes = 0;
                int hitboxIndex = 0;
                if (!vr_vm_stabilize::SafeRead(setBase + 4, numHitboxes) ||
                    !vr_vm_stabilize::SafeRead(setBase + 8, hitboxIndex))
                {
                    continue;
                }
                if (numHitboxes <= 0 || numHitboxes > 512 || hitboxIndex <= 0 || hitboxIndex > 0x200000)
                    continue;

                for (int hitbox = 0; hitbox < numHitboxes; ++hitbox)
                {
                    const size_t hitboxOffset = setOffset + static_cast<size_t>(hitboxIndex) +
                        static_cast<size_t>(hitbox) * static_cast<size_t>(stride);
                    if (studioLength > 0 && hitboxOffset + 32 > static_cast<size_t>(studioLength))
                        continue;

                    const uint8_t* hitboxBase = studioHdr + hitboxOffset;
                    int hitboxBone = -1;
                    Vector bbMin;
                    Vector bbMax;
                    int nameOffset = 0;
                    if (!vr_vm_stabilize::SafeRead(hitboxBase + 0, hitboxBone) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 8, bbMin) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 20, bbMax))
                    {
                        continue;
                    }
                    vr_vm_stabilize::SafeRead(hitboxBase + 32, nameOffset);
                    if (hitboxBone < 0 || hitboxBone >= numBones)
                        continue;
                    if (!HooksVectorComponentsAreFinite(bbMin) || !HooksVectorComponentsAreFinite(bbMax))
                        continue;
                    if (bbMax.x <= bbMin.x || bbMax.y <= bbMin.y || bbMax.z <= bbMin.z)
                        continue;
                    const Vector span = bbMax - bbMin;
                    if (span.x > 128.0f || span.y > 128.0f || span.z > 128.0f)
                        continue;

                    std::string lowerHitboxName;
                    if (nameOffset > 0 && nameOffset < 0x10000)
                    {
                        std::string hitboxName;
                        const size_t nameAddressOffset = hitboxOffset + static_cast<size_t>(nameOffset);
                        if ((studioLength <= 0 || nameAddressOffset < static_cast<size_t>(studioLength)) &&
                            vr_vm_stabilize::TryReadCStringSafe(reinterpret_cast<const char*>(studioHdr + nameAddressOffset), hitboxName))
                        {
                            lowerHitboxName = vr_vm_stabilize::ToLowerAscii(hitboxName);
                        }
                    }

                    if (!HooksMagazineBoxCanUseHitboxBone(boneNames, boneParents, hitboxBone, magazineBone, lowerHitboxName))
                        continue;

                    vr_vm_stabilize::Mat3x4 hitboxBoneWorld{};
                    if (!vr_vm_stabilize::SafeRead(sourceBones + hitboxBone, hitboxBoneWorld))
                        continue;

                    for (int z = 0; z <= 1; ++z)
                    {
                        for (int y = 0; y <= 1; ++y)
                        {
                            for (int x = 0; x <= 1; ++x)
                            {
                                const Vector hitboxLocal(
                                    x ? bbMax.x : bbMin.x,
                                    y ? bbMax.y : bbMin.y,
                                    z ? bbMax.z : bbMin.z);
                                const Vector world = HooksTransformPoint(hitboxBoneWorld, hitboxLocal);
                                const Vector magazineLocal = HooksInverseTransformPoint(magazineWorld, world);
                                HooksAccumulateBounds(boundsMins, boundsMaxs, magazineLocal);
                            }
                        }
                    }
                    ++sampleCount;
                }
            }

            if (sampleCount <= 0 || boundsMins.x == FLT_MAX)
                continue;

            const Vector range = boundsMaxs - boundsMins;
            if (range.x <= 0.001f || range.y <= 0.001f || range.z <= 0.001f)
                continue;

            int axis = 0;
            if (range.y > range[axis])
                axis = 1;
            if (range.z > range[axis])
                axis = 2;

            if (sampleCount > bestCount || (sampleCount == bestCount && range[axis] > (bestMaxs - bestMins)[bestAxis]))
            {
                bestCount = sampleCount;
                bestAxis = axis;
                bestMins = boundsMins;
                bestMaxs = boundsMaxs;
            }
        }

        if (bestCount <= 0)
            return false;

        outMins = bestMins - padding;
        outMaxs = bestMaxs + padding;
        outSampleCount = bestCount;
        outLengthAxis = bestAxis;
        return true;
    }

    inline bool BuildMagazineBoxLocalBoundsFromBoneSamples(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int magazineBone,
        const vr_vm_stabilize::Mat3x4& magazineWorld,
        const Vector& fallbackHalf,
        const Vector& padding,
        const Vector& insertionAxisLocal,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount,
        int& outLengthAxis)
    {
        outSampleCount = 0;
        outLengthAxis = HooksDominantAxis(insertionAxisLocal);
        if (!sourceBones || numBones <= 0 || magazineBone < 0 || magazineBone >= numBones)
            return false;

        Vector sampleMins(FLT_MAX, FLT_MAX, FLT_MAX);
        Vector sampleMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (int bone = 0; bone < numBones; ++bone)
        {
            const int depth = HooksBoneDescendantDepth(boneParents, bone, magazineBone);
            if (depth < 0 || depth > 6)
                continue;

            if (depth > 0)
            {
                const std::string lowerBoneName =
                    bone < static_cast<int>(boneNames.size())
                    ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)])
                    : std::string();
                if (!HooksMagazineBoxCanSampleBoneName(lowerBoneName, depth))
                    continue;
            }

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
                continue;

            const Vector local = HooksInverseTransformPoint(magazineWorld, vr_vm_stabilize::GetOrigin(boneWorld));
            sampleMins.x = std::min(sampleMins.x, local.x);
            sampleMins.y = std::min(sampleMins.y, local.y);
            sampleMins.z = std::min(sampleMins.z, local.z);
            sampleMaxs.x = std::max(sampleMaxs.x, local.x);
            sampleMaxs.y = std::max(sampleMaxs.y, local.y);
            sampleMaxs.z = std::max(sampleMaxs.z, local.z);
            ++outSampleCount;
        }

        if (outSampleCount < 2)
            return false;

        const float ranges[3] = {
            sampleMaxs.x - sampleMins.x,
            sampleMaxs.y - sampleMins.y,
            sampleMaxs.z - sampleMins.z
        };
        if (ranges[0] > 64.0f || ranges[1] > 64.0f || ranges[2] > 64.0f)
            return false;

        int dominantAxis = 0;
        if (ranges[1] > ranges[dominantAxis])
            dominantAxis = 1;
        if (ranges[2] > ranges[dominantAxis])
            dominantAxis = 2;

        const float minUsefulSpan = std::max(0.005f * 43.2f, 0.002f * std::max(1.0f, fallbackHalf.Length()));
        const int preferredAxis = HooksDominantAxis(insertionAxisLocal);
        outLengthAxis = (ranges[preferredAxis] >= minUsefulSpan) ? preferredAxis : dominantAxis;
        if (ranges[outLengthAxis] < minUsefulSpan)
            return false;

        outMins = Vector(0.0f, 0.0f, 0.0f);
        outMaxs = Vector(0.0f, 0.0f, 0.0f);
        for (int axis = 0; axis < 3; ++axis)
        {
            const float center = (sampleMins[axis] + sampleMaxs[axis]) * 0.5f;
            const float sampledHalf = ranges[axis] * 0.5f;
            float half = fallbackHalf[axis] + padding[axis];
            if (axis == outLengthAxis)
                half = std::max(sampledHalf + std::max(padding[axis], fallbackHalf[axis] * 0.5f), fallbackHalf[axis] * 0.65f);
            else if (ranges[axis] >= minUsefulSpan)
                half = std::max(sampledHalf + padding[axis], fallbackHalf[axis] + padding[axis]);

            outMins[axis] = center - half;
            outMaxs[axis] = center + half;
        }

        return true;
    }

    inline void DrawMagazineBoxSolidQuad(
        IVDebugOverlay* overlay,
        const Vector& a,
        const Vector& b,
        const Vector& c,
        const Vector& d,
        int r,
        int g,
        int bColor,
        int alpha,
        bool noDepthTest,
        float duration)
    {
        overlay->AddTriangleOverlay(a, b, c, r, g, bColor, alpha, noDepthTest, duration);
        overlay->AddTriangleOverlay(a, c, d, r, g, bColor, alpha, noDepthTest, duration);
        overlay->AddTriangleOverlay(a, c, b, r, g, bColor, alpha, noDepthTest, duration);
        overlay->AddTriangleOverlay(a, d, c, r, g, bColor, alpha, noDepthTest, duration);
    }

    inline void DrawMagazineBoxSolidObb(
        IVDebugOverlay* overlay,
        const vr_vm_stabilize::Mat3x4& world,
        const Vector& mins,
        const Vector& maxs,
        int r,
        int g,
        int bColor,
        int alpha,
        bool noDepthTest,
        float duration)
    {
        if (!overlay)
            return;

        Vector corners[8];
        for (int z = 0; z <= 1; ++z)
        {
            for (int y = 0; y <= 1; ++y)
            {
                for (int x = 0; x <= 1; ++x)
                {
                    const int index = x | (y << 1) | (z << 2);
                    const Vector local(
                        x ? maxs.x : mins.x,
                        y ? maxs.y : mins.y,
                        z ? maxs.z : mins.z);
                    corners[index] = HooksTransformPoint(world, local);
                }
            }
        }

        DrawMagazineBoxSolidQuad(overlay, corners[0], corners[1], corners[3], corners[2], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[4], corners[6], corners[7], corners[5], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[0], corners[4], corners[5], corners[1], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[2], corners[3], corners[7], corners[6], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[0], corners[2], corners[6], corners[4], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[1], corners[5], corners[7], corners[3], r, g, bColor, alpha, noDepthTest, duration);
    }

    inline void DrawMagazineBoxWireObb(
        IVDebugOverlay* overlay,
        const vr_vm_stabilize::Mat3x4& world,
        const Vector& mins,
        const Vector& maxs,
        int r,
        int g,
        int bColor,
        bool noDepthTest,
        float duration)
    {
        if (!overlay)
            return;

        Vector corners[8];
        for (int z = 0; z <= 1; ++z)
        {
            for (int y = 0; y <= 1; ++y)
            {
                for (int x = 0; x <= 1; ++x)
                {
                    const int index = x | (y << 1) | (z << 2);
                    const Vector local(
                        x ? maxs.x : mins.x,
                        y ? maxs.y : mins.y,
                        z ? maxs.z : mins.z);
                    corners[index] = HooksTransformPoint(world, local);
                }
            }
        }

        static constexpr int kEdges[][2] =
        {
            { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
            { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
        };
        for (const auto& edge : kEdges)
            overlay->AddLineOverlay(corners[edge[0]], corners[edge[1]], r, g, bColor, noDepthTest, duration);
    }

    inline bool CalibrationBoneHighlightBoundsUsable(const Vector& mins, const Vector& maxs)
    {
        if (!HooksVectorComponentsAreFinite(mins) || !HooksVectorComponentsAreFinite(maxs))
            return false;
        const Vector span = maxs - mins;
        if (span.x <= 0.001f || span.y <= 0.001f || span.z <= 0.001f)
            return false;
        return span.x < 180.0f && span.y < 180.0f && span.z < 180.0f;
    }

    inline void CalibrationBoneHighlightEnsureMinimumSpan(Vector& mins, Vector& maxs, const Vector& minHalf)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const float half = std::max(0.05f, HooksVectorComponent(minHalf, axis));
            const float center = (mins[axis] + maxs[axis]) * 0.5f;
            if ((maxs[axis] - mins[axis]) < half * 2.0f)
            {
                mins[axis] = center - half;
                maxs[axis] = center + half;
            }
        }
    }

    inline bool BuildCalibrationBoneHighlightLocalBoundsFromHitboxes(
        void* drawState,
        int requestedHitboxSet,
        int numBonesOffset,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int selectedBone,
        const vr_vm_stabilize::Mat3x4& selectedWorld,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount)
    {
        outSampleCount = 0;
        if (!drawState || !sourceBones || numBones <= 0 || selectedBone < 0 || selectedBone >= numBones)
            return false;

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        int numHitboxSets = 0;
        int hitboxSetIndex = 0;
        const int hitboxSetsOffset = (numBonesOffset > 0) ? (numBonesOffset + 16) : 0xAC;
        const int hitboxSetIndexOffset = hitboxSetsOffset + 4;
        if (!vr_vm_stabilize::SafeRead(studioHdr + hitboxSetsOffset, numHitboxSets) ||
            !vr_vm_stabilize::SafeRead(studioHdr + hitboxSetIndexOffset, hitboxSetIndex))
        {
            return false;
        }
        if (numHitboxSets <= 0 || numHitboxSets > 64 || hitboxSetIndex <= 0 || hitboxSetIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (hitboxSetIndex >= studioLength ||
                hitboxSetIndex + numHitboxSets * 12 > studioLength))
        {
            return false;
        }

        const int firstSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? requestedHitboxSet : 0;
        const int endSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? (requestedHitboxSet + 1) : numHitboxSets;

        static constexpr int kHitboxSetStride = 12;
        static constexpr int kHitboxStrideCandidates[] = { 68, 72, 64, 80, 88 };
        int bestCount = 0;
        bool bestExact = false;
        Vector bestMins(0.0f, 0.0f, 0.0f);
        Vector bestMaxs(0.0f, 0.0f, 0.0f);

        for (int stride : kHitboxStrideCandidates)
        {
            int exactCount = 0;
            int descendantCount = 0;
            Vector exactMins(FLT_MAX, FLT_MAX, FLT_MAX);
            Vector exactMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            Vector descendantMins(FLT_MAX, FLT_MAX, FLT_MAX);
            Vector descendantMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);

            for (int set = firstSet; set < endSet; ++set)
            {
                const size_t setOffset = static_cast<size_t>(hitboxSetIndex) +
                    static_cast<size_t>(set) * static_cast<size_t>(kHitboxSetStride);
                if (studioLength > 0 && setOffset + kHitboxSetStride > static_cast<size_t>(studioLength))
                    continue;

                const uint8_t* setBase = studioHdr + setOffset;
                int numHitboxes = 0;
                int hitboxIndex = 0;
                if (!vr_vm_stabilize::SafeRead(setBase + 4, numHitboxes) ||
                    !vr_vm_stabilize::SafeRead(setBase + 8, hitboxIndex))
                {
                    continue;
                }
                if (numHitboxes <= 0 || numHitboxes > 512 || hitboxIndex <= 0 || hitboxIndex > 0x200000)
                    continue;

                for (int hitbox = 0; hitbox < numHitboxes; ++hitbox)
                {
                    const size_t hitboxOffset = setOffset + static_cast<size_t>(hitboxIndex) +
                        static_cast<size_t>(hitbox) * static_cast<size_t>(stride);
                    if (studioLength > 0 && hitboxOffset + 32 > static_cast<size_t>(studioLength))
                        continue;

                    const uint8_t* hitboxBase = studioHdr + hitboxOffset;
                    int hitboxBone = -1;
                    Vector bbMin;
                    Vector bbMax;
                    if (!vr_vm_stabilize::SafeRead(hitboxBase + 0, hitboxBone) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 8, bbMin) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 20, bbMax))
                    {
                        continue;
                    }
                    if (hitboxBone < 0 || hitboxBone >= numBones)
                        continue;
                    if (!HooksVectorComponentsAreFinite(bbMin) || !HooksVectorComponentsAreFinite(bbMax))
                        continue;
                    if (bbMax.x <= bbMin.x || bbMax.y <= bbMin.y || bbMax.z <= bbMin.z)
                        continue;
                    const Vector span = bbMax - bbMin;
                    if (span.x > 128.0f || span.y > 128.0f || span.z > 128.0f)
                        continue;

                    const int depth = HooksBoneDescendantDepth(boneParents, hitboxBone, selectedBone);
                    if (depth < 0 || depth > 4)
                        continue;

                    Vector& boundsMins = (depth == 0) ? exactMins : descendantMins;
                    Vector& boundsMaxs = (depth == 0) ? exactMaxs : descendantMaxs;
                    int& count = (depth == 0) ? exactCount : descendantCount;
                    for (int z = 0; z <= 1; ++z)
                    {
                        for (int y = 0; y <= 1; ++y)
                        {
                            for (int x = 0; x <= 1; ++x)
                            {
                                const Vector hitboxLocal(
                                    x ? bbMax.x : bbMin.x,
                                    y ? bbMax.y : bbMin.y,
                                    z ? bbMax.z : bbMin.z);

                                vr_vm_stabilize::Mat3x4 hitboxBoneWorld{};
                                if (!vr_vm_stabilize::SafeRead(sourceBones + hitboxBone, hitboxBoneWorld))
                                    continue;

                                const Vector world = HooksTransformPoint(hitboxBoneWorld, hitboxLocal);
                                const Vector selectedLocal = HooksInverseTransformPoint(selectedWorld, world);
                                HooksAccumulateBounds(boundsMins, boundsMaxs, selectedLocal);
                            }
                        }
                    }
                    ++count;
                }
            }

            const bool useExact = exactCount > 0;
            const int sampleCount = useExact ? exactCount : descendantCount;
            if (sampleCount <= 0)
                continue;

            Vector candidateMins = useExact ? exactMins : descendantMins;
            Vector candidateMaxs = useExact ? exactMaxs : descendantMaxs;
            if (!CalibrationBoneHighlightBoundsUsable(candidateMins, candidateMaxs))
                continue;

            if ((useExact && !bestExact) ||
                (useExact == bestExact && sampleCount > bestCount))
            {
                bestExact = useExact;
                bestCount = sampleCount;
                bestMins = candidateMins;
                bestMaxs = candidateMaxs;
            }
        }

        if (bestCount <= 0)
            return false;

        outMins = bestMins - padding;
        outMaxs = bestMaxs + padding;
        outSampleCount = bestCount;
        return CalibrationBoneHighlightBoundsUsable(outMins, outMaxs);
    }

    inline bool BuildCalibrationBoneHighlightLocalBoundsFromBoneLinks(
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int selectedBone,
        const vr_vm_stabilize::Mat3x4& selectedWorld,
        const Vector& fallbackHalf,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount)
    {
        outSampleCount = 1;
        Vector mins(0.0f, 0.0f, 0.0f);
        Vector maxs(0.0f, 0.0f, 0.0f);
        if (!sourceBones || numBones <= 0 || selectedBone < 0 || selectedBone >= numBones)
            return false;

        const int parent =
            selectedBone < static_cast<int>(boneParents.size())
            ? boneParents[static_cast<size_t>(selectedBone)]
            : -1;

        auto addBoneOrigin = [&](int bone) -> void
            {
                if (bone < 0 || bone >= numBones)
                    return;
                vr_vm_stabilize::Mat3x4 boneWorld{};
                if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
                    return;
                const Vector local = HooksInverseTransformPoint(selectedWorld, vr_vm_stabilize::GetOrigin(boneWorld));
                if (!HooksVectorComponentsAreFinite(local) || local.LengthSqr() > (128.0f * 128.0f))
                    return;
                HooksAccumulateBounds(mins, maxs, local);
                ++outSampleCount;
            };

        addBoneOrigin(parent);
        for (int bone = 0; bone < numBones; ++bone)
        {
            const int boneParent =
                bone < static_cast<int>(boneParents.size())
                ? boneParents[static_cast<size_t>(bone)]
                : -1;
            if (boneParent == selectedBone)
                addBoneOrigin(bone);
        }

        outMins = mins - padding;
        outMaxs = maxs + padding;
        CalibrationBoneHighlightEnsureMinimumSpan(outMins, outMaxs, fallbackHalf);
        return CalibrationBoneHighlightBoundsUsable(outMins, outMaxs);
    }

    inline bool CalibrationPreviewModelBoundsUsable(const Vector& mins, const Vector& maxs)
    {
        if (!HooksVectorComponentsAreFinite(mins) || !HooksVectorComponentsAreFinite(maxs))
            return false;
        const Vector span = maxs - mins;
        if (span.x <= 0.001f || span.y <= 0.001f || span.z <= 0.001f)
            return false;
        return span.x < 260.0f && span.y < 260.0f && span.z < 260.0f;
    }

    inline bool BuildCalibrationPreviewModelLocalBoundsFromHitboxes(
        void* drawState,
        int requestedHitboxSet,
        int numBonesOffset,
        const vr_vm_stabilize::Mat3x4* previewBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& previewBasisWorld,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount)
    {
        outSampleCount = 0;
        if (!drawState || !previewBones || numBones <= 0)
            return false;

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        int numHitboxSets = 0;
        int hitboxSetIndex = 0;
        const int hitboxSetsOffset = (numBonesOffset > 0) ? (numBonesOffset + 16) : 0xAC;
        const int hitboxSetIndexOffset = hitboxSetsOffset + 4;
        if (!vr_vm_stabilize::SafeRead(studioHdr + hitboxSetsOffset, numHitboxSets) ||
            !vr_vm_stabilize::SafeRead(studioHdr + hitboxSetIndexOffset, hitboxSetIndex))
        {
            return false;
        }
        if (numHitboxSets <= 0 || numHitboxSets > 64 || hitboxSetIndex <= 0 || hitboxSetIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (hitboxSetIndex >= studioLength ||
                hitboxSetIndex + numHitboxSets * 12 > studioLength))
        {
            return false;
        }

        const int firstSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? requestedHitboxSet : 0;
        const int endSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? (requestedHitboxSet + 1) : numHitboxSets;

        static constexpr int kHitboxSetStride = 12;
        static constexpr int kHitboxStrideCandidates[] = { 68, 72, 64, 80, 88 };
        int bestCount = 0;
        Vector bestMins(0.0f, 0.0f, 0.0f);
        Vector bestMaxs(0.0f, 0.0f, 0.0f);

        for (int stride : kHitboxStrideCandidates)
        {
            int sampleCount = 0;
            Vector candidateMins(FLT_MAX, FLT_MAX, FLT_MAX);
            Vector candidateMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);

            for (int set = firstSet; set < endSet; ++set)
            {
                const size_t setOffset = static_cast<size_t>(hitboxSetIndex) +
                    static_cast<size_t>(set) * static_cast<size_t>(kHitboxSetStride);
                if (studioLength > 0 && setOffset + kHitboxSetStride > static_cast<size_t>(studioLength))
                    continue;

                const uint8_t* setBase = studioHdr + setOffset;
                int numHitboxes = 0;
                int hitboxIndex = 0;
                if (!vr_vm_stabilize::SafeRead(setBase + 4, numHitboxes) ||
                    !vr_vm_stabilize::SafeRead(setBase + 8, hitboxIndex))
                {
                    continue;
                }
                if (numHitboxes <= 0 || numHitboxes > 512 || hitboxIndex <= 0 || hitboxIndex > 0x200000)
                    continue;

                for (int hitbox = 0; hitbox < numHitboxes; ++hitbox)
                {
                    const size_t hitboxOffset = setOffset + static_cast<size_t>(hitboxIndex) +
                        static_cast<size_t>(hitbox) * static_cast<size_t>(stride);
                    if (studioLength > 0 && hitboxOffset + 32 > static_cast<size_t>(studioLength))
                        continue;

                    const uint8_t* hitboxBase = studioHdr + hitboxOffset;
                    int hitboxBone = -1;
                    Vector bbMin;
                    Vector bbMax;
                    if (!vr_vm_stabilize::SafeRead(hitboxBase + 0, hitboxBone) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 8, bbMin) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 20, bbMax))
                    {
                        continue;
                    }
                    if (hitboxBone < 0 || hitboxBone >= numBones)
                        continue;
                    if (!HooksVectorComponentsAreFinite(bbMin) || !HooksVectorComponentsAreFinite(bbMax))
                        continue;
                    if (bbMax.x <= bbMin.x || bbMax.y <= bbMin.y || bbMax.z <= bbMin.z)
                        continue;
                    const Vector span = bbMax - bbMin;
                    if (span.x > 192.0f || span.y > 192.0f || span.z > 192.0f)
                        continue;

                    vr_vm_stabilize::Mat3x4 hitboxBoneWorld{};
                    if (!vr_vm_stabilize::SafeRead(previewBones + hitboxBone, hitboxBoneWorld))
                        continue;

                    for (int z = 0; z <= 1; ++z)
                    {
                        for (int y = 0; y <= 1; ++y)
                        {
                            for (int x = 0; x <= 1; ++x)
                            {
                                const Vector hitboxLocal(
                                    x ? bbMax.x : bbMin.x,
                                    y ? bbMax.y : bbMin.y,
                                    z ? bbMax.z : bbMin.z);
                                const Vector world = HooksTransformPoint(hitboxBoneWorld, hitboxLocal);
                                const Vector previewLocal = HooksInverseTransformPoint(previewBasisWorld, world);
                                if (!HooksVectorComponentsAreFinite(previewLocal) ||
                                    previewLocal.LengthSqr() > (260.0f * 260.0f))
                                {
                                    continue;
                                }
                                HooksAccumulateBounds(candidateMins, candidateMaxs, previewLocal);
                            }
                        }
                    }
                    ++sampleCount;
                }
            }

            if (sampleCount <= 0 || candidateMins.x == FLT_MAX)
                continue;
            if (!CalibrationPreviewModelBoundsUsable(candidateMins, candidateMaxs))
                continue;
            if (sampleCount > bestCount)
            {
                bestCount = sampleCount;
                bestMins = candidateMins;
                bestMaxs = candidateMaxs;
            }
        }

        if (bestCount <= 0)
            return false;

        outMins = bestMins - padding;
        outMaxs = bestMaxs + padding;
        outSampleCount = bestCount;
        return CalibrationPreviewModelBoundsUsable(outMins, outMaxs);
    }

    inline bool BuildCalibrationPreviewModelLocalBoundsFromBones(
        const vr_vm_stabilize::Mat3x4* previewBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& previewBasisWorld,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount)
    {
        outSampleCount = 0;
        if (!previewBones || numBones <= 0)
            return false;

        Vector mins(FLT_MAX, FLT_MAX, FLT_MAX);
        Vector maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(previewBones + bone, boneWorld))
                continue;
            const Vector previewLocal = HooksInverseTransformPoint(previewBasisWorld, vr_vm_stabilize::GetOrigin(boneWorld));
            if (!HooksVectorComponentsAreFinite(previewLocal) ||
                previewLocal.LengthSqr() > (220.0f * 220.0f))
            {
                continue;
            }
            HooksAccumulateBounds(mins, maxs, previewLocal);
            ++outSampleCount;
        }

        if (outSampleCount <= 1 || mins.x == FLT_MAX)
            return false;

        outMins = mins - padding;
        outMaxs = maxs + padding;
        return CalibrationPreviewModelBoundsUsable(outMins, outMaxs);
    }

    inline int HooksVrHandsTwoHandedGripWeaponBoneNameScore(const std::string& lowerName)
    {
        if (lowerName.empty())
            return 0;

        const bool explicitWeaponBone =
            MagazineInteractionNameContains(lowerName, "valvebiped.weapon") ||
            MagazineInteractionNameContains(lowerName, "weapon_bone") ||
            MagazineInteractionNameContains(lowerName, "def_weapon") ||
            MagazineInteractionNameContains(lowerName, "v_weapon");

        static const char* kRejectTokens[] =
        {
            "finger",
            "thumb",
            "hand",
            "upperarm",
            "forearm",
            "wrist",
            "bip01",
            "valvebiped",
            "helper",
            "hlp",
            "ik",
            "camera",
            "attach",
            "attachment",
            "shell",
            "eject",
            "hair",
            "head",
            "face",
            "eye",
            "neck",
            "spine",
            "pelvis",
            "leg",
            "thigh",
            "calf",
            "foot",
            "toe",
            "skirt",
            "cloth",
            "dress",
            "breast",
            "tail",
            "wing",
            "jiggle",
            "physics",
            "phys",
            "pony",
            "character"
        };

        if (!explicitWeaponBone)
        {
            for (const char* token : kRejectTokens)
            {
                if (MagazineInteractionNameContains(lowerName, token))
                    return 0;
            }
        }

        int score = explicitWeaponBone ? 2000 : 0;
        if (HooksViewmodelBoneLabelHasWeaponToken(lowerName))
            score = std::max(score, 900);

        static const char* kWeaponPartTokens[] =
        {
            "barrel",
            "muzzle",
            "stock",
            "receiver",
            "foregrip",
            "grip",
            "handle",
            "rail",
            "scope",
            "sight",
            "trigger",
            "bolt",
            "slide",
            "pump",
            "charging",
            "charger",
            "magazine",
            "clip",
            "rifle",
            "shotgun",
            "pistol",
            "smg",
            "gun"
        };
        for (const char* token : kWeaponPartTokens)
        {
            if (MagazineInteractionNameContains(lowerName, token))
                score = std::max(score, 700);
        }

        return score;
    }

    inline bool HooksVrHandsTwoHandedGripCanSampleWeaponBoneName(const std::string& lowerName)
    {
        return HooksVrHandsTwoHandedGripWeaponBoneNameScore(lowerName) > 0;
    }

    inline bool HooksVrHandsTwoHandedGripFinalizeLocalBounds(
        const std::vector<Vector>& points,
        const Vector& padding,
        float sourceUnitsPerMeter,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount)
    {
        outSampleCount = 0;
        if (points.empty())
            return false;

        const float scale = (std::isfinite(sourceUnitsPerMeter) && sourceUnitsPerMeter > 0.001f)
            ? sourceUnitsPerMeter
            : 43.2f;
        const float maxSpan = std::clamp(2.45f * scale, 84.0f, 126.0f);
        Vector rawMins(FLT_MAX, FLT_MAX, FLT_MAX);
        Vector rawMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Vector& point : points)
            HooksAccumulateBounds(rawMins, rawMaxs, point);

        if (rawMins.x != FLT_MAX)
        {
            const Vector rawPaddedMins = rawMins - padding;
            const Vector rawPaddedMaxs = rawMaxs + padding;
            const Vector rawSpan = rawPaddedMaxs - rawPaddedMins;
            if (rawSpan.x <= maxSpan && rawSpan.y <= maxSpan && rawSpan.z <= maxSpan &&
                CalibrationPreviewModelBoundsUsable(rawPaddedMins, rawPaddedMaxs))
            {
                outMins = rawPaddedMins;
                outMaxs = rawPaddedMaxs;
                outSampleCount = static_cast<int>(points.size());
                return true;
            }
        }

        const float clusterRadius = std::clamp(1.35f * scale, 44.0f, 72.0f);
        const float keepRadius = std::clamp(2.05f * scale, 68.0f, 104.0f);
        const float clusterRadiusSqr = clusterRadius * clusterRadius;
        const float keepRadiusSqr = keepRadius * keepRadius;

        size_t bestIndex = 0u;
        int bestNeighbors = -1;
        float bestDistanceSum = FLT_MAX;
        for (size_t i = 0u; i < points.size(); ++i)
        {
            int neighbors = 0;
            float distanceSum = 0.0f;
            for (size_t j = 0u; j < points.size(); ++j)
            {
                const Vector delta = points[j] - points[i];
                const float distanceSqr = delta.LengthSqr();
                if (distanceSqr <= clusterRadiusSqr)
                {
                    ++neighbors;
                    distanceSum += distanceSqr;
                }
            }
            if (neighbors > bestNeighbors ||
                (neighbors == bestNeighbors && distanceSum < bestDistanceSum))
            {
                bestIndex = i;
                bestNeighbors = neighbors;
                bestDistanceSum = distanceSum;
            }
        }

        const Vector anchor = points[bestIndex];
        Vector mins(FLT_MAX, FLT_MAX, FLT_MAX);
        Vector maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Vector& point : points)
        {
            const Vector delta = point - anchor;
            if (points.size() > 2u && delta.LengthSqr() > keepRadiusSqr)
                continue;

            HooksAccumulateBounds(mins, maxs, point);
            ++outSampleCount;
        }

        if (outSampleCount <= 0 || mins.x == FLT_MAX)
            return false;

        outMins = mins - padding;
        outMaxs = maxs + padding;

        const Vector center = (outMins + outMaxs) * 0.5f;
        Vector half = (outMaxs - outMins) * 0.5f;
        half.x = std::min(half.x, maxSpan * 0.5f);
        half.y = std::min(half.y, maxSpan * 0.5f);
        half.z = std::min(half.z, maxSpan * 0.5f);
        outMins = center - half;
        outMaxs = center + half;

        return CalibrationPreviewModelBoundsUsable(outMins, outMaxs);
    }

    inline bool BuildVrHandsTwoHandedGripWeaponBoxLocalBoundsFromHitboxes(
        void* drawState,
        int requestedHitboxSet,
        int numBonesOffset,
        const std::vector<std::string>& boneNames,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& modelWorld,
        const Vector& padding,
        float sourceUnitsPerMeter,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount)
    {
        outSampleCount = 0;
        if (!drawState || !sourceBones || numBones <= 0)
            return false;

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        int numHitboxSets = 0;
        int hitboxSetIndex = 0;
        const int hitboxSetsOffset = (numBonesOffset > 0) ? (numBonesOffset + 16) : 0xAC;
        const int hitboxSetIndexOffset = hitboxSetsOffset + 4;
        if (!vr_vm_stabilize::SafeRead(studioHdr + hitboxSetsOffset, numHitboxSets) ||
            !vr_vm_stabilize::SafeRead(studioHdr + hitboxSetIndexOffset, hitboxSetIndex))
        {
            return false;
        }
        if (numHitboxSets <= 0 || numHitboxSets > 64 || hitboxSetIndex <= 0 || hitboxSetIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (hitboxSetIndex >= studioLength ||
                hitboxSetIndex + numHitboxSets * 12 > studioLength))
        {
            return false;
        }

        const int firstSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? requestedHitboxSet : 0;
        const int endSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? (requestedHitboxSet + 1) : numHitboxSets;

        static constexpr int kHitboxSetStride = 12;
        static constexpr int kHitboxStrideCandidates[] = { 68, 72, 64, 80, 88 };
        int bestCount = 0;
        Vector bestMins(0.0f, 0.0f, 0.0f);
        Vector bestMaxs(0.0f, 0.0f, 0.0f);

        for (int stride : kHitboxStrideCandidates)
        {
            std::vector<Vector> candidatePoints;
            candidatePoints.reserve(64u);

            for (int set = firstSet; set < endSet; ++set)
            {
                const size_t setOffset = static_cast<size_t>(hitboxSetIndex) +
                    static_cast<size_t>(set) * static_cast<size_t>(kHitboxSetStride);
                if (studioLength > 0 && setOffset + kHitboxSetStride > static_cast<size_t>(studioLength))
                    continue;

                const uint8_t* setBase = studioHdr + setOffset;
                int numHitboxes = 0;
                int hitboxIndex = 0;
                if (!vr_vm_stabilize::SafeRead(setBase + 4, numHitboxes) ||
                    !vr_vm_stabilize::SafeRead(setBase + 8, hitboxIndex))
                {
                    continue;
                }
                if (numHitboxes <= 0 || numHitboxes > 512 || hitboxIndex <= 0 || hitboxIndex > 0x200000)
                    continue;

                for (int hitbox = 0; hitbox < numHitboxes; ++hitbox)
                {
                    const size_t hitboxOffset = setOffset + static_cast<size_t>(hitboxIndex) +
                        static_cast<size_t>(hitbox) * static_cast<size_t>(stride);
                    if (studioLength > 0 && hitboxOffset + 32 > static_cast<size_t>(studioLength))
                        continue;

                    const uint8_t* hitboxBase = studioHdr + hitboxOffset;
                    int hitboxBone = -1;
                    Vector bbMin;
                    Vector bbMax;
                    if (!vr_vm_stabilize::SafeRead(hitboxBase + 0, hitboxBone) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 8, bbMin) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 20, bbMax))
                    {
                        continue;
                    }
                    if (hitboxBone < 0 || hitboxBone >= numBones)
                        continue;
                    const std::string lowerBoneName =
                        hitboxBone < static_cast<int>(boneNames.size())
                        ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(hitboxBone)])
                        : std::string();
                    if (!HooksVrHandsTwoHandedGripCanSampleWeaponBoneName(lowerBoneName))
                        continue;
                    if (!HooksVectorComponentsAreFinite(bbMin) || !HooksVectorComponentsAreFinite(bbMax))
                        continue;
                    if (bbMax.x <= bbMin.x || bbMax.y <= bbMin.y || bbMax.z <= bbMin.z)
                        continue;
                    const Vector span = bbMax - bbMin;
                    if (span.x > 192.0f || span.y > 192.0f || span.z > 192.0f)
                        continue;

                    vr_vm_stabilize::Mat3x4 hitboxBoneWorld{};
                    if (!vr_vm_stabilize::SafeRead(sourceBones + hitboxBone, hitboxBoneWorld))
                        continue;

                    for (int z = 0; z <= 1; ++z)
                    {
                        for (int y = 0; y <= 1; ++y)
                        {
                            for (int x = 0; x <= 1; ++x)
                            {
                                const Vector hitboxLocal(
                                    x ? bbMax.x : bbMin.x,
                                    y ? bbMax.y : bbMin.y,
                                    z ? bbMax.z : bbMin.z);
                                const Vector world = HooksTransformPoint(hitboxBoneWorld, hitboxLocal);
                                const Vector modelLocal = HooksInverseTransformPoint(modelWorld, world);
                                if (!HooksVectorComponentsAreFinite(modelLocal) ||
                                    modelLocal.LengthSqr() > (260.0f * 260.0f))
                                {
                                    continue;
                                }
                                candidatePoints.push_back(modelLocal);
                            }
                        }
                    }
                }
            }

            if (candidatePoints.empty())
                continue;

            Vector candidateMins(0.0f, 0.0f, 0.0f);
            Vector candidateMaxs(0.0f, 0.0f, 0.0f);
            int sampleCount = 0;
            if (!HooksVrHandsTwoHandedGripFinalizeLocalBounds(
                candidatePoints,
                padding,
                sourceUnitsPerMeter,
                candidateMins,
                candidateMaxs,
                sampleCount))
            {
                continue;
            }
            if (sampleCount > bestCount)
            {
                bestCount = sampleCount;
                bestMins = candidateMins;
                bestMaxs = candidateMaxs;
            }
        }

        if (bestCount <= 0)
            return false;

        outMins = bestMins - padding;
        outMaxs = bestMaxs + padding;
        outSampleCount = bestCount;
        return CalibrationPreviewModelBoundsUsable(outMins, outMaxs);
    }

    inline bool BuildVrHandsTwoHandedGripWeaponBoxLocalBoundsFromBones(
        const std::vector<std::string>& boneNames,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& modelWorld,
        const Vector& padding,
        float sourceUnitsPerMeter,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount)
    {
        outSampleCount = 0;
        if (!sourceBones || numBones <= 0)
            return false;

        std::vector<Vector> candidatePoints;
        candidatePoints.reserve(static_cast<size_t>(std::min(numBones, 128)));
        for (int bone = 0; bone < numBones; ++bone)
        {
            const std::string lowerBoneName =
                bone < static_cast<int>(boneNames.size())
                ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)])
                : std::string();
            if (!HooksVrHandsTwoHandedGripCanSampleWeaponBoneName(lowerBoneName))
                continue;

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
                continue;
            const Vector modelLocal = HooksInverseTransformPoint(modelWorld, vr_vm_stabilize::GetOrigin(boneWorld));
            if (!HooksVectorComponentsAreFinite(modelLocal) ||
                modelLocal.LengthSqr() > (220.0f * 220.0f))
            {
                continue;
            }
            candidatePoints.push_back(modelLocal);
        }

        if (candidatePoints.empty())
            return false;

        return HooksVrHandsTwoHandedGripFinalizeLocalBounds(
            candidatePoints,
            padding,
            sourceUnitsPerMeter,
            outMins,
            outMaxs,
            outSampleCount);
    }

    inline bool BuildVrHandsTwoHandedGripWeaponBoxLocalBounds(
        void* drawState,
        int hitboxSet,
        int numBonesOffset,
        const std::vector<std::string>& boneNames,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& modelWorld,
        float scale,
        float touchPaddingScale,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount,
        const char*& outBoundsSource)
    {
        const float clampedTouchPaddingScale = std::clamp(touchPaddingScale, 0.25f, 8.0f);
        const Vector padding(
            std::max(0.12f, 0.006f * scale * clampedTouchPaddingScale),
            std::max(0.12f, 0.006f * scale * clampedTouchPaddingScale),
            std::max(0.12f, 0.006f * scale * clampedTouchPaddingScale));
        const Vector fallbackHalf(
            std::max(1.20f, 0.040f * scale),
            std::max(1.20f, 0.040f * scale),
            std::max(1.20f, 0.040f * scale));

        outBoundsSource = "hitbox";
        bool hasBounds = BuildVrHandsTwoHandedGripWeaponBoxLocalBoundsFromHitboxes(
            drawState,
            hitboxSet,
            numBonesOffset,
            boneNames,
            sourceBones,
            numBones,
            modelWorld,
            padding,
            scale,
            outMins,
            outMaxs,
            outSampleCount);
        if (!hasBounds)
        {
            outBoundsSource = "bones";
            hasBounds = BuildVrHandsTwoHandedGripWeaponBoxLocalBoundsFromBones(
                boneNames,
                sourceBones,
                numBones,
                modelWorld,
                padding,
                scale,
                outMins,
                outMaxs,
                outSampleCount);
        }
        if (!hasBounds)
            return false;

        CalibrationBoneHighlightEnsureMinimumSpan(outMins, outMaxs, fallbackHalf);
        return CalibrationPreviewModelBoundsUsable(outMins, outMaxs);
    }

    inline void DrawCalibrationBoneLinkLines(
        IVDebugOverlay* overlay,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int selectedBone,
        const vr_vm_stabilize::Mat3x4& selectedWorld,
        int r,
        int g,
        int bColor,
        bool noDepthTest,
        float duration)
    {
        if (!overlay || !sourceBones || selectedBone < 0 || selectedBone >= numBones)
            return;

        const Vector selectedOrigin = vr_vm_stabilize::GetOrigin(selectedWorld);
        const int parent =
            selectedBone < static_cast<int>(boneParents.size())
            ? boneParents[static_cast<size_t>(selectedBone)]
            : -1;
        auto addLineToBone = [&](int bone, int lr, int lg, int lb) -> void
            {
                if (bone < 0 || bone >= numBones)
                    return;
                vr_vm_stabilize::Mat3x4 boneWorld{};
                if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
                    return;
                const Vector boneOrigin = vr_vm_stabilize::GetOrigin(boneWorld);
                if (!HooksVectorComponentsAreFinite(boneOrigin) ||
                    (boneOrigin - selectedOrigin).LengthSqr() > (128.0f * 128.0f))
                {
                    return;
                }
                overlay->AddLineOverlay(selectedOrigin, boneOrigin, lr, lg, lb, noDepthTest, duration);
            };

        addLineToBone(parent, r, g, bColor);
        for (int bone = 0; bone < numBones; ++bone)
        {
            const int boneParent =
                bone < static_cast<int>(boneParents.size())
                ? boneParents[static_cast<size_t>(bone)]
                : -1;
            if (boneParent == selectedBone)
                addLineToBone(bone, std::min(255, r + 30), std::min(255, g + 30), std::min(255, bColor + 30));
        }
    }

    inline void DrawMagazineInteractionCalibrationBoneHighlight(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        int entityIndex,
        int hitboxSet,
        const void* pCustomBoneToWorld)
    {
        if (!vr ||
            !vr->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed) ||
            !drawState ||
            !pCustomBoneToWorld ||
            modelName.empty() ||
            !vr->m_Game ||
            !vr->m_Game->m_DebugOverlay)
        {
            return;
        }

        const int selectedBone = vr->m_MagazineInteractionCalibrationSelectedBone.load(std::memory_order_relaxed);
        if (selectedBone < 0)
            return;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsViewmodel(lowerModel) || HooksModelNameIsArmsOrHands(lowerModel))
            return;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return;
        }
        if (numBones <= 0 || numBones > 512 || selectedBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return;
        }

        const uint32_t frameSeq = vr->m_RenderFrameSeq.load(std::memory_order_relaxed);
        const int step = std::clamp(vr->m_MagazineInteractionCalibrationStep.load(std::memory_order_relaxed), 0, 5);
        if (frameSeq != 0)
        {
            static std::mutex s_drawMutex;
            static std::unordered_map<std::string, uint32_t> s_lastDrawSeq;
            const std::string key =
                std::to_string(entityIndex) + "|" +
                lowerModel + "|" +
                std::to_string(selectedBone) + "|" +
                std::to_string(step);
            std::lock_guard<std::mutex> lock(s_drawMutex);
            uint32_t& lastSeq = s_lastDrawSeq[key];
            if (lastSeq == frameSeq)
                return;
            lastSeq = frameSeq;
        }

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        vr_vm_stabilize::Mat3x4 selectedWorld{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + selectedBone, selectedWorld))
            return;

        const float scale = (std::isfinite(vr->m_VRScale) && vr->m_VRScale > 0.001f) ? vr->m_VRScale : 43.2f;
        const Vector fallbackHalf(
            std::max(0.35f, 0.018f * scale),
            std::max(0.35f, 0.018f * scale),
            std::max(0.35f, 0.018f * scale));
        const Vector padding(
            std::max(0.10f, 0.006f * scale),
            std::max(0.10f, 0.006f * scale),
            std::max(0.10f, 0.006f * scale));

        Vector mins(0.0f, 0.0f, 0.0f);
        Vector maxs(0.0f, 0.0f, 0.0f);
        int sampleCount = 0;
        bool hasBounds = BuildCalibrationBoneHighlightLocalBoundsFromHitboxes(
            drawState,
            hitboxSet,
            numBonesOffset,
            boneParents,
            sourceBones,
            numBones,
            selectedBone,
            selectedWorld,
            padding,
            mins,
            maxs,
            sampleCount);
        if (!hasBounds)
        {
            hasBounds = BuildCalibrationBoneHighlightLocalBoundsFromBoneLinks(
                boneParents,
                sourceBones,
                numBones,
                selectedBone,
                selectedWorld,
                fallbackHalf,
                padding,
                mins,
                maxs,
                sampleCount);
        }
        if (!hasBounds)
            return;

        CalibrationBoneHighlightEnsureMinimumSpan(mins, maxs, fallbackHalf);
        if (step == 1 || step == 2)
            ApplyMagazineInteractionMagazineBoxCalibrationAdjustments(vr, selectedWorld, mins, maxs);
        if (step == 2)
            ApplyMagazineInteractionSocketCaptureCalibrationAdjustments(vr, selectedWorld, mins, maxs);
        if (step == 4 || step == 5)
            ApplyMagazineInteractionBoltBoxCalibrationAdjustments(vr, selectedWorld, mins, maxs);

        constexpr bool kUseSourceDebugOverlayForCalibration = false;
        if (!kUseSourceDebugOverlayForCalibration)
            return;

        const bool boltStep = (step >= 3);
        const bool socketStep = (step == 2);
        const int r = boltStep ? 74 : (socketStep ? 40 : 40);
        const int g = boltStep ? 142 : (socketStep ? 190 : 224);
        const int b = boltStep ? 255 : (socketStep ? 255 : 174);
        const float duration = std::max(0.02f, vr->m_LastFrameDuration * 2.5f);
        constexpr bool kNoDepthTest = true;
        IVDebugOverlay* overlay = vr->m_Game->m_DebugOverlay;
        DrawMagazineBoxSolidObb(overlay, selectedWorld, mins, maxs, r, g, b, 74, kNoDepthTest, duration);
        DrawMagazineBoxWireObb(overlay, selectedWorld, mins, maxs, std::min(255, r + 40), std::min(255, g + 30), 255, kNoDepthTest, duration);
        DrawCalibrationBoneLinkLines(overlay, boneParents, sourceBones, numBones, selectedBone, selectedWorld, r, g, b, kNoDepthTest, duration);

        const Vector origin = vr_vm_stabilize::GetOrigin(selectedWorld);
        const float tick = std::max(0.25f, 0.007f * scale);
        overlay->AddLineOverlay(origin - HooksMatrixAxis(selectedWorld, 0) * tick, origin + HooksMatrixAxis(selectedWorld, 0) * tick, 255, 255, 255, kNoDepthTest, duration);
        overlay->AddLineOverlay(origin - HooksMatrixAxis(selectedWorld, 1) * tick, origin + HooksMatrixAxis(selectedWorld, 1) * tick, 255, 255, 255, kNoDepthTest, duration);
        overlay->AddLineOverlay(origin - HooksMatrixAxis(selectedWorld, 2) * tick, origin + HooksMatrixAxis(selectedWorld, 2) * tick, 255, 255, 255, kNoDepthTest, duration);
        if (step == 5)
        {
            Vector pullAxis = BuildMagazineInteractionBoltPullAxisWorld(
                vr,
                modelName,
                sourceBones,
                numBones,
                boneParents,
                ResolveMagazineInteractionWeaponIdForConfig(vr),
                selectedBone,
                selectedWorld,
                ResolveMagazineInteractionProfileKeyForConfig(vr),
                nullptr);
            pullAxis = HooksNormalizeVector(pullAxis, Vector(0.0f, 0.0f, 0.0f));
            if (pullAxis.Length() > 0.0001f)
            {
                const float pullLen = std::max(0.75f, ResolveMagazineInteractionBoltPullDistanceMeters(vr) * scale);
                overlay->AddLineOverlay(origin, origin + pullAxis * pullLen, 255, 230, 90, kNoDepthTest, duration);
            }
        }
    }

    inline bool BuildMagazineInteractionCalibrationPreviewBones(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& modelInfo,
        bool allowViewmodelClass,
        int entityIndex,
        int hitboxSet,
        const void* pCustomBoneToWorld,
        vr_vm_stabilize::Mat3x4*& outBones,
        vr_vm_stabilize::Mat3x4*& outModelToWorld,
        Vector& outPreviewOrigin,
        QAngle& outPreviewAngles)
    {
        outBones = nullptr;
        outModelToWorld = nullptr;
        outPreviewOrigin = Vector(0.0f, 0.0f, 0.0f);
        outPreviewAngles = QAngle(0.0f, 0.0f, 0.0f);
        if (!vr ||
            !vr->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed) ||
            !drawState ||
            !pCustomBoneToWorld ||
            modelName.empty())
        {
            return false;
        }

        const int selectedBone = vr->m_MagazineInteractionCalibrationSelectedBone.load(std::memory_order_relaxed);
        if (selectedBone < 0)
            return false;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (HooksModelNameIsArmsOrHands(lowerModel))
        {
            return false;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return false;
        }
        if (numBones <= 0 || numBones > 512 || selectedBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        const bool sourceIsViewmodelPath = HooksModelNameIsViewmodel(lowerModel);
        const bool sourceHasLooseViewmodelMarker = HooksModelNameHasLooseViewmodelMarker(lowerModel);
        const bool sourceLooksLikeWeaponBones = HooksCalibrationBoneNamesLookLikeWeapon(boneNames);
        if (!allowViewmodelClass &&
            !sourceIsViewmodelPath &&
            !sourceHasLooseViewmodelMarker &&
            !sourceLooksLikeWeaponBones)
        {
            return false;
        }

        const uint32_t sourceBoneSignature = HooksBuildViewmodelBoneSignature(
            modelName,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);
        const uint32_t sourceModelFingerprint =
            HooksBuildStudioHdrFingerprint(drawState, sourceBoneSignature);
        MagazineInteractionCalibrationSnapshot snapshot{};
        if (!vr->GetMagazineInteractionCalibrationSnapshot(snapshot) ||
            std::chrono::duration<float>(std::chrono::steady_clock::now() - snapshot.publishedAt).count() > 1.5f ||
            snapshot.numBones != numBones ||
            snapshot.boneSignature != sourceBoneSignature ||
            snapshot.modelFingerprint != sourceModelFingerprint ||
            (snapshot.entityIndex > 0 && entityIndex > 0 && snapshot.entityIndex != entityIndex) ||
            (snapshot.sourceIsViewmodelClass && !allowViewmodelClass))
        {
            return false;
        }

        Vector viewOrigin{};
        Vector viewForward{};
        Vector viewRight{};
        Vector viewUp{};
        const Vector viewAngles = vr->GetViewAngle();
        QAngle eyeAngles(viewAngles.x, viewAngles.y, viewAngles.z);
        QAngle::AngleVectors(eyeAngles, &viewForward, &viewRight, &viewUp);
        viewForward = HooksNormalizeVector(viewForward, Vector(0.0f, 0.0f, 0.0f));
        viewRight = HooksNormalizeVector(viewRight, Vector(0.0f, 0.0f, 0.0f));
        viewUp = HooksNormalizeVector(viewUp, Vector(0.0f, 0.0f, 0.0f));
        viewOrigin = (vr->GetViewOriginLeft() + vr->GetViewOriginRight()) * 0.5f;
        if (viewForward.Length() <= 0.0001f ||
            viewRight.Length() <= 0.0001f ||
            viewUp.Length() <= 0.0001f ||
            !HooksVectorComponentsAreFinite(viewOrigin))
        {
            return false;
        }

        Vector previewAnchorOrigin = viewOrigin;
        Vector previewAnchorAngles = viewAngles;
        if (vr->m_MagazineInteractionCalibrationPreviewAnchorValid.load(std::memory_order_relaxed))
        {
            const Vector candidateOrigin(
                vr->m_MagazineInteractionCalibrationPreviewAnchorOriginX.load(std::memory_order_relaxed),
                vr->m_MagazineInteractionCalibrationPreviewAnchorOriginY.load(std::memory_order_relaxed),
                vr->m_MagazineInteractionCalibrationPreviewAnchorOriginZ.load(std::memory_order_relaxed));
            const Vector candidateAngles(
                vr->m_MagazineInteractionCalibrationPreviewAnchorPitchDeg.load(std::memory_order_relaxed),
                vr->m_MagazineInteractionCalibrationPreviewAnchorYawDeg.load(std::memory_order_relaxed),
                vr->m_MagazineInteractionCalibrationPreviewAnchorRollDeg.load(std::memory_order_relaxed));
            if (HooksVectorComponentsAreFinite(candidateOrigin) &&
                HooksVectorComponentsAreFinite(candidateAngles))
            {
                previewAnchorOrigin = candidateOrigin;
                previewAnchorAngles = candidateAngles;
                QAngle anchorAngles(previewAnchorAngles.x, previewAnchorAngles.y, previewAnchorAngles.z);
                QAngle::AngleVectors(anchorAngles, &viewForward, &viewRight, &viewUp);
                viewForward = HooksNormalizeVector(viewForward, Vector(0.0f, 0.0f, 0.0f));
                viewRight = HooksNormalizeVector(viewRight, Vector(0.0f, 0.0f, 0.0f));
                viewUp = HooksNormalizeVector(viewUp, Vector(0.0f, 0.0f, 0.0f));
            }
        }

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        vr_vm_stabilize::Mat3x4 selectedWorld{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + selectedBone, selectedWorld))
            return false;

        const float scale = (std::isfinite(vr->m_VRScale) && vr->m_VRScale > 0.001f) ? vr->m_VRScale : 43.2f;
        auto matrixFinite = [](const vr_vm_stabilize::Mat3x4& matrix) -> bool
            {
                for (int row = 0; row < 3; ++row)
                {
                    for (int col = 0; col < 4; ++col)
                    {
                        if (!std::isfinite(matrix.m[row][col]))
                            return false;
                    }
                }
                return HooksVectorComponentsAreFinite(vr_vm_stabilize::GetOrigin(matrix)) &&
                    HooksMatrixAxis(matrix, 0).LengthSqr() > 0.000001f &&
                    HooksMatrixAxis(matrix, 1).LengthSqr() > 0.000001f &&
                    HooksMatrixAxis(matrix, 2).LengthSqr() > 0.000001f;
            };

        vr_vm_stabilize::Mat3x4 sourceModelWorld{};
        bool hasSourceModelWorld = false;
        if (modelInfo.pModelToWorld)
        {
            vr_vm_stabilize::Mat3x4 candidate{};
            if (vr_vm_stabilize::SafeRead(
                reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(modelInfo.pModelToWorld),
                candidate) &&
                matrixFinite(candidate))
            {
                sourceModelWorld = candidate;
                hasSourceModelWorld = true;
            }
        }
        if (!hasSourceModelWorld)
        {
            vr_vm_stabilize::BuildFromOrgAngles(modelInfo.origin, modelInfo.angles, sourceModelWorld);
            hasSourceModelWorld = matrixFinite(sourceModelWorld);
        }
        if (!hasSourceModelWorld)
            sourceModelWorld = selectedWorld;

        const Vector modelPadding(
            std::max(0.18f, 0.010f * scale),
            std::max(0.18f, 0.010f * scale),
            std::max(0.18f, 0.010f * scale));
        const Vector modelFallbackHalf(
            std::max(1.20f, 0.045f * scale),
            std::max(1.20f, 0.045f * scale),
            std::max(1.20f, 0.045f * scale));
        Vector sourceModelMins(0.0f, 0.0f, 0.0f);
        Vector sourceModelMaxs(0.0f, 0.0f, 0.0f);
        int sourceModelSampleCount = 0;
        bool hasSourceModelBounds = BuildCalibrationPreviewModelLocalBoundsFromHitboxes(
            drawState,
            hitboxSet,
            numBonesOffset,
            sourceBones,
            numBones,
            sourceModelWorld,
            modelPadding,
            sourceModelMins,
            sourceModelMaxs,
            sourceModelSampleCount);
        if (!hasSourceModelBounds)
        {
            hasSourceModelBounds = BuildCalibrationPreviewModelLocalBoundsFromBones(
                sourceBones,
                numBones,
                sourceModelWorld,
                modelPadding,
                sourceModelMins,
                sourceModelMaxs,
                sourceModelSampleCount);
        }
        Vector sourceModelCenterLocal(0.0f, 0.0f, 0.0f);
        if (hasSourceModelBounds)
        {
            CalibrationBoneHighlightEnsureMinimumSpan(sourceModelMins, sourceModelMaxs, modelFallbackHalf);
            sourceModelCenterLocal = (sourceModelMins + sourceModelMaxs) * 0.5f;
            if (!HooksVectorComponentsAreFinite(sourceModelCenterLocal) ||
                sourceModelCenterLocal.LengthSqr() > (260.0f * 260.0f))
            {
                sourceModelCenterLocal = Vector(0.0f, 0.0f, 0.0f);
                hasSourceModelBounds = false;
            }
        }
        const float previewForwardMeters = std::clamp(
            vr->m_MagazineInteractionCalibrationPreviewForwardMeters.load(std::memory_order_relaxed),
            0.15f,
            5.00f);
        const float previewRightMeters = std::clamp(
            vr->m_MagazineInteractionCalibrationPreviewRightMeters.load(std::memory_order_relaxed),
            -3.00f,
            3.00f);
        const float previewUpMeters = std::clamp(
            vr->m_MagazineInteractionCalibrationPreviewUpMeters.load(std::memory_order_relaxed),
            -2.00f,
            2.00f);
        const float previewPitchDeg = std::clamp(
            vr->m_MagazineInteractionCalibrationPreviewPitchDeg.load(std::memory_order_relaxed),
            -180.0f,
            180.0f);
        const float previewYawDeg = std::clamp(
            vr->m_MagazineInteractionCalibrationPreviewYawDeg.load(std::memory_order_relaxed),
            -180.0f,
            180.0f);
        const float previewRollDeg = std::clamp(
            vr->m_MagazineInteractionCalibrationPreviewRollDeg.load(std::memory_order_relaxed),
            -180.0f,
            180.0f);
        const Vector previewOrigin =
            previewAnchorOrigin +
            viewForward * (previewForwardMeters * scale) +
            viewRight * (previewRightMeters * scale) +
            viewUp * (previewUpMeters * scale);

        vr_vm_stabilize::Mat3x4 targetModelWorld{};
        QAngle previewAngles(
            previewAnchorAngles.x + previewPitchDeg,
            previewAnchorAngles.y + previewYawDeg,
            previewAnchorAngles.z + previewRollDeg);
        vr_vm_stabilize::BuildFromOrgAngles(previewOrigin, previewAngles, targetModelWorld);
        if (hasSourceModelBounds)
        {
            const Vector centerOffsetWorld =
                HooksMatrixAxis(targetModelWorld, 0) * sourceModelCenterLocal.x +
                HooksMatrixAxis(targetModelWorld, 1) * sourceModelCenterLocal.y +
                HooksMatrixAxis(targetModelWorld, 2) * sourceModelCenterLocal.z;
            targetModelWorld.m[0][3] = previewOrigin.x - centerOffsetWorld.x;
            targetModelWorld.m[1][3] = previewOrigin.y - centerOffsetWorld.y;
            targetModelWorld.m[2][3] = previewOrigin.z - centerOffsetWorld.z;
        }
        vr_vm_stabilize::Mat3x4 inverseSourceModelWorld{};
        vr_vm_stabilize::InvertTR(sourceModelWorld, inverseSourceModelWorld);
        vr_vm_stabilize::Mat3x4 previewDelta{};
        vr_vm_stabilize::Mul(targetModelWorld, inverseSourceModelWorld, previewDelta);

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_relaxed) & ~1u;
        if (seqEven == 0)
            seqEven = (static_cast<uint32_t>(GetTickCount()) << 1u) | 2u;
        vr_vm_stabilize::Mat3x4* previewBones = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!previewBones)
            return false;

        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return false;
            vr_vm_stabilize::Mul(previewDelta, source, previewBones[bone]);
        }

        vr_vm_stabilize::Mat3x4* previewModelToWorld = vr_vm_stabilize::AllocStableBones(1, seqEven);
        if (previewModelToWorld)
        {
            previewModelToWorld[0] = targetModelWorld;
            if (!matrixFinite(previewModelToWorld[0]))
                previewModelToWorld[0] = previewBones[0];
        }

        vr_vm_stabilize::Mat3x4 previewSelectedWorld{};
        vr_vm_stabilize::Mul(previewDelta, selectedWorld, previewSelectedWorld);

        Vector mins(0.0f, 0.0f, 0.0f);
        Vector maxs(0.0f, 0.0f, 0.0f);
        int sampleCount = 0;
        const Vector fallbackHalf(
            std::max(0.35f, 0.018f * scale),
            std::max(0.35f, 0.018f * scale),
            std::max(0.35f, 0.018f * scale));
        const Vector padding(
            std::max(0.10f, 0.006f * scale),
            std::max(0.10f, 0.006f * scale),
            std::max(0.10f, 0.006f * scale));

        constexpr bool kUseSourceDebugOverlayForCalibration = false;
        if (kUseSourceDebugOverlayForCalibration && vr->m_Game && vr->m_Game->m_DebugOverlay)
        {
            if (hasSourceModelBounds)
            {
                constexpr bool kNoDepthTest = true;
                const float duration = std::max(0.02f, vr->m_LastFrameDuration * 2.5f);
                DrawMagazineBoxWireObb(
                    vr->m_Game->m_DebugOverlay,
                    targetModelWorld,
                    sourceModelMins,
                    sourceModelMaxs,
                    82,
                    205,
                    255,
                    kNoDepthTest,
                    duration);
            }
        }

        bool hasBounds = BuildCalibrationBoneHighlightLocalBoundsFromHitboxes(
            drawState,
            hitboxSet,
            numBonesOffset,
            boneParents,
            sourceBones,
            numBones,
            selectedBone,
            selectedWorld,
            padding,
            mins,
            maxs,
            sampleCount);
        if (!hasBounds)
        {
            hasBounds = BuildCalibrationBoneHighlightLocalBoundsFromBoneLinks(
                boneParents,
                sourceBones,
                numBones,
                selectedBone,
                selectedWorld,
                fallbackHalf,
                padding,
                mins,
                maxs,
                sampleCount);
        }
        if (hasBounds)
        {
            CalibrationBoneHighlightEnsureMinimumSpan(mins, maxs, fallbackHalf);
            const int step = std::clamp(vr->m_MagazineInteractionCalibrationStep.load(std::memory_order_relaxed), 0, 5);
            if (step == 1 || step == 2)
                ApplyMagazineInteractionMagazineBoxCalibrationAdjustments(vr, previewSelectedWorld, mins, maxs);
            if (step == 2)
                ApplyMagazineInteractionSocketCaptureCalibrationAdjustments(vr, previewSelectedWorld, mins, maxs);
            if (step == 4 || step == 5)
                ApplyMagazineInteractionBoltBoxCalibrationAdjustments(vr, previewSelectedWorld, mins, maxs);

            const Vector boxOrigin(
                previewSelectedWorld.m[0][3],
                previewSelectedWorld.m[1][3],
                previewSelectedWorld.m[2][3]);
            const Vector boxAxisX(
                previewSelectedWorld.m[0][0],
                previewSelectedWorld.m[1][0],
                previewSelectedWorld.m[2][0]);
            const Vector boxAxisY(
                previewSelectedWorld.m[0][1],
                previewSelectedWorld.m[1][1],
                previewSelectedWorld.m[2][1]);
            const Vector boxAxisZ(
                previewSelectedWorld.m[0][2],
                previewSelectedWorld.m[1][2],
                previewSelectedWorld.m[2][2]);
            const Vector modelOrigin(
                targetModelWorld.m[0][3],
                targetModelWorld.m[1][3],
                targetModelWorld.m[2][3]);
            const Vector modelAxisX(
                targetModelWorld.m[0][0],
                targetModelWorld.m[1][0],
                targetModelWorld.m[2][0]);
            const Vector modelAxisY(
                targetModelWorld.m[0][1],
                targetModelWorld.m[1][1],
                targetModelWorld.m[2][1]);
            const Vector modelAxisZ(
                targetModelWorld.m[0][2],
                targetModelWorld.m[1][2],
                targetModelWorld.m[2][2]);
            const uint32_t publishSeq = (seqEven != 0) ? seqEven : vr->m_RenderFrameSeq.load(std::memory_order_relaxed);
            if (step <= 2)
            {
                vr->PublishMagazineInteractionBox(
                    boxOrigin,
                    boxAxisX,
                    boxAxisY,
                    boxAxisZ,
                    mins,
                    maxs,
                    publishSeq,
                    entityIndex,
                    selectedBone,
                    modelName.c_str(),
                    true,
                    modelOrigin,
                    modelAxisX,
                    modelAxisY,
                    modelAxisZ);
            }
            else
            {
                Vector pullAxis = BuildMagazineInteractionBoltPullAxisWorld(
                    vr,
                    modelName,
                    previewBones,
                    numBones,
                    boneParents,
                    ResolveMagazineInteractionWeaponIdForConfig(vr),
                    selectedBone,
                    previewSelectedWorld,
                    ResolveMagazineInteractionProfileKeyForConfig(vr),
                    &targetModelWorld);
                pullAxis = HooksNormalizeVector(pullAxis, Vector(0.0f, 0.0f, 0.0f));
                vr->PublishMagazineInteractionBoltBox(
                    boxOrigin,
                    boxAxisX,
                    boxAxisY,
                    boxAxisZ,
                    mins,
                    maxs,
                    pullAxis,
                    publishSeq,
                    entityIndex,
                    selectedBone,
                    modelName.c_str());
            }

            if (kUseSourceDebugOverlayForCalibration && vr->m_Game && vr->m_Game->m_DebugOverlay)
            {
                const bool boltStep = (step >= 3);
                const bool socketStep = (step == 2);
                const int r = boltStep ? 74 : (socketStep ? 40 : 40);
                const int g = boltStep ? 142 : (socketStep ? 190 : 224);
                const int b = boltStep ? 255 : (socketStep ? 255 : 174);
                const float duration = std::max(0.02f, vr->m_LastFrameDuration * 2.5f);
                constexpr bool kNoDepthTest = true;
                IVDebugOverlay* overlay = vr->m_Game->m_DebugOverlay;
                DrawMagazineBoxSolidObb(overlay, previewSelectedWorld, mins, maxs, r, g, b, 92, kNoDepthTest, duration);
                DrawMagazineBoxWireObb(overlay, previewSelectedWorld, mins, maxs, std::min(255, r + 50), std::min(255, g + 35), 255, kNoDepthTest, duration);
                DrawCalibrationBoneLinkLines(overlay, boneParents, previewBones, numBones, selectedBone, previewSelectedWorld, r, g, b, kNoDepthTest, duration);

                const Vector tickOrigin = vr_vm_stabilize::GetOrigin(previewSelectedWorld);
                const float tick = std::max(0.25f, 0.007f * scale);
                overlay->AddLineOverlay(tickOrigin - HooksMatrixAxis(previewSelectedWorld, 0) * tick, tickOrigin + HooksMatrixAxis(previewSelectedWorld, 0) * tick, 255, 255, 255, kNoDepthTest, duration);
                overlay->AddLineOverlay(tickOrigin - HooksMatrixAxis(previewSelectedWorld, 1) * tick, tickOrigin + HooksMatrixAxis(previewSelectedWorld, 1) * tick, 255, 255, 255, kNoDepthTest, duration);
                overlay->AddLineOverlay(tickOrigin - HooksMatrixAxis(previewSelectedWorld, 2) * tick, tickOrigin + HooksMatrixAxis(previewSelectedWorld, 2) * tick, 255, 255, 255, kNoDepthTest, duration);
                if (step == 5)
                {
                    Vector pullAxis = BuildMagazineInteractionBoltPullAxisWorld(
                        vr,
                        modelName,
                        previewBones,
                        numBones,
                        boneParents,
                        ResolveMagazineInteractionWeaponIdForConfig(vr),
                        selectedBone,
                        previewSelectedWorld,
                        ResolveMagazineInteractionProfileKeyForConfig(vr),
                        nullptr);
                    pullAxis = HooksNormalizeVector(pullAxis, Vector(0.0f, 0.0f, 0.0f));
                    if (pullAxis.Length() > 0.0001f)
                    {
                        const float pullLen = std::max(0.75f, ResolveMagazineInteractionBoltPullDistanceMeters(vr) * scale);
                        overlay->AddLineOverlay(tickOrigin, tickOrigin + pullAxis * pullLen, 255, 230, 90, kNoDepthTest, duration);
                    }
                }
            }
        }

        (void)entityIndex;
        outBones = previewBones;
        outModelToWorld = previewModelToWorld;
        outPreviewOrigin = previewOrigin;
        outPreviewAngles = previewAngles;
        return true;
    }

    inline vr_vm_stabilize::Mat3x4* BuildHiddenMagazineInteractionCalibrationViewmodelBones(
        VR* vr,
        void* drawState,
        const void* sourceBoneToWorld)
    {
        if (!vr || !drawState || !sourceBoneToWorld)
            return nullptr;

        int numBones = 0;
        if (!vr_vm_stabilize::TryGetNumBonesFromDrawState(drawState, numBones) ||
            numBones <= 0 ||
            numBones > 512)
        {
            return nullptr;
        }

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_relaxed) & ~1u;
        if (seqEven == 0)
            seqEven = (static_cast<uint32_t>(GetTickCount()) << 1u) | 2u;

        vr_vm_stabilize::Mat3x4* hiddenBones = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!hiddenBones)
            return nullptr;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(sourceBoneToWorld);
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return nullptr;
            hiddenBones[bone] = source;
            hiddenBones[bone].m[0][3] += 100000.0f;
            hiddenBones[bone].m[1][3] += 100000.0f;
            hiddenBones[bone].m[2][3] += 100000.0f;
        }
        return hiddenBones;
    }

    inline int FindConfiguredViewmodelBoneOverride(
        const char* logTag,
        const char* role,
        const char* configName,
        int weaponId,
        const std::unordered_map<int, std::vector<std::string>>& overrides,
        const std::string& profileKey,
        const std::unordered_map<std::string, std::vector<std::string>>& profileOverrides,
        const std::string& overrideSpec,
        const std::string& modelName,
        const std::vector<std::string>& boneNames,
        std::string& outConfiguredName,
        bool logDiagnostics)
    {
        outConfiguredName.clear();
        const std::vector<std::string>* requestedNames = nullptr;
        std::string matchedOverrideKey;
        int matchedWeaponId = 0;
        if (!profileKey.empty())
        {
            const std::string normalizedProfileKey = vr_vm_stabilize::ToLowerAscii(profileKey);
            const auto profileIt = profileOverrides.find(normalizedProfileKey);
            if (profileIt != profileOverrides.end() && !profileIt->second.empty())
            {
                requestedNames = &profileIt->second;
                matchedOverrideKey = std::string("profile:") + normalizedProfileKey;
            }
        }
        if (!requestedNames && weaponId > 0)
        {
            const auto overrideIt = overrides.find(weaponId);
            if (overrideIt != overrides.end() && !overrideIt->second.empty())
            {
                requestedNames = &overrideIt->second;
                matchedWeaponId = weaponId;
                matchedOverrideKey = std::string("weapon:") + std::to_string(weaponId);
            }
        }
        if (!requestedNames)
            return -1;

        for (const std::string& requestedName : *requestedNames)
        {
            const std::string requestedLower = vr_vm_stabilize::ToLowerAscii(requestedName);
            if (requestedLower.empty())
                continue;

            const char* indexText = requestedLower.c_str();
            if (*indexText == '#')
                ++indexText;
            if (*indexText)
            {
                char* end = nullptr;
                const long requestedIndex = std::strtol(indexText, &end, 10);
                if (end && *end == '\0' && requestedIndex >= 0 &&
                    requestedIndex < static_cast<long>(boneNames.size()))
                {
                    const int bone = static_cast<int>(requestedIndex);
                    outConfiguredName = requestedName;

                    static std::mutex s_overrideIndexLogMutex;
                    static std::unordered_set<std::string> s_loggedIndexMatches;
                    const std::string logKey =
                        std::string(logTag ? logTag : "VR") + "|index|" +
                        (role ? role : "bone") + "|" + matchedOverrideKey + "|" + modelName + "|" + requestedLower;
                    bool shouldLog = false;
                    {
                        std::lock_guard<std::mutex> lock(s_overrideIndexLogMutex);
                        shouldLog = s_loggedIndexMatches.insert(logKey).second;
                    }
                    if (logDiagnostics && shouldLog)
                    {
                        const char* boneName =
                            (bone < static_cast<int>(boneNames.size()) && !boneNames[static_cast<size_t>(bone)].empty())
                            ? boneNames[static_cast<size_t>(bone)].c_str()
                            : "<unnamed>";
                        Game::logMsg(
                            "[VR][%s] configured %s bone index override matched source=%s weaponId=%d model=%s bone=%d name=%s",
                            logTag ? logTag : "Unknown",
                            role ? role : "viewmodel",
                            configName ? configName : "unknown",
                            matchedWeaponId > 0 ? matchedWeaponId : weaponId,
                            modelName.c_str(),
                            bone,
                            boneName);
                    }
                    return bone;
                }
            }

            for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
            {
                if (vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]) != requestedLower)
                    continue;

                outConfiguredName = requestedName;

                static std::mutex s_overrideLogMutex;
                static std::unordered_set<std::string> s_loggedMatches;
                const std::string logKey =
                    std::string(logTag ? logTag : "VR") + "|match|" +
                    (role ? role : "bone") + "|" + matchedOverrideKey + "|" + modelName + "|" + requestedLower;
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(s_overrideLogMutex);
                    shouldLog = s_loggedMatches.insert(logKey).second;
                }
                if (logDiagnostics && shouldLog)
                {
                    Game::logMsg(
                        "[VR][%s] configured %s bone override matched source=%s weaponId=%d model=%s bone=%d name=%s",
                        logTag ? logTag : "Unknown",
                        role ? role : "viewmodel",
                        configName ? configName : "unknown",
                        matchedWeaponId > 0 ? matchedWeaponId : weaponId,
                        modelName.c_str(),
                        bone,
                        boneNames[static_cast<size_t>(bone)].c_str());
                }
                return bone;
            }
        }

        static std::mutex s_overrideMissLogMutex;
        static std::unordered_set<std::string> s_loggedMisses;
        const std::string missKey =
            std::string(logTag ? logTag : "VR") + "|miss|" +
            (role ? role : "bone") + "|" + matchedOverrideKey + "|" + modelName + "|" + overrideSpec;
        bool shouldLogMiss = false;
        {
            std::lock_guard<std::mutex> lock(s_overrideMissLogMutex);
            shouldLogMiss = s_loggedMisses.insert(missKey).second;
        }
        if (logDiagnostics && shouldLogMiss)
        {
            Game::logMsg(
                "[VR][%s] configured %s bone override not found; falling back to automatic detection source=%s weaponId=%d model=%s spec=%s",
                logTag ? logTag : "Unknown",
                role ? role : "viewmodel",
                configName ? configName : "unknown",
                matchedWeaponId > 0 ? matchedWeaponId : weaponId,
                modelName.c_str(),
                overrideSpec.c_str());
        }
        return -1;
    }

    inline void DrawCurrentWeaponMagazineBox(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        bool sourceIsViewmodelClass,
        int entityIndex,
        int hitboxSet,
        const void* pModelToWorld,
        const void* pCustomBoneToWorld)
    {
        if (!vr || !drawState || !pCustomBoneToWorld || modelName.empty())
        {
            return;
        }

        const bool calibrationOverlayActive =
            vr->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed);
        const bool wantsMagazineInteractionBox =
            vr->m_MagazineInteractionEnabled ||
            vr->m_MagazineBoxDebugEnabled ||
            calibrationOverlayActive;
        const bool wantsVrHandsMagazineExclusionBox =
            vr->m_IsVREnabled &&
            (vr->m_VrHandsEnabled || vr->m_NativeViewmodelHandsOnly);
        constexpr bool wantsTwoHandedGripWeaponBox = false;
        const bool wantsMagazineBox =
            wantsMagazineInteractionBox ||
            wantsVrHandsMagazineExclusionBox;
        if (!wantsMagazineBox)
            return;
        const bool logMagazineBoxDiagnostics = ShouldLogMagazineBoxDiagnostics(vr);
        constexpr bool drawDebugBox = false;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (HooksModelNameIsArmsOrHands(lowerModel))
            return;
        const bool sourceIsViewmodelPath = HooksModelNameIsViewmodel(lowerModel);
        const bool sourceHasLooseViewmodelMarker = HooksModelNameHasLooseViewmodelMarker(lowerModel);
        const bool allowSlowBoneNameProbe =
            calibrationOverlayActive ||
            vr->m_MagazineBoxDebugEnabled;
        if (!sourceIsViewmodelClass &&
            !sourceIsViewmodelPath &&
            !sourceHasLooseViewmodelMarker &&
            !allowSlowBoneNameProbe)
        {
            return;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return;
        }
        const bool sourceLooksLikeWeaponBones =
            (!sourceIsViewmodelClass &&
                !sourceIsViewmodelPath &&
                !sourceHasLooseViewmodelMarker)
            ? HooksCalibrationBoneNamesLookLikeWeapon(boneNames)
            : true;
        if (!sourceIsViewmodelPath &&
            !sourceHasLooseViewmodelMarker &&
            !sourceIsViewmodelClass &&
            !sourceLooksLikeWeaponBones)
        {
            return;
        }

        const int currentMagazineInteractionWeaponId =
            vr->m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed);
        const int inferredModelWeaponId = MagazineInteractionInferWeaponIdFromViewmodelModelName(lowerModel);
        const int magazineInteractionWeaponId = inferredModelWeaponId > 0
            ? inferredModelWeaponId
            : currentMagazineInteractionWeaponId > 0
            ? currentMagazineInteractionWeaponId
            : vr->m_MagazineInteractionWeaponId;
        const uint32_t boneSignature = HooksBuildViewmodelBoneSignature(
            modelName,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);
        const uint32_t modelFingerprint = HooksBuildStudioHdrFingerprint(drawState, boneSignature);
        const std::string overrideProfileKey =
            HooksBuildMagazineInteractionProfileKey(modelFingerprint, boneSignature);
        vr->m_MagazineInteractionCurrentModelFingerprint.store(modelFingerprint, std::memory_order_relaxed);
        vr->m_MagazineInteractionCurrentBoneSignature.store(boneSignature, std::memory_order_relaxed);

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        const uint32_t frameSeq = vr->m_RenderFrameSeq.load(std::memory_order_relaxed);
        if (frameSeq != 0)
        {
            static std::mutex s_drawMutex;
            static std::unordered_map<std::string, uint32_t> s_lastDrawSeq;
            const std::string key = std::to_string(entityIndex) + "|" + lowerModel;
            std::lock_guard<std::mutex> lock(s_drawMutex);
            uint32_t& lastSeq = s_lastDrawSeq[key];
            if (lastSeq == frameSeq)
                return;
            lastSeq = frameSeq;
        }

        vr_vm_stabilize::Mat3x4 modelWorld{};
        bool modelBasisValid =
            pModelToWorld != nullptr &&
            vr_vm_stabilize::SafeRead(
                reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pModelToWorld),
                modelWorld);

        vr_vm_stabilize::Mat3x4 weaponBoxWorld = modelWorld;
        bool weaponBoxWorldValid = modelBasisValid && HooksMatrixLooksUsable(weaponBoxWorld);
        if (!weaponBoxWorldValid)
        {
            vr_vm_stabilize::Mat3x4 rootWorld{};
            if (vr_vm_stabilize::SafeRead(sourceBones, rootWorld) && HooksMatrixLooksUsable(rootWorld))
            {
                weaponBoxWorld = rootWorld;
                weaponBoxWorldValid = true;
            }
        }

        if (wantsTwoHandedGripWeaponBox && weaponBoxWorldValid)
        {
            Vector weaponBoxMins(0.0f, 0.0f, 0.0f);
            Vector weaponBoxMaxs(0.0f, 0.0f, 0.0f);
            int weaponBoxSampleCount = 0;
            const char* weaponBoxBoundsSource = "none";
            const float scale = (std::isfinite(vr->m_VRScale) && vr->m_VRScale > 0.001f) ? vr->m_VRScale : 43.2f;
            if (BuildVrHandsTwoHandedGripWeaponBoxLocalBounds(
                drawState,
                hitboxSet,
                numBonesOffset,
                boneNames,
                sourceBones,
                numBones,
                weaponBoxWorld,
                scale,
                vr->m_VrHandsTwoHandedGripTargetBoxScale,
                weaponBoxMins,
                weaponBoxMaxs,
                weaponBoxSampleCount,
                weaponBoxBoundsSource))
            {
                vr->PublishVrHandsTwoHandedGripWeaponBox(
                    Vector(weaponBoxWorld.m[0][3], weaponBoxWorld.m[1][3], weaponBoxWorld.m[2][3]),
                    Vector(weaponBoxWorld.m[0][0], weaponBoxWorld.m[1][0], weaponBoxWorld.m[2][0]),
                    Vector(weaponBoxWorld.m[0][1], weaponBoxWorld.m[1][1], weaponBoxWorld.m[2][1]),
                    Vector(weaponBoxWorld.m[0][2], weaponBoxWorld.m[1][2], weaponBoxWorld.m[2][2]),
                    weaponBoxMins,
                    weaponBoxMaxs,
                    frameSeq,
                    entityIndex,
                    modelName.c_str());
            }
        }

        if (inferredModelWeaponId > 0 &&
            currentMagazineInteractionWeaponId > 0 &&
            inferredModelWeaponId != currentMagazineInteractionWeaponId)
        {
            static std::mutex s_weaponIdOverrideLogMutex;
            static std::unordered_set<std::string> s_loggedWeaponIdOverrides;
            const std::string logKey =
                lowerModel + "|" +
                std::to_string(currentMagazineInteractionWeaponId) + "|" +
                std::to_string(inferredModelWeaponId);
            bool shouldLog = false;
            {
                std::lock_guard<std::mutex> lock(s_weaponIdOverrideLogMutex);
                shouldLog = s_loggedWeaponIdOverrides.insert(logKey).second;
            }
            if (logMagazineBoxDiagnostics && shouldLog)
            {
                Game::logMsg(
                    "[VR][MagazineBox] viewmodel model weaponId overrides stale current weaponId model=%s currentWeaponId=%d modelWeaponId=%d",
                    lowerModel.c_str(),
                    currentMagazineInteractionWeaponId,
                    inferredModelWeaponId);
            }
        }
        std::string configuredMagazineBoneName;
        int magazineBone = FindConfiguredViewmodelBoneOverride(
            "MagazineInteraction",
            "magazine",
            "ManualReloadMagazineBoneOverrides",
            magazineInteractionWeaponId,
            vr->m_MagazineInteractionMagazineBoneOverrides,
            overrideProfileKey,
            vr->m_MagazineInteractionMagazineBoneProfileOverrides,
            vr->m_MagazineInteractionMagazineBoneOverridesSpec,
            modelName,
            boneNames,
            configuredMagazineBoneName,
            logMagazineBoxDiagnostics);
        const std::string normalizedOverrideProfileKey =
            vr_vm_stabilize::ToLowerAscii(overrideProfileKey);
        const auto profileMagazineBoneIt =
            normalizedOverrideProfileKey.empty()
            ? vr->m_MagazineInteractionMagazineBoneProfileOverrides.end()
            : vr->m_MagazineInteractionMagazineBoneProfileOverrides.find(normalizedOverrideProfileKey);
        const bool profileMagazineBoneOverride =
            !configuredMagazineBoneName.empty() &&
            profileMagazineBoneIt != vr->m_MagazineInteractionMagazineBoneProfileOverrides.end() &&
            !profileMagazineBoneIt->second.empty();
        if (magazineBone < 0 && MagazineInteractionWeaponIdIsShotgun(magazineInteractionWeaponId))
            magazineBone = FindMagazineInteractionShotgunShellBone(lowerModel, boneNames, logMagazineBoxDiagnostics);
        if (magazineBone < 0)
            magazineBone = FindMagazineBoxBone(boneNames);
        if (magazineBone < 0)
            magazineBone = FindMagazineBoxOfficialProfileFallbackBone(lowerModel, boneNames, logMagazineBoxDiagnostics);
        const int calibrationStep = std::clamp(
            vr->m_MagazineInteractionCalibrationStep.load(std::memory_order_relaxed),
            0,
            5);
        const int calibrationSelectedBone =
            vr->m_MagazineInteractionCalibrationSelectedBone.load(std::memory_order_relaxed);
        if (calibrationOverlayActive &&
            calibrationStep <= 2 &&
            calibrationSelectedBone >= 0 &&
            calibrationSelectedBone < numBones)
        {
            magazineBone = calibrationSelectedBone;
        }
        if (magazineBone < 0 || magazineBone >= numBones)
        {
            if (MagazineBoxOfficialProfileExists(lowerModel))
            {
                int namedBoneCount = 0;
                for (const std::string& boneName : boneNames)
                {
                    if (!boneName.empty())
                        ++namedBoneCount;
                }

                static std::mutex s_missingMagazineBoneLogMutex;
                static std::unordered_set<std::string> s_loggedMissingMagazineBoneModels;
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(s_missingMagazineBoneLogMutex);
                    shouldLog = s_loggedMissingMagazineBoneModels.insert(lowerModel).second;
                }
                if (logMagazineBoxDiagnostics && shouldLog)
                {
                    Game::logMsg(
                        "[VR][MagazineBox] no magazine bone candidate model=%s weaponId=%d bones=%d namedBones=%d officialProfile=1 hint=VMProbe only exposed arm/hand bones; use ManualReloadMagazineBoneOverrides only if a clip/mag bone appears",
                        modelName.c_str(),
                        magazineInteractionWeaponId,
                        numBones,
                        namedBoneCount);
                }
            }
            return;
        }

        vr_vm_stabilize::Mat3x4 magazineWorld{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + magazineBone, magazineWorld))
            return;

        const std::string magazineBoneName =
            (magazineBone < static_cast<int>(boneNames.size()) && !boneNames[static_cast<size_t>(magazineBone)].empty())
            ? boneNames[static_cast<size_t>(magazineBone)]
            : std::string();
        const std::string lowerMagazineBoneName = vr_vm_stabilize::ToLowerAscii(magazineBoneName);
        const bool magazineInteractionIsShotgun =
            MagazineInteractionWeaponIdIsShotgun(magazineInteractionWeaponId);
        const bool magazineBoneUsesStockProfileAxes =
            MagazineInteractionNameIsLegacyValveBipedClip(lowerMagazineBoneName) ||
            (magazineInteractionIsShotgun &&
                MagazineInteractionShotgunShellBoneUsesStockProfileAxes(lowerMagazineBoneName));

        const Vector fallbackHalf(
            std::max(0.001f, vr->m_MagazineBoxDebugFallbackHalfExtentsMeters.x) * vr->m_VRScale,
            std::max(0.001f, vr->m_MagazineBoxDebugFallbackHalfExtentsMeters.y) * vr->m_VRScale,
            std::max(0.001f, vr->m_MagazineBoxDebugFallbackHalfExtentsMeters.z) * vr->m_VRScale);
        const Vector padding(
            std::max(0.0f, vr->m_MagazineBoxDebugPaddingMeters.x) * vr->m_VRScale,
            std::max(0.0f, vr->m_MagazineBoxDebugPaddingMeters.y) * vr->m_VRScale,
            std::max(0.0f, vr->m_MagazineBoxDebugPaddingMeters.z) * vr->m_VRScale);

        const Vector insertionAxisLocal = HooksNormalizeVector(
            vr->m_MagazineInteractionMagazineInsertionAxisLocal,
            Vector(0.0f, -1.0f, 0.0f));
        const float magazineLengthHalf = std::max(fallbackHalf.x, std::max(fallbackHalf.y, fallbackHalf.z));
        int sampleCount = 0;
        int lengthAxis = HooksDominantAxis(insertionAxisLocal);
        Vector mins;
        Vector maxs;
        const char* boundsSource = "fallback";
        auto useFallbackBounds = [&]()
            {
                sampleCount = 1;
                lengthAxis = HooksDominantAxis(insertionAxisLocal);
                const Vector centerLocal = insertionAxisLocal * magazineLengthHalf;
                mins = centerLocal - fallbackHalf - padding;
                maxs = centerLocal + fallbackHalf + padding;
                boundsSource = "fallback";
            };
        if (magazineBoneUsesStockProfileAxes &&
            TryGetOfficialMagazineBoxProfile(
                lowerModel,
                padding,
                mins,
                maxs,
                sampleCount,
                lengthAxis))
        {
            boundsSource = "official";
        }
        else if (BuildMagazineBoxLocalBoundsFromHitboxes(
            drawState,
            hitboxSet,
            numBonesOffset,
            boneNames,
            boneParents,
            sourceBones,
            numBones,
            magazineBone,
            magazineWorld,
            padding,
            mins,
            maxs,
            sampleCount,
            lengthAxis))
        {
            boundsSource = "hitbox";
        }
        else if (BuildMagazineBoxLocalBoundsFromBoneSamples(
            boneNames,
            boneParents,
            sourceBones,
            numBones,
            magazineBone,
            magazineWorld,
            fallbackHalf,
            padding,
            insertionAxisLocal,
            mins,
            maxs,
            sampleCount,
            lengthAxis))
        {
            boundsSource = "bones";
        }
        else
        {
            useFallbackBounds();
        }

        if (std::strcmp(boundsSource, "official") != 0)
        {
            const Vector span = maxs - mins;
            const int clampedLengthAxis = std::clamp(lengthAxis, 0, 2);
            const float maxAllowed[3] =
            {
                std::max(fallbackHalf.x * 2.4f, ((clampedLengthAxis == 0) ? 0.18f : 0.035f) * vr->m_VRScale),
                std::max(fallbackHalf.y * 2.4f, ((clampedLengthAxis == 1) ? 0.18f : 0.035f) * vr->m_VRScale),
                std::max(fallbackHalf.z * 2.4f, ((clampedLengthAxis == 2) ? 0.18f : 0.035f) * vr->m_VRScale)
            };
            float effectiveMaxAllowed[3] = { maxAllowed[0], maxAllowed[1], maxAllowed[2] };
            if (profileMagazineBoneOverride)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float relaxedMeters = (axis == clampedLengthAxis) ? 0.30f : 0.12f;
                    effectiveMaxAllowed[axis] =
                        std::max(effectiveMaxAllowed[axis], relaxedMeters * vr->m_VRScale);
                }
            }
            if (span.x > effectiveMaxAllowed[0] || span.y > effectiveMaxAllowed[1] || span.z > effectiveMaxAllowed[2])
            {
                const char* rejectedSource = boundsSource;
                useFallbackBounds();
                boundsSource = "fallback-clamped";
                static std::mutex s_clampLogMutex;
                static std::unordered_map<std::string, bool> s_clampLoggedModels;
                std::lock_guard<std::mutex> lock(s_clampLogMutex);
                if (logMagazineBoxDiagnostics && s_clampLoggedModels.emplace(lowerModel, true).second)
                {
                    Game::logMsg(
                        "[VR][MagazineBox] rejected oversized %s bounds model=%s span=(%.2f %.2f %.2f) allowed=(%.2f %.2f %.2f)",
                        rejectedSource,
                        lowerModel.c_str(),
                        span.x,
                        span.y,
                        span.z,
                        effectiveMaxAllowed[0],
                        effectiveMaxAllowed[1],
                        effectiveMaxAllowed[2]);
                }
            }
        }

        vr_vm_stabilize::Mat3x4 boxWorld = magazineWorld;
        int basisBone = magazineBone;
        const char* basisSource = "magazine-bone";
        const bool officialCustomMagazineBone =
            std::strcmp(boundsSource, "official") == 0 &&
            !magazineBoneUsesStockProfileAxes;
        if (officialCustomMagazineBone)
        {
            const int directOffsetAnchorBone = FindMagazineBoxDirectOffsetAnchorBone(
                boneNames,
                boneParents,
                magazineBone);
            if (directOffsetAnchorBone >= 0 && directOffsetAnchorBone < numBones)
            {
                vr_vm_stabilize::Mat3x4 anchorWorld{};
                if (vr_vm_stabilize::SafeRead(sourceBones + directOffsetAnchorBone, anchorWorld))
                {
                    vr_vm_stabilize::Mat3x4 directOffsetBasisWorld{};
                    if (BuildMagazineBoxBasisFromParentOffset(
                        magazineWorld,
                        anchorWorld,
                        pModelToWorld,
                        insertionAxisLocal,
                        lengthAxis,
                        directOffsetBasisWorld))
                    {
                        boxWorld = directOffsetBasisWorld;
                        basisBone = directOffsetAnchorBone;
                        basisSource = "direct-parent-offset-axis";
                    }
                }
            }

            if (basisBone == magazineBone)
            {
                const int parentBasisBone = FindMagazineBoxParentBasisBone(boneNames, boneParents, magazineBone);
                if (parentBasisBone >= 0 && parentBasisBone < numBones)
                {
                    vr_vm_stabilize::Mat3x4 parentWorld{};
                    if (vr_vm_stabilize::SafeRead(sourceBones + parentBasisBone, parentWorld))
                    {
                        vr_vm_stabilize::Mat3x4 parentOffsetBasisWorld{};
                        if (BuildMagazineBoxBasisFromParentOffset(
                            magazineWorld,
                            parentWorld,
                            pModelToWorld,
                            insertionAxisLocal,
                            lengthAxis,
                            parentOffsetBasisWorld))
                        {
                            boxWorld = parentOffsetBasisWorld;
                            basisBone = parentBasisBone;
                            basisSource = "parent-offset-axis";
                        }
                        else
                        {
                            boxWorld = parentWorld;
                            boxWorld.m[0][3] = magazineWorld.m[0][3];
                            boxWorld.m[1][3] = magazineWorld.m[1][3];
                            boxWorld.m[2][3] = magazineWorld.m[2][3];
                            basisBone = parentBasisBone;
                            basisSource = "parent-axes";
                        }
                    }
                }
            }

            if (basisBone == magazineBone)
            {
                const int legacyClipBasisBone = FindMagazineBoxLegacyClipBone(boneNames);
                if (legacyClipBasisBone >= 0 &&
                    legacyClipBasisBone < numBones &&
                    legacyClipBasisBone != magazineBone)
                {
                    vr_vm_stabilize::Mat3x4 legacyClipWorld{};
                    if (vr_vm_stabilize::SafeRead(sourceBones + legacyClipBasisBone, legacyClipWorld))
                    {
                        boxWorld = legacyClipWorld;
                        boxWorld.m[0][3] = magazineWorld.m[0][3];
                        boxWorld.m[1][3] = magazineWorld.m[1][3];
                        boxWorld.m[2][3] = magazineWorld.m[2][3];
                        basisBone = legacyClipBasisBone;
                        basisSource = "legacy-clip-axes";
                    }
                }
            }
        }

        if (magazineInteractionIsShotgun)
        {
            const int stableAnchorBone = FindMagazineInteractionShotgunStableAnchorBone(
                boneNames,
                boneParents,
                magazineBone);
            if (stableAnchorBone >= 0 && stableAnchorBone < numBones)
            {
                vr_vm_stabilize::Mat3x4 stableAnchorWorld{};
                if (vr_vm_stabilize::SafeRead(sourceBones + stableAnchorBone, stableAnchorWorld))
                {
                    modelWorld = stableAnchorWorld;
                    modelBasisValid = true;

                    static std::mutex s_shotgunAnchorLogMutex;
                    static std::unordered_set<std::string> s_loggedShotgunAnchors;
                    const std::string stableAnchorName =
                        (stableAnchorBone < static_cast<int>(boneNames.size()))
                        ? boneNames[static_cast<size_t>(stableAnchorBone)]
                        : std::string();
                    const std::string logKey =
                        lowerModel + "|" +
                        std::to_string(magazineBone) + "|" +
                        std::to_string(stableAnchorBone);
                    bool shouldLog = false;
                    {
                        std::lock_guard<std::mutex> lock(s_shotgunAnchorLogMutex);
                        shouldLog = s_loggedShotgunAnchors.insert(logKey).second;
                    }
                    if (logMagazineBoxDiagnostics && shouldLog)
                    {
                        Game::logMsg(
                            "[VR][MagazineBox] shotgun stable socket anchor model=%s shellBone=%d shellName=%s anchorBone=%d anchorName=%s",
                            lowerModel.c_str(),
                            magazineBone,
                            magazineBoneName.c_str(),
                            stableAnchorBone,
                            stableAnchorName.c_str());
                    }
                }
            }
        }

        ApplyMagazineInteractionMagazineBoxCalibrationAdjustments(vr, boxWorld, mins, maxs);

        vr->PublishMagazineInteractionBox(
            Vector(boxWorld.m[0][3], boxWorld.m[1][3], boxWorld.m[2][3]),
            Vector(boxWorld.m[0][0], boxWorld.m[1][0], boxWorld.m[2][0]),
            Vector(boxWorld.m[0][1], boxWorld.m[1][1], boxWorld.m[2][1]),
            Vector(boxWorld.m[0][2], boxWorld.m[1][2], boxWorld.m[2][2]),
            mins,
            maxs,
            frameSeq,
            entityIndex,
            magazineBone,
            modelName.c_str(),
            modelBasisValid,
            Vector(modelWorld.m[0][3], modelWorld.m[1][3], modelWorld.m[2][3]),
            Vector(modelWorld.m[0][0], modelWorld.m[1][0], modelWorld.m[2][0]),
            Vector(modelWorld.m[0][1], modelWorld.m[1][1], modelWorld.m[2][1]),
            Vector(modelWorld.m[0][2], modelWorld.m[1][2], modelWorld.m[2][2]));

        std::string configuredBoltBoneName;
        int boltBone = FindConfiguredViewmodelBoneOverride(
            "MagazineInteraction",
            "bolt",
            "MagazineInteractionBoltBoneOverrides",
            magazineInteractionWeaponId,
            vr->m_MagazineInteractionBoltBoneOverrides,
            overrideProfileKey,
            vr->m_MagazineInteractionBoltBoneProfileOverrides,
            vr->m_MagazineInteractionBoltBoneOverridesSpec,
            modelName,
            boneNames,
            configuredBoltBoneName,
            logMagazineBoxDiagnostics);
        if (boltBone < 0)
            boltBone = FindMagazineInteractionBoltBone(
                magazineInteractionWeaponId,
                lowerModel,
                boneNames,
                logMagazineBoxDiagnostics);
        if (calibrationOverlayActive)
        {
            const int calibrationStep = std::clamp(
                vr->m_MagazineInteractionCalibrationStep.load(std::memory_order_relaxed),
                0,
                5);
            const int calibrationSelectedBone =
                vr->m_MagazineInteractionCalibrationSelectedBone.load(std::memory_order_relaxed);
            if (calibrationStep >= 3 &&
                calibrationSelectedBone >= 0 &&
                calibrationSelectedBone < numBones)
            {
                boltBone = calibrationSelectedBone;
            }
        }
        if (boltBone >= 0 && boltBone < numBones)
        {
            vr_vm_stabilize::Mat3x4 boltWorld{};
            if (vr_vm_stabilize::SafeRead(sourceBones + boltBone, boltWorld))
            {
                const Vector boltHalfMeters =
                    ResolveMagazineInteractionBoltBoxHalfExtentsMeters(vr, magazineInteractionWeaponId, overrideProfileKey);
                const Vector boltHalf(
                    std::max(0.005f, boltHalfMeters.x) * vr->m_VRScale,
                    std::max(0.005f, boltHalfMeters.y) * vr->m_VRScale,
                    std::max(0.005f, boltHalfMeters.z) * vr->m_VRScale);
                const Vector boltOffsetMeters =
                    ResolveMagazineInteractionBoltBoxLocalOffsetMeters(vr, magazineInteractionWeaponId, overrideProfileKey);
                const Vector boltLocalOffset(
                    boltOffsetMeters.x * vr->m_VRScale,
                    boltOffsetMeters.y * vr->m_VRScale,
                    boltOffsetMeters.z * vr->m_VRScale);
                const Vector pullAxisWorld = BuildMagazineInteractionBoltPullAxisWorld(
                    vr,
                    modelName,
                    sourceBones,
                    numBones,
                    boneParents,
                    magazineInteractionWeaponId,
                    boltBone,
                    boltWorld,
                    overrideProfileKey,
                    pModelToWorld);
                vr->PublishMagazineInteractionBoltBox(
                    Vector(boltWorld.m[0][3], boltWorld.m[1][3], boltWorld.m[2][3]),
                    Vector(boltWorld.m[0][0], boltWorld.m[1][0], boltWorld.m[2][0]),
                    Vector(boltWorld.m[0][1], boltWorld.m[1][1], boltWorld.m[2][1]),
                    Vector(boltWorld.m[0][2], boltWorld.m[1][2], boltWorld.m[2][2]),
                    boltLocalOffset - boltHalf,
                    boltLocalOffset + boltHalf,
                    pullAxisWorld,
                    frameSeq,
                    entityIndex,
                    boltBone,
                    modelName.c_str());

                {
                    static std::mutex s_boltBoxLogMutex;
                    static std::unordered_set<std::string> s_loggedBoltBoxes;
                    const char* boltBoneName =
                        (static_cast<size_t>(boltBone) < boneNames.size())
                        ? boneNames[static_cast<size_t>(boltBone)].c_str()
                        : "";
                    char key[256] = {};
                    std::snprintf(
                        key,
                        sizeof(key),
                        "%s|%d|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f",
                        lowerModel.c_str(),
                        boltBone,
                        boltHalfMeters.x,
                        boltHalfMeters.y,
                        boltHalfMeters.z,
                        boltOffsetMeters.x,
                        boltOffsetMeters.y,
                        boltOffsetMeters.z);
                    bool shouldLog = false;
                    {
                        std::lock_guard<std::mutex> lock(s_boltBoxLogMutex);
                        shouldLog = s_loggedBoltBoxes.insert(key).second;
                    }
                    if (logMagazineBoxDiagnostics && shouldLog)
                    {
                        Game::logMsg(
                            "[VR][MagazineBolt] drawing bolt box model=%s bone=%d name=%s mins=(%.2f %.2f %.2f) maxs=(%.2f %.2f %.2f) pullAxis=(%.3f %.3f %.3f)",
                            modelName.c_str(),
                            boltBone,
                            boltBoneName,
                            boltLocalOffset.x - boltHalf.x,
                            boltLocalOffset.y - boltHalf.y,
                            boltLocalOffset.z - boltHalf.z,
                            boltLocalOffset.x + boltHalf.x,
                            boltLocalOffset.y + boltHalf.y,
                            boltLocalOffset.z + boltHalf.z,
                            pullAxisWorld.x,
                            pullAxisWorld.y,
                            pullAxisWorld.z);
                    }
                }
            }
        }

        if (drawDebugBox)
        {
            const float duration = std::max(0.02f, vr->m_LastFrameDuration * 2.0f);
            constexpr int kR = 255;
            constexpr int kG = 72;
            constexpr int kB = 24;
            constexpr int kA = 220;
            constexpr bool kNoDepthTest = true;
            IVDebugOverlay* overlay = vr->m_Game->m_DebugOverlay;
            DrawMagazineBoxSolidObb(overlay, boxWorld, mins, maxs, kR, kG, kB, kA, kNoDepthTest, duration);
        }

        static std::mutex s_logMutex;
        static std::unordered_map<std::string, std::string> s_loggedSignatureByModel;
        {
            std::lock_guard<std::mutex> lock(s_logMutex);
            const std::string logSignature =
                std::to_string(magazineBone) + "|" +
                std::to_string(basisBone) + "|" +
                (basisSource ? basisSource : "unknown") + "|" +
                (boundsSource ? boundsSource : "unknown");
            auto it = s_loggedSignatureByModel.find(lowerModel);
            if (logMagazineBoxDiagnostics && (it == s_loggedSignatureByModel.end() || it->second != logSignature))
            {
                s_loggedSignatureByModel[lowerModel] = logSignature;
                const char* boneName =
                    !magazineBoneName.empty()
                    ? magazineBoneName.c_str()
                    : "<unnamed>";
                const char* basisName =
                    (basisBone >= 0 &&
                        basisBone < static_cast<int>(boneNames.size()) &&
                        !boneNames[static_cast<size_t>(basisBone)].empty())
                    ? boneNames[static_cast<size_t>(basisBone)].c_str()
                    : "<unnamed>";
                Game::logMsg(
                    "[VR][MagazineBox] drawing solid magazine box model=%s bone=%d name=%s basisBone=%d basisName=%s basis=%s source=%s samples=%d axis=%d mins=(%.2f %.2f %.2f) maxs=(%.2f %.2f %.2f)",
                    modelName.c_str(),
                    magazineBone,
                    boneName,
                    basisBone,
                    basisName,
                    basisSource,
                    boundsSource,
                    sampleCount,
                    lengthAxis,
                    mins.x,
                    mins.y,
                    mins.z,
                    maxs.x,
                    maxs.y,
                    maxs.z);
            }
        }
    }

    struct MagazineInteractionDetachedMagazinePoseCache
    {
        VR* owner = nullptr;
        std::string modelName;
        int clipBone = -1;
        int numBones = 0;
        std::vector<vr_vm_stabilize::Mat3x4> sourceBones;

        void Reset()
        {
            owner = nullptr;
            modelName.clear();
            clipBone = -1;
            numBones = 0;
            sourceBones.clear();
        }
    };

    struct MagazineInteractionRenderSnapshotScope
    {
        bool previous = false;
        bool active = false;

        explicit MagazineInteractionRenderSnapshotScope(bool enabled)
            : previous(VR::t_UseRenderFrameSnapshot)
            , active(enabled)
        {
            if (active)
                VR::t_UseRenderFrameSnapshot = true;
        }

        ~MagazineInteractionRenderSnapshotScope()
        {
            if (active)
                VR::t_UseRenderFrameSnapshot = previous;
        }
    };

    inline MagazineInteractionDetachedMagazinePoseCache& GetMagazineInteractionDetachedMagazinePoseCache()
    {
        static MagazineInteractionDetachedMagazinePoseCache cache;
        return cache;
    }

    inline void LogMagazineInteractionDetachedDrawSkip(const char* reason, const std::string& modelName = std::string())
    {
        (void)reason;
        (void)modelName;
    }

    inline bool BuildDetachedSourceMagazineBones(
        const char* logPrefix,
        const std::string& modelName,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int clipBone,
        const VrHandMatrix4& targetMagazineWorld,
        const vr_vm_stabilize::Mat3x4* cachedMagazineBones,
        uint32_t renderFrameSeq,
        bool logDiagnostics,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!sourceBones)
            return false;
        if (numBones <= 0 || numBones > 512 || clipBone < 0 || clipBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
            return false;

        const vr_vm_stabilize::Mat3x4* magazineSourceBones = cachedMagazineBones ? cachedMagazineBones : sourceBones;
        vr_vm_stabilize::Mat3x4 originalClipWorld{};
        if (!vr_vm_stabilize::SafeRead(magazineSourceBones + clipBone, originalClipWorld))
            return false;

        vr_vm_stabilize::Mat3x4 inverseOriginalClip{};
        vr_vm_stabilize::Mat3x4 targetClipWorld = HooksVrHandMatrixToMat3x4(
            HooksStripVrHandMatrixScale(targetMagazineWorld));
        vr_vm_stabilize::Mat3x4 targetDelta{};
        vr_vm_stabilize::InvertTR(originalClipWorld, inverseOriginalClip);
        vr_vm_stabilize::Mul(targetClipWorld, inverseOriginalClip, targetDelta);

        uint32_t seqEven = renderFrameSeq & ~1u;
        if (seqEven == 0)
            seqEven = (static_cast<uint32_t>(GetTickCount()) << 1u) | 2u;
        vr_vm_stabilize::Mat3x4* isolatedBones = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!isolatedBones)
            return false;

        auto isClipOrDescendant = [&](int bone)
            {
                int current = bone;
                for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
                {
                    if (current == clipBone)
                        return true;
                    current = boneParents[static_cast<size_t>(current)];
                }
                return false;
            };

        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return false;

            if (isClipOrDescendant(bone))
            {
                if (!vr_vm_stabilize::SafeRead(magazineSourceBones + bone, source))
                    return false;
                vr_vm_stabilize::Mul(targetDelta, source, isolatedBones[bone]);
            }
            else
            {
                isolatedBones[bone] = source;
                isolatedBones[bone].m[0][3] += 100000.0f;
                isolatedBones[bone].m[1][3] += 100000.0f;
                isolatedBones[bone].m[2][3] += 100000.0f;
            }
        }

        static std::mutex s_detachedSourceMagazineLogMutex;
        static std::unordered_set<std::string> s_detachedSourceMagazineLoggedModels;
        {
            std::lock_guard<std::mutex> lock(s_detachedSourceMagazineLogMutex);
            const std::string key = std::string(logPrefix ? logPrefix : "Magazine") + "|" + modelName + "|" + std::to_string(clipBone);
            if (logDiagnostics && s_detachedSourceMagazineLoggedModels.emplace(key).second)
            {
                Game::logMsg(
                    "[VR][%s] drawing detached magazine through Source viewmodel shader model=%s clipBone=%d cached=%d",
                    logPrefix ? logPrefix : "Magazine",
                    modelName.c_str(),
                    clipBone,
                    cachedMagazineBones ? 1 : 0);
            }
        }

        outBones = isolatedBones;
        return true;
    }

    inline bool BuildMagazineInteractionDetachedMagazineBones(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const void* pCustomBoneToWorld,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!vr || !vr->ShouldDrawMagazineInteractionDetachedMagazine() || !drawState || !pCustomBoneToWorld)
        {
            if (vr && vr->IsMagazineInteractionManualActive())
                LogMagazineInteractionDetachedDrawSkip("inactive-or-missing-draw-input", modelName);
            return false;
        }
        if (!vr->m_Game)
        {
            LogMagazineInteractionDetachedDrawSkip("missing-game", modelName);
            return false;
        }

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsViewmodel(lowerModel))
        {
            LogMagazineInteractionDetachedDrawSkip("not-viewmodel", modelName);
            return false;
        }
        if (vr->m_MagazineInteractionMagazineModelName.empty() ||
            vr_vm_stabilize::ToLowerAscii(vr->m_MagazineInteractionMagazineModelName) != lowerModel)
        {
            LogMagazineInteractionDetachedDrawSkip("model-mismatch", modelName);
            return false;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            LogMagazineInteractionDetachedDrawSkip("collect-bones-failed", modelName);
            return false;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneParents.size()) < numBones)
        {
            LogMagazineInteractionDetachedDrawSkip("invalid-bone-count", modelName);
            return false;
        }

        const int clipBone = vr->m_MagazineInteractionMagazineBoneIndex;
        if (clipBone < 0 || clipBone >= numBones)
        {
            LogMagazineInteractionDetachedDrawSkip("invalid-clip-bone", modelName);
            return false;
        }

        static std::mutex s_detachedMagazinePoseCacheMutex;
        std::lock_guard<std::mutex> poseCacheLock(s_detachedMagazinePoseCacheMutex);
        MagazineInteractionDetachedMagazinePoseCache& poseCache = GetMagazineInteractionDetachedMagazinePoseCache();
        const bool cacheMatches =
            poseCache.owner == vr &&
            poseCache.modelName == lowerModel &&
            poseCache.clipBone == clipBone &&
            poseCache.numBones == numBones &&
            static_cast<int>(poseCache.sourceBones.size()) == numBones;
        const bool refreshPoseCache =
            vr->m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine &&
            !vr->m_MagazineInteractionReloadTriggered;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        if (refreshPoseCache || !cacheMatches)
        {
            std::vector<vr_vm_stabilize::Mat3x4> capturedBones(static_cast<size_t>(numBones));
            bool captured = true;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!vr_vm_stabilize::SafeRead(sourceBones + bone, capturedBones[static_cast<size_t>(bone)]))
                {
                    captured = false;
                    break;
                }
            }
            if (captured)
            {
                poseCache.owner = vr;
                poseCache.modelName = lowerModel;
                poseCache.clipBone = clipBone;
                poseCache.numBones = numBones;
                poseCache.sourceBones.swap(capturedBones);
            }
            else if (!cacheMatches)
            {
                poseCache.Reset();
            }
        }

        const bool useCachedMagazinePose =
            poseCache.owner == vr &&
            poseCache.modelName == lowerModel &&
            poseCache.clipBone == clipBone &&
            poseCache.numBones == numBones &&
            static_cast<int>(poseCache.sourceBones.size()) == numBones;
        const vr_vm_stabilize::Mat3x4* magazineSourceBones = useCachedMagazinePose
            ? poseCache.sourceBones.data()
            : sourceBones;

        VrHandMatrix4 targetMagazineWorld{};
        if (!vr->GetMagazineInteractionDetachedMagazineWorld(targetMagazineWorld))
        {
            LogMagazineInteractionDetachedDrawSkip("missing-target-world", modelName);
            return false;
        }

        const char* logPrefix =
            (vr->m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
            ? "MagazineInteractionFresh"
            : "MagazineInteraction";
        const bool built = BuildDetachedSourceMagazineBones(
            logPrefix,
            modelName,
            boneParents,
            sourceBones,
            numBones,
            clipBone,
            targetMagazineWorld,
            useCachedMagazinePose ? magazineSourceBones : nullptr,
            vr->m_RenderFrameSeq.load(std::memory_order_relaxed),
            ShouldLogMagazineBoxDiagnostics(vr),
            outBones);
        if (!built)
            LogMagazineInteractionDetachedDrawSkip("build-source-magazine-failed", modelName);
        return built;
    }

    inline void MaybeCaptureVrHandsVmPose(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& modelInfo,
        const void* pCustomBoneToWorld,
        bool autoGripAligned)
    {
        const bool wantsViewmodelHandSnapshot =
            vr &&
            (vr->m_VrHandsEnabled ||
                vr->m_NativeViewmodelHandsOnly ||
                vr->m_VrHandsRightUseViewmodelPose ||
                vr->IsVrHandsTwoHandedGripPoseActive());
        if (!wantsViewmodelHandSnapshot)
            return;
        const bool sourceIsArmsOrHands = HooksModelNameIsArmsOrHands(modelName);
        const bool sourceIsAlignedViewmodel = autoGripAligned && HooksModelNameIsViewmodel(modelName);
        if (!drawState || !pCustomBoneToWorld ||
            (!sourceIsArmsOrHands && !sourceIsAlignedViewmodel))
            return;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones)
            return;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        std::vector<VrHandMatrix4> boneWorldMatrices(static_cast<size_t>(numBones));
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return;
            boneWorldMatrices[static_cast<size_t>(bone)] = HooksMat3x4ToVrHandMatrix(source);
        }

        vr_vm_stabilize::Mat3x4 modelWorld{};
        vr_vm_stabilize::BuildFromOrgAngles(modelInfo.origin, modelInfo.angles, modelWorld);
        VrHandVmPose::Capture(
            modelName.c_str(),
            boneNames,
            boneParents,
            HooksMat3x4ToVrHandMatrix(modelWorld),
            boneWorldMatrices,
            autoGripAligned);
    }

    struct MagazineInteractionFrozenViewmodelPoseEntry
    {
        std::string modelName;
        int numBones = 0;
        int rootBone = -1;
        bool valid = false;
        std::vector<vr_vm_stabilize::Mat3x4> frozenLocalBones;
    };

    struct MagazineInteractionFrozenViewmodelPoseCache
    {
        VR* owner = nullptr;
        std::unordered_map<std::string, MagazineInteractionFrozenViewmodelPoseEntry> models;

        void Reset()
        {
            owner = nullptr;
            models.clear();
        }
    };

    inline MagazineInteractionFrozenViewmodelPoseCache& GetMagazineInteractionFrozenViewmodelPoseCache()
    {
        static MagazineInteractionFrozenViewmodelPoseCache cache;
        return cache;
    }

    inline MagazineInteractionFrozenViewmodelPoseCache& GetManualThrowFrozenViewmodelPoseCache()
    {
        static MagazineInteractionFrozenViewmodelPoseCache cache;
        return cache;
    }

    inline bool TryGetMagazineInteractionModelAnchor(
        const ModelRenderInfo_t& info,
        vr_vm_stabilize::Mat3x4& outAnchor)
    {
        vr_vm_stabilize::BuildFromOrgAngles(info.origin, info.angles, outAnchor);
        return true;
    }

    inline int FindMagazineInteractionTopLevelBone(const std::vector<int>& boneParents, int preferredBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (numBones <= 0)
            return -1;

        int rootBone = (preferredBone >= 0 && preferredBone < numBones) ? preferredBone : 0;
        for (int guard = 0; guard < numBones; ++guard)
        {
            const int parent = boneParents[static_cast<size_t>(rootBone)];
            if (parent < 0 || parent >= numBones || parent == rootBone)
                break;
            rootBone = parent;
        }
        return rootBone;
    }

    inline bool BuildMagazineInteractionLocalBones(
        const vr_vm_stabilize::Mat3x4& modelAnchor,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        std::vector<vr_vm_stabilize::Mat3x4>& outLocalBones)
    {
        if (!sourceBones || numBones <= 0)
            return false;

        vr_vm_stabilize::Mat3x4 inverseAnchor{};
        vr_vm_stabilize::InvertTR(modelAnchor, inverseAnchor);
        outLocalBones.resize(static_cast<size_t>(numBones));
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return false;
            vr_vm_stabilize::Mul(inverseAnchor, source, outLocalBones[static_cast<size_t>(bone)]);
        }
        return true;
    }

    inline void ApplyMagazineInteractionLocalPose(
        const vr_vm_stabilize::Mat3x4& modelAnchor,
        const std::vector<vr_vm_stabilize::Mat3x4>& localBones,
        vr_vm_stabilize::Mat3x4* outBones,
        int numBones)
    {
        if (!outBones || static_cast<int>(localBones.size()) != numBones)
            return;

        for (int bone = 0; bone < numBones; ++bone)
            vr_vm_stabilize::Mul(modelAnchor, localBones[static_cast<size_t>(bone)], outBones[bone]);
    }

    inline void ApplyManualThrowViewmodelAnimationFreeze(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        bool drawEntityIsViewmodelClass,
        const ModelRenderInfo_t& info,
        void*& pCustomBoneToWorld)
    {
        MagazineInteractionFrozenViewmodelPoseCache& frozenCache =
            GetManualThrowFrozenViewmodelPoseCache();

        constexpr uint32_t kThrowableActive = 1u << 0;
        constexpr uint32_t kTriggerHeld = 1u << 1;
        const uint32_t inputState = vr
            ? vr->m_ManualThrowViewmodelInputState.load(std::memory_order_acquire)
            : 0u;
        const bool throwableActive =
            vr &&
            vr->m_IsVREnabled &&
            vr->m_ManualThrowEnabled &&
            (inputState & kThrowableActive) != 0u;
        if (!throwableActive)
        {
            if (!vr || frozenCache.owner == vr)
                frozenCache.Reset();
            return;
        }

        if (!drawState || !pCustomBoneToWorld || modelName.empty())
            return;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!drawEntityIsViewmodelClass && !HooksModelNameIsViewmodel(lowerModel))
            return;

        int numBones = 0;
        if (!vr_vm_stabilize::TryGetNumBonesFromDrawState(drawState, numBones) ||
            numBones <= 0 || numBones > 512)
        {
            return;
        }

        vr_vm_stabilize::Mat3x4 modelAnchor{};
        if (!TryGetMagazineInteractionModelAnchor(info, modelAnchor))
            return;

        if (frozenCache.owner != vr)
        {
            frozenCache.Reset();
            frozenCache.owner = vr;
        }

        const auto* sourceBones =
            reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        MagazineInteractionFrozenViewmodelPoseEntry& frozenPose =
            frozenCache.models[lowerModel];
        const bool poseMatches =
            frozenPose.valid &&
            frozenPose.modelName == modelName &&
            frozenPose.numBones == numBones &&
            static_cast<int>(frozenPose.frozenLocalBones.size()) == numBones;
        const bool triggerHeld = (inputState & kTriggerHeld) != 0u;

        // While the trigger is up, continuously retain the latest prepared idle
        // pose. The first held draw can then restore the last pre-attack pose
        // instead of capturing a viewmodel that has already started moving.
        if (!triggerHeld || !poseMatches)
        {
            frozenPose = {};
            frozenPose.modelName = modelName;
            frozenPose.numBones = numBones;
            frozenPose.valid = BuildMagazineInteractionLocalBones(
                modelAnchor,
                sourceBones,
                numBones,
                frozenPose.frozenLocalBones);
            if (!triggerHeld || !frozenPose.valid)
                return;
        }

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_acquire) & ~1u;
        if (seqEven == 0u)
            seqEven = (static_cast<uint32_t>(GetTickCount()) << 1u) | 2u;
        vr_vm_stabilize::Mat3x4* frozenBones =
            vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!frozenBones)
            return;

        ApplyMagazineInteractionLocalPose(
            modelAnchor,
            frozenPose.frozenLocalBones,
            frozenBones,
            numBones);
        pCustomBoneToWorld = frozenBones;
    }

    inline void ApplyMagazineInteractionBoltPose(
        VR* vr,
        const std::string& modelName,
        const ModelRenderInfo_t& info,
        const std::vector<int>& boneParents,
        int boltBone,
        vr_vm_stabilize::Mat3x4* bones,
        int numBones)
    {
        if (!vr || !vr->ShouldMoveMagazineInteractionBolt() || !bones ||
            boltBone < 0 || boltBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return;
        }

        const float maxPull = std::max(
            0.0f,
            std::max(
                vr->m_MagazineInteractionBoltMaxPullDistance,
                ResolveMagazineInteractionBoltPullDistanceMeters(vr) * vr->m_VRScale));
        const float pullDistance = std::clamp(
            vr->m_MagazineInteractionBoltPullDistance,
            0.0f,
            maxPull);
        if (!(pullDistance > 0.0001f))
            return;

        Vector pullAxis = BuildMagazineInteractionBoltPullAxisWorld(
            vr,
            modelName,
            bones,
            numBones,
            boneParents,
            ResolveMagazineInteractionWeaponIdForConfig(vr),
            boltBone,
            bones[boltBone],
            ResolveMagazineInteractionProfileKeyForConfig(vr),
            info.pModelToWorld);
        pullAxis = HooksNormalizeVector(pullAxis, Vector(0.0f, 0.0f, 0.0f));
        if (pullAxis.Length() <= 0.0001f)
            return;

        const Vector runtimeAxis = HooksNormalizeVector(
            vr->m_MagazineInteractionBoltPullAxisWorld,
            pullAxis);
        if (runtimeAxis.Length() > 0.0001f && DotProduct(pullAxis, runtimeAxis) < 0.0f)
            pullAxis = pullAxis * -1.0f;

        vr_vm_stabilize::Mat3x4 targetBolt = bones[boltBone];
        targetBolt.m[0][3] += pullAxis.x * pullDistance;
        targetBolt.m[1][3] += pullAxis.y * pullDistance;
        targetBolt.m[2][3] += pullAxis.z * pullDistance;

        vr_vm_stabilize::Mat3x4 inverseCurrentBolt{};
        vr_vm_stabilize::InvertTR(bones[boltBone], inverseCurrentBolt);
        vr_vm_stabilize::Mat3x4 delta{};
        vr_vm_stabilize::Mul(targetBolt, inverseCurrentBolt, delta);

        auto isBoltOrDescendant = [&](int bone)
            {
                int current = bone;
                for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
                {
                    if (current == boltBone)
                        return true;
                    current = boneParents[static_cast<size_t>(current)];
                }
                return false;
            };

        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!isBoltOrDescendant(bone))
                continue;

            vr_vm_stabilize::Mat3x4 moved{};
            vr_vm_stabilize::Mul(delta, bones[bone], moved);
            bones[bone] = moved;
        }
    }

    inline int HooksNativeViewmodelHandsOnlyBoneSide(const std::string& lowerName)
    {
        if (lowerName.find("bip01_l_") != std::string::npos ||
            lowerName.find("_l_") != std::string::npos ||
            lowerName.find("_l.") != std::string::npos ||
            lowerName.find(".l_") != std::string::npos ||
            lowerName.find(".l.") != std::string::npos ||
            lowerName.find("left") != std::string::npos ||
            lowerName.find("arml") != std::string::npos)
        {
            return -1;
        }

        if (lowerName.find("bip01_r_") != std::string::npos ||
            lowerName.find("_r_") != std::string::npos ||
            lowerName.find("_r.") != std::string::npos ||
            lowerName.find(".r_") != std::string::npos ||
            lowerName.find(".r.") != std::string::npos ||
            lowerName.find("right") != std::string::npos ||
            lowerName.find("armr") != std::string::npos)
        {
            return 1;
        }

        return 0;
    }

    inline bool HooksNativeViewmodelHandsOnlyFindNamedBone(
        const std::vector<std::string>& boneNames,
        const std::vector<const char*>& needles,
        int side,
        int& outBone)
    {
        outBone = -1;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            if (side != 0 && HooksNativeViewmodelHandsOnlyBoneSide(lowerName) != side)
                continue;

            for (const char* needle : needles)
            {
                if (needle && lowerName.find(needle) != std::string::npos)
                {
                    outBone = bone;
                    return true;
                }
            }
        }
        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameMatchesAny(
        const std::string& lowerName,
        const std::vector<const char*>& needles)
    {
        for (const char* needle : needles)
        {
            if (needle && lowerName.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        int startBone,
        const std::vector<const char*>& needles,
        int side,
        int& outBone)
    {
        outBone = -1;
        if (startBone < 0 || startBone >= numBones ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        int current = boneParents[static_cast<size_t>(startBone)];
        for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(current)]);
            if ((side == 0 || HooksNativeViewmodelHandsOnlyBoneSide(lowerName) == side) &&
                HooksNativeViewmodelHandsOnlyBoneNameMatchesAny(lowerName, needles))
            {
                outBone = current;
                return true;
            }
            current = boneParents[static_cast<size_t>(current)];
        }
        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyFindBestWristBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        int hand,
        const std::vector<const char*>& wristNeedles,
        int side,
        int& outBone)
    {
        if (HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
            boneNames,
            boneParents,
            numBones,
            hand,
            wristNeedles,
            side,
            outBone))
        {
            return true;
        }

        return HooksNativeViewmodelHandsOnlyFindNamedBone(boneNames, wristNeedles, side, outBone);
    }

    inline bool HooksNativeViewmodelHandsOnlyFindBestForearmBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        int hand,
        int side,
        int& outBone)
    {
        const std::vector<const char*> preferredNeedles = {
            "forearm", "lowerarm", "lower_arm", "lower-arm", "ulna", "radius",
        };
        const std::vector<const char*> fallbackNeedles = {
            "elbow", "upperarm",
        };

        if (HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
            boneNames,
            boneParents,
            numBones,
            hand,
            preferredNeedles,
            side,
            outBone))
        {
            return true;
        }

        if (HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            preferredNeedles,
            side,
            outBone))
        {
            return true;
        }

        if (HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
            boneNames,
            boneParents,
            numBones,
            hand,
            fallbackNeedles,
            side,
            outBone))
        {
            return true;
        }

        return HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            fallbackNeedles,
            side,
            outBone);
    }

    inline bool HooksNativeViewmodelHandsOnlyReadBoneRestPos(
        void* drawState,
        int boneIndex,
        int stride,
        int bone,
        Vector& outPos)
    {
        outPos = Vector(0.0f, 0.0f, 0.0f);
        if (!drawState || boneIndex <= 0 || stride <= 0 || bone < 0)
            return false;

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        const size_t boneOffset = static_cast<size_t>(boneIndex) +
            (static_cast<size_t>(stride) * static_cast<size_t>(bone));
        constexpr size_t kMStudioBonePosOffset = 32u;
        if (studioLength > 0 && boneOffset + kMStudioBonePosOffset + sizeof(float) * 3u > static_cast<size_t>(studioLength))
            return false;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        const uint8_t* posBase = studioHdr + boneOffset + kMStudioBonePosOffset;
        if (!vr_vm_stabilize::SafeRead(posBase + 0, x) ||
            !vr_vm_stabilize::SafeRead(posBase + 4, y) ||
            !vr_vm_stabilize::SafeRead(posBase + 8, z))
        {
            return false;
        }

        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return false;

        outPos = Vector(x, y, z);
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyReadBoneRestLocalTransform(
        void* drawState,
        int boneIndex,
        int stride,
        int bone,
        vr_vm_stabilize::Mat3x4& outLocal)
    {
        outLocal = vr_vm_stabilize::Identity();
        if (!drawState || boneIndex <= 0 || stride <= 0 || bone < 0)
            return false;

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        const size_t boneOffset = static_cast<size_t>(boneIndex) +
            (static_cast<size_t>(stride) * static_cast<size_t>(bone));
        constexpr size_t kMStudioBonePosOffset = 32u;
        constexpr size_t kMStudioBoneQuatOffset = 44u;
        constexpr size_t kQuatBytes = sizeof(float) * 4u;
        if (studioLength > 0 &&
            boneOffset + kMStudioBoneQuatOffset + kQuatBytes > static_cast<size_t>(studioLength))
        {
            return false;
        }

        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        float qx = 0.0f;
        float qy = 0.0f;
        float qz = 0.0f;
        float qw = 1.0f;
        const uint8_t* boneBase = studioHdr + boneOffset;
        if (!vr_vm_stabilize::SafeRead(boneBase + kMStudioBonePosOffset + 0u, px) ||
            !vr_vm_stabilize::SafeRead(boneBase + kMStudioBonePosOffset + 4u, py) ||
            !vr_vm_stabilize::SafeRead(boneBase + kMStudioBonePosOffset + 8u, pz) ||
            !vr_vm_stabilize::SafeRead(boneBase + kMStudioBoneQuatOffset + 0u, qx) ||
            !vr_vm_stabilize::SafeRead(boneBase + kMStudioBoneQuatOffset + 4u, qy) ||
            !vr_vm_stabilize::SafeRead(boneBase + kMStudioBoneQuatOffset + 8u, qz) ||
            !vr_vm_stabilize::SafeRead(boneBase + kMStudioBoneQuatOffset + 12u, qw))
        {
            return false;
        }

        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
            !std::isfinite(qx) || !std::isfinite(qy) ||
            !std::isfinite(qz) || !std::isfinite(qw))
        {
            return false;
        }

        const float quatLengthSq = qx * qx + qy * qy + qz * qz + qw * qw;
        if (!std::isfinite(quatLengthSq) || quatLengthSq < 0.000001f)
            return false;

        const float invQuatLength = 1.0f / std::sqrt(quatLengthSq);
        qx *= invQuatLength;
        qy *= invQuatLength;
        qz *= invQuatLength;
        qw *= invQuatLength;

        const float xx = qx * qx;
        const float yy = qy * qy;
        const float zz = qz * qz;
        const float xy = qx * qy;
        const float xz = qx * qz;
        const float yz = qy * qz;
        const float wx = qw * qx;
        const float wy = qw * qy;
        const float wz = qw * qz;

        outLocal.m[0][0] = 1.0f - 2.0f * (yy + zz);
        outLocal.m[0][1] = 2.0f * (xy - wz);
        outLocal.m[0][2] = 2.0f * (xz + wy);
        outLocal.m[1][0] = 2.0f * (xy + wz);
        outLocal.m[1][1] = 1.0f - 2.0f * (xx + zz);
        outLocal.m[1][2] = 2.0f * (yz - wx);
        outLocal.m[2][0] = 2.0f * (xz - wy);
        outLocal.m[2][1] = 2.0f * (yz + wx);
        outLocal.m[2][2] = 1.0f - 2.0f * (xx + yy);
        outLocal.m[0][3] = px;
        outLocal.m[1][3] = py;
        outLocal.m[2][3] = pz;
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyIsAncestor(
        const std::vector<int>& boneParents,
        int child,
        int ancestor,
        int numBones)
    {
        int current = child;
        for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
        {
            if (current == ancestor)
                return true;
            current = boneParents[static_cast<size_t>(current)];
        }
        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyResolveStableBoneLength(
        void* drawState,
        const std::vector<int>& boneParents,
        int numBones,
        int boneIndex,
        int stride,
        int hand,
        int forearm,
        float currentLen,
        float& outLen)
    {
        outLen = 0.0f;
        if (!drawState || numBones <= 0 || hand < 0 || hand >= numBones || forearm < 0 || forearm >= numBones)
            return false;

        auto validateLen = [&](float candidate) -> bool
            {
                if (!std::isfinite(candidate) || candidate < 0.25f || candidate > 256.0f)
                    return false;
                if (std::isfinite(currentLen) && currentLen > 0.25f)
                {
                    if (candidate < currentLen * 0.20f || candidate > currentLen * 5.0f)
                        return false;
                }
                outLen = candidate;
                return true;
            };

        Vector restOffset{};
        if (hand < static_cast<int>(boneParents.size()) &&
            boneParents[static_cast<size_t>(hand)] == forearm &&
            HooksNativeViewmodelHandsOnlyReadBoneRestPos(drawState, boneIndex, stride, hand, restOffset) &&
            validateLen(restOffset.Length()))
        {
            return true;
        }

        if (forearm < static_cast<int>(boneParents.size()) &&
            boneParents[static_cast<size_t>(forearm)] == hand &&
            HooksNativeViewmodelHandsOnlyReadBoneRestPos(drawState, boneIndex, stride, forearm, restOffset) &&
            validateLen(restOffset.Length()))
        {
            return true;
        }

        if (static_cast<int>(boneParents.size()) >= numBones &&
            HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, hand, forearm, numBones))
        {
            Vector accumulated(0.0f, 0.0f, 0.0f);
            int current = hand;
            for (int guard = 0; guard < numBones && current >= 0 && current < numBones && current != forearm; ++guard)
            {
                Vector local{};
                if (!HooksNativeViewmodelHandsOnlyReadBoneRestPos(drawState, boneIndex, stride, current, local))
                    break;
                accumulated += local;
                current = boneParents[static_cast<size_t>(current)];
                if (current == forearm && validateLen(accumulated.Length()))
                    return true;
            }
        }

        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyNormalizePlane(float plane[4])
    {
        if (!plane)
            return false;

        const float lenSq = plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2];
        if (!std::isfinite(lenSq) || lenSq < 0.000001f)
            return false;

        const float invLen = 1.0f / std::sqrt(lenSq);
        plane[0] *= invLen;
        plane[1] *= invLen;
        plane[2] *= invLen;
        plane[3] *= invLen;
        return std::isfinite(plane[0]) && std::isfinite(plane[1]) &&
            std::isfinite(plane[2]) && std::isfinite(plane[3]);
    }

    inline float HooksNativeViewmodelHandsOnlyResolveWristKeepDistance(VR* vr, float stableBoneLen)
    {
        (void)stableBoneLen;
        if (!vr)
            return 0.0f;

        return std::clamp(
            vr->m_NativeViewmodelHandsOnlyWristKeepFraction,
            0.0f,
            8.0f);
    }

    inline float HooksNativeViewmodelHandsOnlyResolveTrimDistance(VR* vr, float stableBoneLen)
    {
        (void)stableBoneLen;
        if (!vr)
            return 0.0f;

        return std::clamp(vr->m_NativeViewmodelHandsOnlyTrimUnits, -32.0f, 32.0f);
    }

    inline float HooksNativeViewmodelHandsOnlyResolveRightAnimationKeepDistance(VR* vr)
    {
        if (!vr)
            return 0.0f;

        return std::clamp(vr->m_NativeViewmodelRightHandAnimationKeepUnits, -16.0f, 16.0f);
    }

    inline float HooksNativeViewmodelHandsOnlyResolveSideTrimDistance(VR* vr, float stableBoneLen, int side)
    {
        if (side == 1)
            return std::clamp(
                -HooksNativeViewmodelHandsOnlyResolveRightAnimationKeepDistance(vr),
                -64.0f,
                32.0f);
        if (side == -1 && vr && vr->IsVrHandsTwoHandedGripPoseActive())
            return std::clamp(
                -HooksNativeViewmodelHandsOnlyResolveRightAnimationKeepDistance(vr),
                -64.0f,
                32.0f);

        return std::clamp(
            HooksNativeViewmodelHandsOnlyResolveTrimDistance(vr, stableBoneLen),
            -64.0f,
            32.0f);
    }

    inline Vector HooksNativeViewmodelHandsOnlyRotateVectorAroundAxis(
        const Vector& value,
        const Vector& axis,
        float degrees)
    {
        const float axisLen = axis.Length();
        if (!std::isfinite(axisLen) || axisLen <= 0.0001f || std::fabs(degrees) <= 0.0001f)
            return value;

        const Vector n = axis * (1.0f / axisLen);
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float radians = degrees * kDegToRad;
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        return value * c + CrossProduct(n, value) * s + n * (DotProduct(n, value) * (1.0f - c));
    }

    inline Vector HooksNativeViewmodelHandsOnlyResolveCutRotationDeg(
        VR* vr,
        int side,
        const std::string* lowerModel = nullptr);
    inline Vector HooksNativeViewmodelHandsOnlyBaseCutRotationDeg(VR* vr, int side);
    inline bool HooksNativeViewmodelHandsOnlyTryResolveExplicitCutRotationDeg(
        VR* vr,
        int side,
        const std::string* lowerModel,
        Vector& outRotation);
    inline bool HooksNativeViewmodelHandsOnlyResolveViewFrame(
        VR* vr,
        Vector& outViewOrigin,
        Vector& outForward,
        Vector& outRight,
        Vector& outUp);

    inline bool HooksNativeViewmodelHandsOnlyBuildStableCutRotationAxes(
        VR* vr,
        Vector outAxes[3])
    {
        if (!outAxes)
            return false;

        outAxes[0] = Vector(1.0f, 0.0f, 0.0f);
        outAxes[1] = Vector(0.0f, 0.0f, 1.0f);
        outAxes[2] = Vector(0.0f, 1.0f, 0.0f);

        Vector viewOrigin{};
        Vector viewForward{};
        Vector viewRight{};
        Vector viewUp{};
        if (HooksNativeViewmodelHandsOnlyResolveViewFrame(vr, viewOrigin, viewForward, viewRight, viewUp))
        {
            outAxes[0] = HooksNormalizeVector(viewRight, outAxes[0]);
            outAxes[1] = HooksNormalizeVector(viewUp, outAxes[1]);
            outAxes[2] = HooksNormalizeVector(viewForward, outAxes[2]);
        }

        return outAxes[0].Length() > 0.0001f &&
            outAxes[1].Length() > 0.0001f &&
            outAxes[2].Length() > 0.0001f;
    }

    inline Vector HooksNativeViewmodelHandsOnlySourceReferenceVectorToWorld(
        const Vector& sourceVector,
        const Vector axes[3])
    {
        return HooksNormalizeVector(
            axes[0] * sourceVector.x +
            axes[2] * sourceVector.y +
            axes[1] * sourceVector.z,
            Vector(0.0f, 0.0f, 0.0f));
    }

    inline Vector HooksNativeViewmodelHandsOnlyCanonicalBaseSourceNormal(int side)
    {
        return HooksNormalizeVector(
            (side < 0)
            ? Vector(-0.3363f, 0.0192f, -0.9416f)
            : Vector(0.2984f, 0.1444f, -0.9434f),
            (side < 0)
            ? Vector(-0.34f, 0.02f, -0.94f)
            : Vector(0.30f, 0.14f, -0.94f));
    }

    inline Vector HooksNativeViewmodelHandsOnlyAutoReferenceCutRotationDeg(int side)
    {
        return (side < 0)
            ? Vector(0.0f, -25.0f, 0.0f)
            : Vector(8.0f, -25.0f, 0.0f);
    }

    inline Vector HooksNativeViewmodelHandsOnlyRotateVectorBetweenNormals(
        const Vector& fromNormal,
        const Vector& toNormal,
        const Vector& value)
    {
        const Vector from = HooksNormalizeVector(fromNormal, Vector(0.0f, 0.0f, 0.0f));
        const Vector to = HooksNormalizeVector(toNormal, Vector(0.0f, 0.0f, 0.0f));
        const Vector input = HooksNormalizeVector(value, Vector(0.0f, 0.0f, 0.0f));
        if (from.Length() <= 0.0001f || to.Length() <= 0.0001f || input.Length() <= 0.0001f)
            return value;

        const float dot = std::clamp(DotProduct(from, to), -1.0f, 1.0f);
        if (dot > 0.9999f)
            return input;

        Vector axis = CrossProduct(from, to);
        float axisLen = axis.Length();
        if (axisLen <= 0.0001f)
        {
            axis = CrossProduct(from, Vector(1.0f, 0.0f, 0.0f));
            axisLen = axis.Length();
            if (axisLen <= 0.0001f)
            {
                axis = CrossProduct(from, Vector(0.0f, 1.0f, 0.0f));
                axisLen = axis.Length();
            }
        }
        if (axisLen <= 0.0001f)
            return input;

        axis *= (1.0f / axisLen);
        const float radians = std::atan2(axisLen, dot);
        constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
        return HooksNormalizeVector(
            HooksNativeViewmodelHandsOnlyRotateVectorAroundAxis(input, axis, radians * kRadToDeg),
            input);
    }

    inline Vector HooksNativeViewmodelHandsOnlyResolveArmBendBaseNormal(
        VR* vr,
        const Vector& forearmNormal,
        const Vector& wristNormal)
    {
        Vector bentNormal = HooksNormalizeVector(forearmNormal, Vector(0.0f, 0.0f, 0.0f));
        if (bentNormal.Length() <= 0.0001f)
            return bentNormal;

        Vector straightNormal = HooksNormalizeVector(wristNormal, bentNormal);
        if (straightNormal.Length() <= 0.0001f)
            straightNormal = bentNormal;
        if (DotProduct(straightNormal, bentNormal) < 0.0f)
            straightNormal *= -1.0f;

        const float bendScale = std::clamp(
            vr ? vr->m_NativeViewmodelHandsOnlyArmBendScale : 1.0f,
            0.0f,
            1.0f);

        Vector blended = HooksNormalizeVector(
            straightNormal * (1.0f - bendScale) + bentNormal * bendScale,
            bentNormal);
        if (blended.Length() <= 0.0001f)
            return bentNormal;
        if (DotProduct(blended, bentNormal) < 0.0f)
            blended *= -1.0f;
        return blended;
    }

    inline Vector HooksNativeViewmodelHandsOnlyApplyCutNormalRotation(
        VR* vr,
        const vr_vm_stabilize::Mat3x4& handBone,
        const Vector& normal,
        int side,
        const Vector& cutRotationDeg)
    {
        (void)handBone;

        if (!vr)
            return normal;

        const Vector rotation = cutRotationDeg;
        if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z))
            return normal;

        Vector rotationAxes[3]{};
        if (!HooksNativeViewmodelHandsOnlyBuildStableCutRotationAxes(vr, rotationAxes))
            return normal;

        Vector out = normal;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float degrees = std::clamp(
                HooksVectorComponent(rotation, axis),
                -89.0f,
                89.0f);
            if (std::fabs(degrees) <= 0.0001f)
                continue;

            const Vector localAxis = HooksNormalizeVector(
                rotationAxes[axis],
                Vector(0.0f, 0.0f, 0.0f));
            if (localAxis.Length() <= 0.0001f)
                continue;

            out = HooksNativeViewmodelHandsOnlyRotateVectorAroundAxis(out, localAxis, degrees);
        }

        out = HooksNormalizeVector(out, normal);
        return (out.Length() > 0.0001f) ? out : normal;
    }

    inline Vector HooksNativeViewmodelHandsOnlyResolveArmBendNormal(
        VR* vr,
        const vr_vm_stabilize::Mat3x4& handBone,
        const Vector& forearmNormal,
        const Vector& wristNormal,
        int side,
        const Vector& cutRotationDeg)
    {
        const Vector bentNormal = HooksNormalizeVector(forearmNormal, Vector(0.0f, 0.0f, 0.0f));
        const Vector blended = HooksNativeViewmodelHandsOnlyResolveArmBendBaseNormal(
            vr,
            forearmNormal,
            wristNormal);

        Vector rotated = HooksNativeViewmodelHandsOnlyApplyCutNormalRotation(
            vr,
            handBone,
            blended,
            side,
            cutRotationDeg);
        if (DotProduct(rotated, bentNormal) < 0.0f)
            rotated *= -1.0f;
        return rotated;
    }

    inline bool HooksNativeViewmodelHandsOnlyTryResolveAutoCanonicalCutNormal(
        VR* vr,
        int side,
        const std::string& lowerModel,
        const Vector& currentBaseNormal,
        const Vector& cutRotationDeg,
        Vector& outNormal)
    {
        outNormal = Vector(0.0f, 0.0f, 0.0f);
        if (!vr || !vr->m_NativeViewmodelHandsOnlyAutoCutRotation || side == 0)
            return false;

        const Vector currentBase =
            HooksNormalizeVector(currentBaseNormal, Vector(0.0f, 0.0f, 0.0f));
        if (currentBase.Length() <= 0.0001f)
            return false;

        Vector axes[3]{};
        if (!HooksNativeViewmodelHandsOnlyBuildStableCutRotationAxes(vr, axes))
            return false;

        const Vector canonicalBase =
            HooksNativeViewmodelHandsOnlySourceReferenceVectorToWorld(
                HooksNativeViewmodelHandsOnlyCanonicalBaseSourceNormal(side),
                axes);
        if (canonicalBase.Length() <= 0.0001f)
            return false;

        const Vector referenceCutRotationDeg =
            HooksNativeViewmodelHandsOnlyAutoReferenceCutRotationDeg(side);
        const vr_vm_stabilize::Mat3x4 ignoredHandBone{};
        const Vector canonicalCut =
            HooksNativeViewmodelHandsOnlyApplyCutNormalRotation(
                vr,
                ignoredHandBone,
                canonicalBase,
                side,
                referenceCutRotationDeg);
        if (canonicalCut.Length() <= 0.0001f)
            return false;

        outNormal = HooksNativeViewmodelHandsOnlyRotateVectorBetweenNormals(
            canonicalBase,
            currentBase,
            canonicalCut);
        outNormal = HooksNormalizeVector(outNormal, currentBase);
        if (outNormal.Length() <= 0.0001f)
            return false;
        const Vector userDeltaRotationDeg(
            cutRotationDeg.x - referenceCutRotationDeg.x,
            cutRotationDeg.y - referenceCutRotationDeg.y,
            cutRotationDeg.z - referenceCutRotationDeg.z);
        if (std::fabs(userDeltaRotationDeg.x) > 0.0001f ||
            std::fabs(userDeltaRotationDeg.y) > 0.0001f ||
            std::fabs(userDeltaRotationDeg.z) > 0.0001f)
        {
            outNormal = HooksNativeViewmodelHandsOnlyApplyCutNormalRotation(
                vr,
                ignoredHandBone,
                outNormal,
                side,
                userDeltaRotationDeg);
            outNormal = HooksNormalizeVector(outNormal, currentBase);
            if (outNormal.Length() <= 0.0001f)
                return false;
        }
        if (DotProduct(outNormal, currentBase) < 0.0f)
            outNormal *= -1.0f;

        if (vr->m_VrHandsDebugLog)
        {
            static std::mutex s_autoCanonicalCutLogMutex;
            static std::unordered_set<std::string> s_loggedAutoCanonicalCut;
            const std::string logKey = lowerModel + "|" + std::to_string(side);
            bool shouldLog = false;
            {
                std::lock_guard<std::mutex> lock(s_autoCanonicalCutLogMutex);
                shouldLog = s_loggedAutoCanonicalCut.insert(logKey).second;
            }
            if (shouldLog)
            {
                Game::logMsg(
                    "[VR][NativeHandsOnly] auto canonical cut normal model=\"%s\" side=%s base=(%.2f %.2f %.2f) ref=(%.2f %.2f %.2f) refCut=(%.2f %.2f %.2f) normal=(%.2f %.2f %.2f) rotation=(%.2f %.2f %.2f) autoRefRotation=(%.2f %.2f %.2f) userDelta=(%.2f %.2f %.2f)",
                    lowerModel.c_str(),
                    side < 0 ? "left" : "right",
                    currentBase.x,
                    currentBase.y,
                    currentBase.z,
                    canonicalBase.x,
                    canonicalBase.y,
                    canonicalBase.z,
                    canonicalCut.x,
                    canonicalCut.y,
                    canonicalCut.z,
                    outNormal.x,
                    outNormal.y,
                    outNormal.z,
                    cutRotationDeg.x,
                    cutRotationDeg.y,
                    cutRotationDeg.z,
                    referenceCutRotationDeg.x,
                    referenceCutRotationDeg.y,
                    referenceCutRotationDeg.z,
                    userDeltaRotationDeg.x,
                    userDeltaRotationDeg.y,
                    userDeltaRotationDeg.z);
            }
        }

        return true;
    }

    inline Vector HooksNativeViewmodelHandsOnlyResolveArmBendNormalForRig(
        VR* vr,
        const vr_vm_stabilize::Mat3x4& handBone,
        const Vector& forearmNormal,
        const Vector& wristNormal,
        int side,
        const std::string& lowerModel,
        const Vector& cutRotationDeg,
        bool allowAutoCanonicalCut,
        bool* outUsedAutoCanonicalCut = nullptr)
    {
        if (outUsedAutoCanonicalCut)
            *outUsedAutoCanonicalCut = false;

        const Vector bentNormal = HooksNormalizeVector(forearmNormal, Vector(0.0f, 0.0f, 0.0f));
        const Vector blended = HooksNativeViewmodelHandsOnlyResolveArmBendBaseNormal(
            vr,
            forearmNormal,
            wristNormal);

        Vector rotated{};
        if (allowAutoCanonicalCut &&
            HooksNativeViewmodelHandsOnlyTryResolveAutoCanonicalCutNormal(
                vr,
                side,
                lowerModel,
                blended,
                cutRotationDeg,
                rotated))
        {
            if (outUsedAutoCanonicalCut)
                *outUsedAutoCanonicalCut = true;
        }
        else
        {
            rotated = HooksNativeViewmodelHandsOnlyApplyCutNormalRotation(
                vr,
                handBone,
                blended,
                side,
                cutRotationDeg);
        }

        if (DotProduct(rotated, bentNormal) < 0.0f)
            rotated *= -1.0f;
        return rotated;
    }

    inline float HooksNativeViewmodelHandsOnlyResolveViewmodelAspect(const CViewSetup& view)
    {
        if (view.width > 0 && view.height > 0)
            return static_cast<float>(view.width) / static_cast<float>(view.height);
        if (view.m_flAspectRatio > 0.001f)
            return view.m_flAspectRatio;
        return 1.0f;
    }

    inline bool HooksNativeViewmodelHandsOnlyWorldPlaneToClipPlane(
        VR* vr,
        const float worldPlane[4],
        float outClipPlane[4])
    {
        if (!vr || !vr->m_VrHandsActiveEyeView || !worldPlane || !outClipPlane)
            return false;

        const CViewSetup& view = *vr->m_VrHandsActiveEyeView;
        const float fov = (view.fovViewmodel > 0.001f) ? view.fovViewmodel : view.fov;
        const float aspect = HooksNativeViewmodelHandsOnlyResolveViewmodelAspect(view);
        const float zNear = (view.zNearViewmodel > 0.001f) ? view.zNearViewmodel : view.zNear;
        const float zFar = (view.zFarViewmodel > zNear) ? view.zFarViewmodel : view.zFar;
        if (!(fov > 0.001f) || !(aspect > 0.001f) || !(zFar > zNear))
            return false;

        const VrHandMatrix4 viewMatrix = VrHandMath::BuildSourceView(view.origin, view.angles);
        const VrHandMatrix4 projection = VrHandMath::BuildPerspective(fov, aspect, zNear, zFar);
        const VrHandMatrix4 worldToClip = VrHandMath::Multiply(projection, viewMatrix);

        VrHandMatrix4 clipToWorld{};
        if (!VrHandMath::Invert4x4(worldToClip, clipToWorld))
            return false;

        for (int i = 0; i < 4; ++i)
        {
            outClipPlane[i] =
                VrHandMath::Get(clipToWorld, 0, i) * worldPlane[0] +
                VrHandMath::Get(clipToWorld, 1, i) * worldPlane[1] +
                VrHandMath::Get(clipToWorld, 2, i) * worldPlane[2] +
                VrHandMath::Get(clipToWorld, 3, i) * worldPlane[3];
        }

        return HooksNativeViewmodelHandsOnlyNormalizePlane(outClipPlane);
    }

    struct HooksNativeViewmodelHandsOnlySideInfo
    {
        int side = 0;
        int hand = -1;
        int wrist = -1;
        int forearm = -1;
        Vector handPos = Vector(0.0f, 0.0f, 0.0f);
        Vector anchorPos = Vector(0.0f, 0.0f, 0.0f);
        Vector forearmPos = Vector(0.0f, 0.0f, 0.0f);
        bool oppositeSideValid = false;
        Vector oppositeHandPos = Vector(0.0f, 0.0f, 0.0f);
        Vector oppositeAnchorPos = Vector(0.0f, 0.0f, 0.0f);
        Vector oppositeForearmPos = Vector(0.0f, 0.0f, 0.0f);
        Vector cutRotationDeg = Vector(0.0f, 0.0f, 0.0f);
        bool autoCanonicalCutNormal = false;
        float wristKeepDistance = 0.0f;
        float wristPlaneWorld[4]{};
        bool deterministicWristPlaneLocalValid = false;
        float deterministicWristPlaneLocal[4]{};
    };

    inline bool HooksNativeViewmodelHandsOnlyVectorFinite(const Vector& value);
    inline bool HooksNativeViewmodelHandsOnlyMatrixFinite(const vr_vm_stabilize::Mat3x4& matrix);
    inline bool HooksNativeViewmodelHandsOnlyPlaneFinite(const float plane[4]);
    inline bool HooksNativeViewmodelHandsOnlyBuildPlaneLocalToAnchor(
        const vr_vm_stabilize::Mat3x4& anchor,
        const float worldPlane[4],
        float outLocalPlane[4]);

    inline bool HooksNativeViewmodelHandsOnlyResolveRestOffsetToAncestor(
        void* drawState,
        int boneIndex,
        int stride,
        const std::vector<int>& boneParents,
        int numBones,
        int child,
        int ancestor,
        Vector& outOffset)
    {
        outOffset = Vector(0.0f, 0.0f, 0.0f);
        if (!drawState || child < 0 || child >= numBones || ancestor < 0 || ancestor >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        int current = child;
        for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
        {
            if (current == ancestor)
                return true;

            Vector local{};
            if (!HooksNativeViewmodelHandsOnlyReadBoneRestPos(
                drawState,
                boneIndex,
                stride,
                current,
                local) ||
                !HooksNativeViewmodelHandsOnlyVectorFinite(local))
            {
                return false;
            }

            outOffset += local;
            current = boneParents[static_cast<size_t>(current)];
        }

        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyResolveRestBonePositionInHandLocal(
        void* drawState,
        int boneIndex,
        int stride,
        const std::vector<int>& boneParents,
        int numBones,
        int hand,
        int bone,
        Vector& outPosition)
    {
        outPosition = Vector(0.0f, 0.0f, 0.0f);
        if (bone == hand)
            return true;

        Vector offset{};
        if (HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, hand, bone, numBones) &&
            HooksNativeViewmodelHandsOnlyResolveRestOffsetToAncestor(
                drawState,
                boneIndex,
                stride,
                boneParents,
                numBones,
                hand,
                bone,
                offset))
        {
            outPosition = offset * -1.0f;
            return HooksNativeViewmodelHandsOnlyVectorFinite(outPosition);
        }

        if (HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, bone, hand, numBones) &&
            HooksNativeViewmodelHandsOnlyResolveRestOffsetToAncestor(
                drawState,
                boneIndex,
                stride,
                boneParents,
                numBones,
                bone,
                hand,
                offset))
        {
            outPosition = offset;
            return HooksNativeViewmodelHandsOnlyVectorFinite(outPosition);
        }

        return false;
    }

    inline Vector HooksNativeViewmodelHandsOnlyApplyLocalCutNormalRotation(
        VR* vr,
        const Vector& normal,
        const Vector& cutRotationDeg)
    {
        if (!vr)
            return normal;

        const Vector rotation = cutRotationDeg;
        if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z))
            return normal;

        const Vector axes[3] = {
            Vector(1.0f, 0.0f, 0.0f),
            Vector(0.0f, 0.0f, 1.0f),
            Vector(0.0f, 1.0f, 0.0f),
        };

        Vector out = normal;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float degrees = std::clamp(
                HooksVectorComponent(rotation, axis),
                -89.0f,
                89.0f);
            if (std::fabs(degrees) <= 0.0001f)
                continue;

            out = HooksNativeViewmodelHandsOnlyRotateVectorAroundAxis(out, axes[axis], degrees);
        }

        out = HooksNormalizeVector(out, normal);
        return (out.Length() > 0.0001f) ? out : normal;
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildDeterministicWristPlaneLocal(
        VR* vr,
        void* drawState,
        const std::vector<int>& boneParents,
        int numBones,
        int boneIndex,
        int stride,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        float outPlane[4])
    {
        if (!vr || !drawState || !outPlane || keepSide.side == 0 ||
            numBones <= 0 || numBones > 512 ||
            keepSide.hand < 0 || keepSide.hand >= numBones ||
            keepSide.forearm < 0 || keepSide.forearm >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        Vector forearmLocal{};
        if (!HooksNativeViewmodelHandsOnlyResolveRestBonePositionInHandLocal(
            drawState,
            boneIndex,
            stride,
            boneParents,
            numBones,
            keepSide.hand,
            keepSide.forearm,
            forearmLocal))
        {
            return false;
        }

        Vector normal = forearmLocal * -1.0f;
        Vector wristNormal = normal;
        if (keepSide.wrist >= 0 && keepSide.wrist < numBones)
        {
            Vector wristLocal{};
            if (HooksNativeViewmodelHandsOnlyResolveRestBonePositionInHandLocal(
                drawState,
                boneIndex,
                stride,
                boneParents,
                numBones,
                keepSide.hand,
                keepSide.wrist,
                wristLocal))
            {
                const Vector candidate = wristLocal * -1.0f;
                const float candidateLen = candidate.Length();
                if (std::isfinite(candidateLen) && candidateLen > 0.001f)
                    wristNormal = candidate;
            }
        }

        const float wristLength = normal.Length();
        if (!std::isfinite(wristLength) || wristLength <= 0.001f)
            return false;

        normal = HooksNativeViewmodelHandsOnlyResolveArmBendBaseNormal(
            vr,
            normal,
            wristNormal);
        normal = HooksNativeViewmodelHandsOnlyApplyLocalCutNormalRotation(
            vr,
            normal,
            keepSide.cutRotationDeg);
        normal = HooksNormalizeVector(normal, forearmLocal * -1.0f);
        if (normal.Length() <= 0.0001f)
            return false;

        const float trimDistance =
            HooksNativeViewmodelHandsOnlyResolveSideTrimDistance(vr, wristLength, keepSide.side);
        const float wristKeepDistance =
            HooksNativeViewmodelHandsOnlyResolveWristKeepDistance(vr, wristLength);
        const Vector planePoint = normal * (trimDistance - wristKeepDistance);
        outPlane[0] = normal.x;
        outPlane[1] = normal.y;
        outPlane[2] = normal.z;
        outPlane[3] = -DotProduct(normal, planePoint);
        return HooksNativeViewmodelHandsOnlyNormalizePlane(outPlane) &&
            HooksNativeViewmodelHandsOnlyPlaneFinite(outPlane);
    }

    inline bool HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLock(VR* vr);
    inline bool HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(VR* vr, int side);

    inline bool HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const std::string& lowerModel,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        vr_vm_stabilize::Mat3x4* outTargetAnchor,
        vr_vm_stabilize::Mat3x4* outTargetDelta,
        float outPlane[4],
        const vr_vm_stabilize::Mat3x4* planeAnchor = nullptr);

    inline bool HooksNativeViewmodelHandsOnlyBuildClipPlane(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        void* pCustomBoneToWorld,
        float outPlane[4])
    {
        if (!vr || !drawState || !pCustomBoneToWorld || !outPlane)
            return false;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsArmsOrHands(lowerModel))
            return false;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return false;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones)
            return false;

        const std::vector<const char*> handNeedles = {
            "bip01_r_hand", "_r_hand", ".r.hand", "r.hand",
            "bip01_l_hand", "_l_hand", ".l.hand", "l.hand",
        };
        const std::vector<const char*> wristNeedles = {
            "bip01_r_wrist", "_r_wrist", ".r.wrist", "r.wrist",
            "bip01_l_wrist", "_l_wrist", ".l.wrist", "l.wrist",
        };
        int hand = -1;
        int wrist = -1;
        int forearm = -1;
        auto resolveSideBones = [&](int side) -> bool
            {
                int sideHand = -1;
                int sideForearm = -1;
                if (!HooksNativeViewmodelHandsOnlyFindNamedBone(boneNames, handNeedles, side, sideHand) ||
                    !HooksNativeViewmodelHandsOnlyFindBestForearmBone(
                        boneNames,
                        boneParents,
                        numBones,
                        sideHand,
                        side,
                        sideForearm))
                {
                    return false;
                }

                hand = sideHand;
                forearm = sideForearm;
                return true;
            };
        if (!resolveSideBones(1) && !resolveSideBones(-1))
            return false;

        const int resolvedSide =
            HooksNativeViewmodelHandsOnlyBoneSide(vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(hand)]));
        HooksNativeViewmodelHandsOnlyFindBestWristBone(
            boneNames,
            boneParents,
            numBones,
            hand,
            wristNeedles,
            resolvedSide,
            wrist);

        if (hand < 0 || hand >= numBones || forearm < 0 || forearm >= numBones)
            return false;

        const auto* bones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        vr_vm_stabilize::Mat3x4 handBone{};
        vr_vm_stabilize::Mat3x4 forearmBone{};
        if (!vr_vm_stabilize::SafeRead(bones + hand, handBone) ||
            !vr_vm_stabilize::SafeRead(bones + forearm, forearmBone))
        {
            return false;
        }

        const Vector handPos = vr_vm_stabilize::GetOrigin(handBone);
        const Vector forearmPos = vr_vm_stabilize::GetOrigin(forearmBone);
        Vector anchorPos = handPos;

        Vector normal = anchorPos - forearmPos;
        Vector wristNormal = normal;
        if (wrist >= 0 && wrist < numBones)
        {
            vr_vm_stabilize::Mat3x4 wristBone{};
            if (vr_vm_stabilize::SafeRead(bones + wrist, wristBone))
            {
                const Vector candidate = anchorPos - vr_vm_stabilize::GetOrigin(wristBone);
                const float candidateLen = candidate.Length();
                if (std::isfinite(candidateLen) && candidateLen > 0.001f)
                    wristNormal = candidate;
            }
        }
        float len = normal.Length();
        if (!std::isfinite(len) || len < 0.001f)
        {
            anchorPos = handPos;
            normal = anchorPos - forearmPos;
            wristNormal = normal;
            len = normal.Length();
        }
        if (!std::isfinite(len) || len < 0.001f)
            return false;
        const int handSide =
            HooksNativeViewmodelHandsOnlyBoneSide(vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(hand)]));
        Vector cutRotationDeg{};
        const bool explicitCutRotation =
            HooksNativeViewmodelHandsOnlyTryResolveExplicitCutRotationDeg(
                vr,
                handSide,
                &lowerModel,
                cutRotationDeg);
        if (!explicitCutRotation)
            cutRotationDeg = HooksNativeViewmodelHandsOnlyBaseCutRotationDeg(vr, handSide);

        bool usedAutoCanonicalCut = false;
        normal = HooksNativeViewmodelHandsOnlyResolveArmBendNormalForRig(
            vr,
            handBone,
            normal,
            wristNormal,
            handSide,
            lowerModel,
            cutRotationDeg,
            !explicitCutRotation,
            &usedAutoCanonicalCut);

        const float trimDistance =
            HooksNativeViewmodelHandsOnlyResolveSideTrimDistance(vr, len, handSide);
        anchorPos = handPos + (normal * trimDistance);

        float worldPlane[4]{};
        const float wristKeepDistance = HooksNativeViewmodelHandsOnlyResolveWristKeepDistance(vr, len);
        const Vector planePoint = anchorPos - (normal * wristKeepDistance);
        worldPlane[0] = normal.x;
        worldPlane[1] = normal.y;
        worldPlane[2] = normal.z;
        worldPlane[3] = -DotProduct(normal, planePoint);
        if (!HooksNativeViewmodelHandsOnlyNormalizePlane(worldPlane))
            return false;

        if (HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, handSide))
        {
            HooksNativeViewmodelHandsOnlySideInfo sideInfo{};
            sideInfo.side = handSide;
            sideInfo.hand = hand;
            sideInfo.wrist = wrist;
            sideInfo.forearm = forearm;
            sideInfo.handPos = handPos;
            sideInfo.anchorPos = anchorPos;
            sideInfo.forearmPos = forearmPos;
            sideInfo.cutRotationDeg = cutRotationDeg;
            sideInfo.autoCanonicalCutNormal = usedAutoCanonicalCut;
            sideInfo.wristKeepDistance = wristKeepDistance;
            memcpy(sideInfo.wristPlaneWorld, worldPlane, sizeof(sideInfo.wristPlaneWorld));
            if (usedAutoCanonicalCut)
            {
                sideInfo.deterministicWristPlaneLocalValid =
                    HooksNativeViewmodelHandsOnlyBuildPlaneLocalToAnchor(
                        handBone,
                        worldPlane,
                        sideInfo.deterministicWristPlaneLocal);
            }
            else
            {
                sideInfo.deterministicWristPlaneLocalValid =
                    HooksNativeViewmodelHandsOnlyBuildDeterministicWristPlaneLocal(
                        vr,
                        drawState,
                        boneParents,
                        numBones,
                        boneIndex,
                        stride,
                        sideInfo,
                        sideInfo.deterministicWristPlaneLocal);
            }

            float lockedWorldPlane[4]{};
            if (!HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                vr,
                sideInfo,
                lowerModel,
                bones,
                numBones,
                nullptr,
                nullptr,
                lockedWorldPlane,
                &handBone))
            {
                return false;
            }
            memcpy(worldPlane, lockedWorldPlane, sizeof(worldPlane));
        }

        return HooksNativeViewmodelHandsOnlyWorldPlaneToClipPlane(vr, worldPlane, outPlane);
    }

    static constexpr int kHooksNativeViewmodelHandsOnlyMaxClipPlanes = 6;

    struct HooksNativeViewmodelHandsOnlyClipSet
    {
        float planes[kHooksNativeViewmodelHandsOnlyMaxClipPlanes][4]{};
        int planeCount = 0;
        int side = 0;
        vr_vm_stabilize::Mat3x4* isolatedBones = nullptr;
    };

    inline bool HooksNativeViewmodelHandsOnlyShouldHidePendingFreeze(VR* vr)
    {
        return vr &&
            !vr->IsVrHandsTwoHandedGripPoseActive() &&
            vr->m_NativeViewmodelLeftHandFreezePending &&
            vr->m_NativeViewmodelLeftHandFreezeReady.load(std::memory_order_acquire) == 0u;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameLooksHandOnly(
        const std::string& lowerName)
    {
        if (lowerName.find("forearm") != std::string::npos ||
            lowerName.find("upperarm") != std::string::npos ||
            lowerName.find("elbow") != std::string::npos ||
            lowerName.find("ulna") != std::string::npos ||
            lowerName.find("wrist") != std::string::npos ||
            lowerName.find("sleeve") != std::string::npos ||
            lowerName.find("body") != std::string::npos ||
            lowerName.find("spine") != std::string::npos ||
            lowerName.find("chest") != std::string::npos ||
            lowerName.find("shoulder") != std::string::npos ||
            lowerName.find("clavicle") != std::string::npos)
        {
            return false;
        }

        return lowerName.find("hand") != std::string::npos ||
            lowerName.find("palm") != std::string::npos ||
            lowerName.find("finger") != std::string::npos ||
            lowerName.find("thumb") != std::string::npos ||
            lowerName.find("index") != std::string::npos ||
            lowerName.find("middle") != std::string::npos ||
            lowerName.find("ring") != std::string::npos ||
            lowerName.find("pinky") != std::string::npos ||
            lowerName.find("pinkie") != std::string::npos;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameLooksArmKeep(
        const std::string& lowerName)
    {
        return lowerName.find("wrist") != std::string::npos ||
            lowerName.find("forearm") != std::string::npos ||
            lowerName.find("fore_arm") != std::string::npos ||
            lowerName.find("lowerarm") != std::string::npos ||
            lowerName.find("lower_arm") != std::string::npos ||
            lowerName.find("upperarm") != std::string::npos ||
            lowerName.find("upper_arm") != std::string::npos ||
            lowerName.find("elbow") != std::string::npos ||
            lowerName.find("ulna") != std::string::npos ||
            lowerName.find("sleeve") != std::string::npos ||
            lowerName.find("arm") != std::string::npos;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameLooksWristBlend(
        const std::string& lowerName)
    {
        if (lowerName.find("upperarm") != std::string::npos ||
            lowerName.find("upper_arm") != std::string::npos ||
            lowerName.find("clavicle") != std::string::npos ||
            lowerName.find("shoulder") != std::string::npos ||
            lowerName.find("spine") != std::string::npos ||
            lowerName.find("chest") != std::string::npos ||
            lowerName.find("body") != std::string::npos)
        {
            return false;
        }

        return lowerName.find("wrist") != std::string::npos ||
            lowerName.find("ulna") != std::string::npos ||
            lowerName.find("radius") != std::string::npos ||
            lowerName.find("forearm_driven") != std::string::npos ||
            lowerName.find("fore_arm_driven") != std::string::npos ||
            lowerName.find("driven_forearm") != std::string::npos ||
            lowerName.find("driven_lowerarm") != std::string::npos;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameLooksHandAttachment(
        const std::string& lowerName)
    {
        return lowerName.find("ring") != std::string::npos ||
            lowerName.find("cuff") != std::string::npos ||
            lowerName.find("chain") != std::string::npos ||
            lowerName.find("bracelet") != std::string::npos ||
            lowerName.find("watch") != std::string::npos ||
            lowerName.find("charm") != std::string::npos ||
            lowerName.find("jewel") != std::string::npos ||
            lowerName.find("ornament") != std::string::npos ||
            lowerName.find("skirt") != std::string::npos ||
            lowerName.find("cloth") != std::string::npos;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameLooksFingerOnly(
        const std::string& lowerName)
    {
        return lowerName.find("finger") != std::string::npos ||
            lowerName.find("thumb") != std::string::npos ||
            lowerName.find("index") != std::string::npos ||
            lowerName.find("middle") != std::string::npos ||
            lowerName.find("ring") != std::string::npos ||
            lowerName.find("pinky") != std::string::npos ||
            lowerName.find("pinkie") != std::string::npos;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameLooksDiscardableHelper(
        const std::string& lowerName)
    {
        return lowerName.find("helper") != std::string::npos ||
            lowerName.find("hlp_") != std::string::npos ||
            lowerName.find(".hlp") != std::string::npos ||
            lowerName.find("_fix") != std::string::npos ||
            lowerName.find("thumb_fix") != std::string::npos;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneOrAncestorLooksFingerOnly(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        int bone)
    {
        for (int current = bone, guard = 0;
            current >= 0 && current < numBones && guard < numBones;
            ++guard)
        {
            if (current < static_cast<int>(boneNames.size()))
            {
                const std::string lowerName =
                    vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(current)]);
                if (HooksNativeViewmodelHandsOnlyBoneNameLooksFingerOnly(lowerName))
                    return true;
            }

            if (current >= static_cast<int>(boneParents.size()))
                break;
            current = boneParents[static_cast<size_t>(current)];
        }
        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyVectorFinite(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneRegionDistanceSq(
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int bone,
        const Vector& handPos,
        const Vector& anchorPos,
        const Vector& forearmPos,
        float& outDistanceSq,
        float& outRadiusSq)
    {
        outDistanceSq = FLT_MAX;
        outRadiusSq = 0.0f;
        if (!sourceBones || bone < 0 ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(handPos) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(forearmPos))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 boneWorld{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
        {
            return false;
        }

        const Vector origin = vr_vm_stabilize::GetOrigin(boneWorld);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(origin))
            return false;

        const Vector segment = handPos - forearmPos;
        const float segmentLenSq = DotProduct(segment, segment);
        if (!std::isfinite(segmentLenSq) || segmentLenSq < 0.001f)
            return false;

        const float segmentLen = std::sqrt(segmentLenSq);
        const float radius = std::clamp(segmentLen * 1.35f, 10.0f, 56.0f);
        const float radiusSq = radius * radius;

        const float t = std::clamp(
            DotProduct(origin - forearmPos, segment) / segmentLenSq,
            0.0f,
            1.0f);
        const Vector closest = forearmPos + segment * t;
        outDistanceSq = (origin - closest).LengthSqr();

        if (HooksNativeViewmodelHandsOnlyVectorFinite(anchorPos))
            outDistanceSq = std::min(outDistanceSq, (origin - anchorPos).LengthSqr());

        outDistanceSq = std::min(outDistanceSq, (origin - handPos).LengthSqr());
        outRadiusSq = radiusSq;
        return std::isfinite(outDistanceSq) && std::isfinite(outRadiusSq);
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNearSideHandRegion(
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int bone,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide)
    {
        float distanceSq = 0.0f;
        float radiusSq = 0.0f;
        return HooksNativeViewmodelHandsOnlyBoneRegionDistanceSq(
            sourceBones,
            bone,
            keepSide.handPos,
            keepSide.anchorPos,
            keepSide.forearmPos,
            distanceSq,
            radiusSq) &&
            distanceSq <= radiusSq;
    }

    inline bool HooksNativeViewmodelHandsOnlyUnnamedBoneBelongsToSide(
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int bone,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide)
    {
        float sideDistanceSq = 0.0f;
        float sideRadiusSq = 0.0f;
        if (!HooksNativeViewmodelHandsOnlyBoneRegionDistanceSq(
            sourceBones,
            bone,
            keepSide.handPos,
            keepSide.anchorPos,
            keepSide.forearmPos,
            sideDistanceSq,
            sideRadiusSq) ||
            sideDistanceSq > sideRadiusSq)
        {
            return false;
        }

        if (keepSide.oppositeSideValid)
        {
            float oppositeDistanceSq = 0.0f;
            float oppositeRadiusSq = 0.0f;
            if (HooksNativeViewmodelHandsOnlyBoneRegionDistanceSq(
                sourceBones,
                bone,
                keepSide.oppositeHandPos,
                keepSide.oppositeAnchorPos,
                keepSide.oppositeForearmPos,
                oppositeDistanceSq,
                oppositeRadiusSq) &&
                oppositeDistanceSq + 1.0f < sideDistanceSq)
            {
                return false;
            }
        }

        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyShouldKeepBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        float wristKeepDistance,
        int bone)
    {
        if (bone < 0 || bone >= numBones)
            return false;

        const bool haveName = bone < static_cast<int>(boneNames.size());
        const std::string lowerName = haveName
            ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)])
            : std::string();

        if (!lowerName.empty() &&
            HooksNativeViewmodelHandsOnlyBoneNameLooksDiscardableHelper(lowerName) &&
            !HooksNativeViewmodelHandsOnlyBoneNearSideHandRegion(sourceBones, bone, keepSide))
        {
            return false;
        }

        if (HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, bone, keepSide.hand, numBones))
        {
            return true;
        }

        if (lowerName.empty())
            return false;

        const int boneSide = HooksNativeViewmodelHandsOnlyBoneSide(lowerName);
        if (boneSide != 0 && boneSide != keepSide.side)
            return false;

        (void)wristKeepDistance;
        if (bone == keepSide.wrist ||
            HooksNativeViewmodelHandsOnlyBoneNameLooksWristBlend(lowerName) ||
            HooksNativeViewmodelHandsOnlyBoneNameLooksHandOnly(lowerName) ||
            HooksNativeViewmodelHandsOnlyBoneNameLooksHandAttachment(lowerName))
        {
            return HooksNativeViewmodelHandsOnlyBoneNearSideHandRegion(sourceBones, bone, keepSide);
        }

        return false;
    }

    inline std::string HooksNativeViewmodelHandsOnlyDebugBoneName(
        const std::vector<std::string>& boneNames,
        int bone)
    {
        if (bone >= 0 &&
            bone < static_cast<int>(boneNames.size()) &&
            !boneNames[static_cast<size_t>(bone)].empty())
        {
            return boneNames[static_cast<size_t>(bone)];
        }

        return "<unnamed>";
    }

    inline bool HooksNativeViewmodelHandsOnlyDebugBoneLooksInteresting(
        const std::vector<std::string>& boneNames,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        int bone)
    {
        const std::string name = HooksNativeViewmodelHandsOnlyDebugBoneName(boneNames, bone);
        const std::string lowerName = vr_vm_stabilize::ToLowerAscii(name);
        if (lowerName == "<unnamed>" ||
            lowerName.find("hand") != std::string::npos ||
            lowerName.find("finger") != std::string::npos ||
            lowerName.find("thumb") != std::string::npos ||
            lowerName.find("wrist") != std::string::npos ||
            lowerName.find("forearm") != std::string::npos ||
            lowerName.find("lowerarm") != std::string::npos ||
            lowerName.find("upperarm") != std::string::npos ||
            lowerName.find("ulna") != std::string::npos ||
            lowerName.find("helper") != std::string::npos ||
            lowerName.find("hlp") != std::string::npos ||
            lowerName.find("sleeve") != std::string::npos ||
            lowerName.find("cloth") != std::string::npos ||
            lowerName.find("cuff") != std::string::npos)
        {
            return true;
        }

        return HooksNativeViewmodelHandsOnlyBoneNearSideHandRegion(sourceBones, bone, keepSide);
    }

    inline std::string HooksNativeViewmodelHandsOnlyFormatBoneListForLog(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const std::vector<uint8_t>& keepMask,
        int numBones,
        bool wantKept,
        bool interestingOnly,
        int maxItems)
    {
        std::string out;
        int matched = 0;
        int emitted = 0;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (bone >= static_cast<int>(keepMask.size()))
                break;

            const bool kept = keepMask[static_cast<size_t>(bone)] != 0u;
            if (kept != wantKept)
                continue;

            if (interestingOnly &&
                !HooksNativeViewmodelHandsOnlyDebugBoneLooksInteresting(
                    boneNames,
                    sourceBones,
                    keepSide,
                    bone))
            {
                continue;
            }

            ++matched;
            if (emitted >= maxItems)
                continue;

            const int parent = (bone >= 0 && bone < static_cast<int>(boneParents.size()))
                ? boneParents[static_cast<size_t>(bone)]
                : -1;
            if (!out.empty())
                out += "; ";
            out += std::to_string(bone);
            out += ":p";
            out += std::to_string(parent);
            out += ":";
            out += HooksNativeViewmodelHandsOnlyDebugBoneName(boneNames, bone);
            ++emitted;
        }

        if (matched > emitted)
        {
            if (!out.empty())
                out += "; ";
            out += "... +";
            out += std::to_string(matched - emitted);
            out += " more";
        }

        if (out.empty())
            out = "<none>";
        return out;
    }

    inline bool HooksNativeViewmodelHandsOnlyThrottleIsolationDebugLog(
        const std::string& key,
        float maxHz = 1.0f)
    {
        if (key.empty() || maxHz <= 0.0f)
            return false;

        static std::mutex s_isolationLogMutex;
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastIsolationLog;
        const auto now = std::chrono::steady_clock::now();
        const auto minInterval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(1.0f / maxHz));

        std::lock_guard<std::mutex> lock(s_isolationLogMutex);
        auto& last = s_lastIsolationLog[key];
        if (last.time_since_epoch().count() != 0 && now - last < minInterval)
            return true;

        last = now;
        return false;
    }

    inline void HooksNativeViewmodelHandsOnlyLogIsolationDebug(
        VR* vr,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const std::vector<uint8_t>& keepMask,
        int keptBones,
        const float workingWristPlaneWorld[4],
        const Vector& planePoint,
        const Vector& normal,
        const Vector& hiddenOriginPreTransform,
        bool useTargetDelta,
        bool haveTargetAnchor,
        const vr_vm_stabilize::Mat3x4& targetDelta,
        const vr_vm_stabilize::Mat3x4& targetAnchor,
        bool useFixedFreezePlaneLock,
        bool useCanonicalFreezeLock,
        bool appliedFrozenPose,
        const vr_vm_stabilize::Mat3x4* finalBones)
    {
        if (!vr || !vr->m_VrHandsDebugLog || keepSide.side == 0 || numBones <= 0)
            return;

        const char* sideName = (keepSide.side < 0) ? "L" : "R";
        const std::string key =
            lowerModel + "|" + sideName + "|" + std::to_string(numBones);
        if (HooksNativeViewmodelHandsOnlyThrottleIsolationDebugLog(key, 1.0f))
            return;

        Vector hiddenAfterTarget = hiddenOriginPreTransform;
        if (useTargetDelta && HooksNativeViewmodelHandsOnlyMatrixFinite(targetDelta))
            hiddenAfterTarget = HooksTransformPoint(targetDelta, hiddenOriginPreTransform);

        Vector targetAnchorOrigin = keepSide.handPos;
        if (haveTargetAnchor && HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor))
            targetAnchorOrigin = vr_vm_stabilize::GetOrigin(targetAnchor);

        Vector finalHand = targetAnchorOrigin;
        if (finalBones && keepSide.hand >= 0 && keepSide.hand < numBones)
        {
            vr_vm_stabilize::Mat3x4 finalHandBone{};
            if (vr_vm_stabilize::SafeRead(finalBones + keepSide.hand, finalHandBone) &&
                HooksNativeViewmodelHandsOnlyMatrixFinite(finalHandBone))
            {
                finalHand = vr_vm_stabilize::GetOrigin(finalHandBone);
            }
        }

        int sampleHiddenBone = -1;
        Vector sampleHiddenOrigin = hiddenAfterTarget;
        if (finalBones)
        {
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (bone >= static_cast<int>(keepMask.size()) ||
                    keepMask[static_cast<size_t>(bone)] != 0u)
                {
                    continue;
                }

                vr_vm_stabilize::Mat3x4 hiddenBone{};
                if (vr_vm_stabilize::SafeRead(finalBones + bone, hiddenBone) &&
                    HooksNativeViewmodelHandsOnlyMatrixFinite(hiddenBone))
                {
                    sampleHiddenBone = bone;
                    sampleHiddenOrigin = vr_vm_stabilize::GetOrigin(hiddenBone);
                    break;
                }
            }
        }

        const float hiddenPreDist = (hiddenOriginPreTransform - targetAnchorOrigin).Length();
        const float hiddenAfterTargetDist = (hiddenAfterTarget - targetAnchorOrigin).Length();
        const float sampleHiddenDist = (sampleHiddenOrigin - finalHand).Length();
        const int hiddenBones = std::max(0, numBones - keptBones);

        Game::logMsg(
            "[VR][NativeHandsOnly][Iso] model=\"%s\" side=%s bones=%d kept=%d hidden=%d hand=%d wrist=%d forearm=%d useTargetDelta=%d haveTargetAnchor=%d fixed=%d canonical=%d frozen=%d plane=(%.3f %.3f %.3f %.3f) planePoint=(%.1f %.1f %.1f) hiddenPre=(%.1f %.1f %.1f) hiddenAfterTarget=(%.1f %.1f %.1f) targetHand=(%.1f %.1f %.1f) finalHand=(%.1f %.1f %.1f) hiddenDistPre=%.1f hiddenDistAfterTarget=%.1f sampleHidden=%d sampleHiddenDist=%.1f",
            lowerModel.c_str(),
            sideName,
            numBones,
            keptBones,
            hiddenBones,
            keepSide.hand,
            keepSide.wrist,
            keepSide.forearm,
            useTargetDelta ? 1 : 0,
            haveTargetAnchor ? 1 : 0,
            useFixedFreezePlaneLock ? 1 : 0,
            useCanonicalFreezeLock ? 1 : 0,
            appliedFrozenPose ? 1 : 0,
            workingWristPlaneWorld ? workingWristPlaneWorld[0] : 0.0f,
            workingWristPlaneWorld ? workingWristPlaneWorld[1] : 0.0f,
            workingWristPlaneWorld ? workingWristPlaneWorld[2] : 0.0f,
            workingWristPlaneWorld ? workingWristPlaneWorld[3] : 0.0f,
            planePoint.x,
            planePoint.y,
            planePoint.z,
            hiddenOriginPreTransform.x,
            hiddenOriginPreTransform.y,
            hiddenOriginPreTransform.z,
            hiddenAfterTarget.x,
            hiddenAfterTarget.y,
            hiddenAfterTarget.z,
            targetAnchorOrigin.x,
            targetAnchorOrigin.y,
            targetAnchorOrigin.z,
            finalHand.x,
            finalHand.y,
            finalHand.z,
            hiddenPreDist,
            hiddenAfterTargetDist,
            sampleHiddenBone,
            sampleHiddenDist);

        const std::string keptList =
            HooksNativeViewmodelHandsOnlyFormatBoneListForLog(
                boneNames,
                boneParents,
                sourceBones,
                keepSide,
                keepMask,
                numBones,
                true,
                false,
                48);
        const std::string hiddenList =
            HooksNativeViewmodelHandsOnlyFormatBoneListForLog(
                boneNames,
                boneParents,
                sourceBones,
                keepSide,
                keepMask,
                numBones,
                false,
                true,
                48);
        Game::logMsg(
            "[VR][NativeHandsOnly][IsoBones] model=\"%s\" side=%s kept=%s",
            lowerModel.c_str(),
            sideName,
            keptList.c_str());
        Game::logMsg(
            "[VR][NativeHandsOnly][IsoBones] model=\"%s\" side=%s hiddenInteresting=%s",
            lowerModel.c_str(),
            sideName,
            hiddenList.c_str());
    }

    inline vr_vm_stabilize::Mat3x4 HooksNativeViewmodelHandsOnlyCollapsedBoneAt(
        const Vector& origin)
    {
        vr_vm_stabilize::Mat3x4 out{};
        out.m[0][3] = origin.x;
        out.m[1][3] = origin.y;
        out.m[2][3] = origin.z;
        return out;
    }

    inline bool HooksNativeViewmodelHandsOnlyMatrixFinite(const vr_vm_stabilize::Mat3x4& matrix)
    {
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                if (!std::isfinite(matrix.m[row][col]))
                    return false;
            }
        }
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyFrozenAnchorLocalMatrixPlausible(
        const vr_vm_stabilize::Mat3x4& matrix)
    {
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(matrix))
            return false;

        // Frozen matrices are relative to the hand anchor, so every selected arm,
        // wrist, helper, and finger bone must remain close to that hand. Some custom
        // models expose helper bones with finite but world-sized transforms; replaying
        // those transforms after a name-based layout remap stretches weighted vertices
        // into giant spikes. Collapsed arm-cut matrices intentionally have a zero basis,
        // so only reject an excessive basis rather than requiring an invertible matrix.
        constexpr float kMaxAnchorLocalOffset = 256.0f;
        constexpr float kMaxBasisComponent = 4.0f;
        const Vector origin = vr_vm_stabilize::GetOrigin(matrix);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(origin) ||
            origin.LengthSqr() > (kMaxAnchorLocalOffset * kMaxAnchorLocalOffset))
        {
            return false;
        }

        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                if (std::fabs(matrix.m[row][col]) > kMaxBasisComponent)
                    return false;
            }
        }
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyPlaneFinite(const float plane[4])
    {
        if (!plane)
            return false;

        return std::isfinite(plane[0]) &&
            std::isfinite(plane[1]) &&
            std::isfinite(plane[2]) &&
            std::isfinite(plane[3]);
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildWorldPlaneFromPointNormal(
        const Vector& point,
        const Vector& normal,
        float outPlane[4])
    {
        if (!outPlane ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(point) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(normal))
        {
            return false;
        }

        Vector n = HooksNormalizeVector(normal, Vector(0.0f, 0.0f, 0.0f));
        if (n.Length() <= 0.0001f)
            return false;

        outPlane[0] = n.x;
        outPlane[1] = n.y;
        outPlane[2] = n.z;
        outPlane[3] = -DotProduct(n, point);
        return HooksNativeViewmodelHandsOnlyNormalizePlane(outPlane);
    }

    inline bool HooksNativeViewmodelHandsOnlyAppendWorldClipPlane(
        VR* vr,
        const float worldPlane[4],
        HooksNativeViewmodelHandsOnlyClipSet& set)
    {
        if (!vr || !worldPlane ||
            set.planeCount < 0 ||
            set.planeCount >= kHooksNativeViewmodelHandsOnlyMaxClipPlanes)
        {
            return false;
        }

        float normalizedPlane[4] = {
            worldPlane[0],
            worldPlane[1],
            worldPlane[2],
            worldPlane[3],
        };
        if (!HooksNativeViewmodelHandsOnlyNormalizePlane(normalizedPlane))
            return false;

        if (!HooksNativeViewmodelHandsOnlyWorldPlaneToClipPlane(
            vr,
            normalizedPlane,
            set.planes[set.planeCount]))
        {
            return false;
        }

        ++set.planeCount;
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyResolveRegionClipAxes(
        VR* vr,
        const Vector& normal,
        Vector& outAxisA,
        Vector& outAxisB)
    {
        outAxisA = Vector(0.0f, 0.0f, 0.0f);
        outAxisB = Vector(0.0f, 0.0f, 0.0f);

        const Vector n = HooksNormalizeVector(normal, Vector(0.0f, 0.0f, 0.0f));
        if (n.Length() <= 0.0001f)
            return false;

        Vector viewOrigin{};
        Vector viewForward{};
        Vector viewRight{};
        Vector viewUp{};
        Vector axisA = Vector(0.0f, 0.0f, 1.0f);
        if (HooksNativeViewmodelHandsOnlyResolveViewFrame(vr, viewOrigin, viewForward, viewRight, viewUp))
        {
            axisA = viewUp - n * DotProduct(viewUp, n);
            if (axisA.Length() <= 0.0001f)
                axisA = viewRight - n * DotProduct(viewRight, n);
        }
        else
        {
            axisA = axisA - n * DotProduct(axisA, n);
        }

        if (axisA.Length() <= 0.0001f)
            axisA = CrossProduct(n, Vector(0.0f, 0.0f, 1.0f));
        if (axisA.Length() <= 0.0001f)
            axisA = CrossProduct(n, Vector(0.0f, 1.0f, 0.0f));
        axisA = HooksNormalizeVector(axisA, Vector(0.0f, 0.0f, 0.0f));
        if (axisA.Length() <= 0.0001f)
            return false;

        Vector axisB = HooksNormalizeVector(CrossProduct(n, axisA), Vector(0.0f, 0.0f, 0.0f));
        if (axisB.Length() <= 0.0001f)
            return false;
        axisA = HooksNormalizeVector(CrossProduct(axisB, n), axisA);

        outAxisA = axisA;
        outAxisB = axisB;
        return true;
    }

    inline void HooksNativeViewmodelHandsOnlyAppendRegionClipPlanes(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        HooksNativeViewmodelHandsOnlyClipSet& set)
    {
        if (!vr || set.planeCount <= 0 ||
            set.planeCount + 4 > kHooksNativeViewmodelHandsOnlyMaxClipPlanes ||
            !HooksNativeViewmodelHandsOnlyPlaneFinite(keepSide.wristPlaneWorld))
        {
            return;
        }

        Vector normal(
            keepSide.wristPlaneWorld[0],
            keepSide.wristPlaneWorld[1],
            keepSide.wristPlaneWorld[2]);
        normal = HooksNormalizeVector(normal, keepSide.handPos - keepSide.forearmPos);
        if (normal.Length() <= 0.0001f)
            return;

        Vector axisA{};
        Vector axisB{};
        if (!HooksNativeViewmodelHandsOnlyResolveRegionClipAxes(vr, normal, axisA, axisB))
            return;

        const float wristLen = (keepSide.handPos - keepSide.forearmPos).Length();
        if (!std::isfinite(wristLen) || wristLen <= 0.001f)
            return;

        const float radius = std::clamp(
            wristLen * 1.05f + std::max(keepSide.wristKeepDistance, 0.0f) * 0.75f + 4.0f,
            12.0f,
            28.0f);
        const Vector center = keepSide.handPos;

        auto appendSlab = [&](const Vector& axis) -> bool
            {
                float plane[4]{};
                if (!HooksNativeViewmodelHandsOnlyBuildWorldPlaneFromPointNormal(
                    center - axis * radius,
                    axis,
                    plane))
                {
                    return false;
                }
                if (!HooksNativeViewmodelHandsOnlyAppendWorldClipPlane(vr, plane, set))
                    return false;

                if (!HooksNativeViewmodelHandsOnlyBuildWorldPlaneFromPointNormal(
                    center + axis * radius,
                    axis * -1.0f,
                    plane))
                {
                    return false;
                }
                return HooksNativeViewmodelHandsOnlyAppendWorldClipPlane(vr, plane, set);
            };

        appendSlab(axisA);
        appendSlab(axisB);
    }

    inline HooksNativeViewmodelHandsOnlySideInfo HooksNativeViewmodelHandsOnlyBuildFinalRegionSideInfo(
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* isolatedBones,
        int numBones,
        const float wristPlaneWorld[4])
    {
        HooksNativeViewmodelHandsOnlySideInfo outInfo = keepSide;
        if (wristPlaneWorld && HooksNativeViewmodelHandsOnlyPlaneFinite(wristPlaneWorld))
            memcpy(outInfo.wristPlaneWorld, wristPlaneWorld, sizeof(outInfo.wristPlaneWorld));

        auto readFinalBoneOrigin = [&](int bone, Vector& outOrigin) -> bool
            {
                if (!isolatedBones || bone < 0 || bone >= numBones)
                    return false;

                vr_vm_stabilize::Mat3x4 boneWorld{};
                if (!vr_vm_stabilize::SafeRead(isolatedBones + bone, boneWorld) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(boneWorld))
                {
                    return false;
                }

                const Vector origin = vr_vm_stabilize::GetOrigin(boneWorld);
                if (!HooksNativeViewmodelHandsOnlyVectorFinite(origin))
                    return false;

                outOrigin = origin;
                return true;
            };

        Vector finalHand{};
        if (readFinalBoneOrigin(keepSide.hand, finalHand))
        {
            outInfo.handPos = finalHand;
            outInfo.anchorPos = finalHand;
        }

        Vector finalForearm{};
        if (readFinalBoneOrigin(keepSide.forearm, finalForearm) ||
            readFinalBoneOrigin(keepSide.wrist, finalForearm))
        {
            outInfo.forearmPos = finalForearm;
        }

        return outInfo;
    }

    inline bool HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
        const vr_vm_stabilize::Mat3x4& delta,
        const float sourcePlane[4],
        float outPlane[4])
    {
        if (!sourcePlane || !outPlane || !HooksNativeViewmodelHandsOnlyMatrixFinite(delta))
            return false;

        float plane[4] = {
            sourcePlane[0],
            sourcePlane[1],
            sourcePlane[2],
            sourcePlane[3],
        };
        if (!HooksNativeViewmodelHandsOnlyNormalizePlane(plane))
            return false;

        Vector normal(plane[0], plane[1], plane[2]);
        const float normalLen = normal.Length();
        if (!std::isfinite(normalLen) || normalLen < 0.001f)
            return false;
        normal *= (1.0f / normalLen);

        const Vector point = normal * (-plane[3] / normalLen);
        Vector movedPoint = HooksTransformPoint(delta, point);
        Vector movedNormal = HooksTransformVector(delta, normal);
        const float movedNormalLen = movedNormal.Length();
        if (!std::isfinite(movedNormalLen) || movedNormalLen < 0.001f)
            return false;
        movedNormal *= (1.0f / movedNormalLen);

        outPlane[0] = movedNormal.x;
        outPlane[1] = movedNormal.y;
        outPlane[2] = movedNormal.z;
        outPlane[3] = -DotProduct(movedNormal, movedPoint);
        return HooksNativeViewmodelHandsOnlyNormalizePlane(outPlane) &&
            HooksNativeViewmodelHandsOnlyPlaneFinite(outPlane);
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildPlaneLocalToAnchor(
        const vr_vm_stabilize::Mat3x4& anchor,
        const float worldPlane[4],
        float outLocalPlane[4])
    {
        if (!worldPlane || !outLocalPlane || !HooksNativeViewmodelHandsOnlyMatrixFinite(anchor))
            return false;

        vr_vm_stabilize::Mat3x4 inverseAnchor{};
        vr_vm_stabilize::InvertTR(anchor, inverseAnchor);
        return HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
            inverseAnchor,
            worldPlane,
            outLocalPlane);
    }

    inline bool HooksNativeViewmodelHandsOnlyFindBoneByLowerSuffix(
        const std::vector<std::string>& boneNames,
        const char* lowerSuffix,
        int& outBone);

    inline bool HooksNativeViewmodelHandsOnlyReanchorPlaneToTargetHand(
        const vr_vm_stabilize::Mat3x4& targetAnchor,
        const Vector& sourceHandPos,
        float plane[4]);

    inline int HooksNativeViewmodelHandsOnlyBuildFingerDriveMask(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        int side,
        const std::array<float, 5>* curls,
        float strength,
        float direction,
        std::vector<uint8_t>& outDriveMask,
        std::vector<uint8_t>* outHasAngle,
        std::vector<float>* outAngleByBone)
    {
        const size_t maskSize = static_cast<size_t>(numBones > 0 ? numBones : 0);
        outDriveMask.assign(maskSize, 0u);
        if (outHasAngle)
            outHasAngle->assign(maskSize, 0u);
        if (outAngleByBone)
            outAngleByBone->assign(maskSize, 0.0f);
        if ((side != -1 && side != 1) ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return 0;
        }

        static const char* kLeftFingerBones[5][3] =
        {
            { "bip01_l_finger0", "bip01_l_finger01", "bip01_l_finger02" },
            { "bip01_l_finger1", "bip01_l_finger11", "bip01_l_finger12" },
            { "bip01_l_finger2", "bip01_l_finger21", "bip01_l_finger22" },
            { "bip01_l_finger3", "bip01_l_finger31", "bip01_l_finger32" },
            { "bip01_l_finger4", "bip01_l_finger41", "bip01_l_finger42" },
        };
        static const char* kRightFingerBones[5][3] =
        {
            { "bip01_r_finger0", "bip01_r_finger01", "bip01_r_finger02" },
            { "bip01_r_finger1", "bip01_r_finger11", "bip01_r_finger12" },
            { "bip01_r_finger2", "bip01_r_finger21", "bip01_r_finger22" },
            { "bip01_r_finger3", "bip01_r_finger31", "bip01_r_finger32" },
            { "bip01_r_finger4", "bip01_r_finger41", "bip01_r_finger42" },
        };
        static const float kMaxCurlRadians[5][3] =
        {
            { 0.75f, 0.90f, 0.65f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
        };

        std::vector<uint8_t> drivenRoot(static_cast<size_t>(numBones), 0u);
        int mappedSegments = 0;
        for (int finger = 0; finger < 5; ++finger)
        {
            const float curl = curls ? (*curls)[static_cast<size_t>(finger)] : 0.0f;
            for (int segment = 0; segment < 3; ++segment)
            {
                int bone = -1;
                if (!HooksNativeViewmodelHandsOnlyFindBoneByLowerSuffix(
                    boneNames,
                    (side < 0)
                    ? kLeftFingerBones[finger][segment]
                    : kRightFingerBones[finger][segment],
                    bone) ||
                    bone < 0 ||
                    bone >= numBones)
                {
                    continue;
                }

                drivenRoot[static_cast<size_t>(bone)] = 1u;
                outDriveMask[static_cast<size_t>(bone)] = 1u;
                if (outHasAngle && outAngleByBone)
                {
                    const bool thumbRoot = (finger == 0 && segment == 0);
                    if (!thumbRoot)
                    {
                        (*outHasAngle)[static_cast<size_t>(bone)] = 1u;
                        (*outAngleByBone)[static_cast<size_t>(bone)] =
                            curl * kMaxCurlRadians[finger][segment] * strength * direction;
                    }
                }
                ++mappedSegments;
            }
        }

        for (int bone = 0; bone < numBones; ++bone)
        {
            int current = bone;
            for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
            {
                if (drivenRoot[static_cast<size_t>(current)])
                {
                    outDriveMask[static_cast<size_t>(bone)] = 1u;
                    break;
                }
                current = boneParents[static_cast<size_t>(current)];
            }
        }

        return mappedSegments;
    }

    inline int HooksNativeViewmodelHandsOnlyBuildLeftFingerDriveMask(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const std::array<float, 5>* curls,
        float strength,
        float direction,
        std::vector<uint8_t>& outDriveMask,
        std::vector<uint8_t>* outHasAngle,
        std::vector<float>* outAngleByBone)
    {
        return HooksNativeViewmodelHandsOnlyBuildFingerDriveMask(
            boneNames,
            boneParents,
            numBones,
            -1,
            curls,
            strength,
            direction,
            outDriveMask,
            outHasAngle,
            outAngleByBone);
    }

    inline int HooksNativeViewmodelHandsOnlyBuildThumbRootMask(
        const std::vector<std::string>& boneNames,
        int numBones,
        int side,
        std::vector<uint8_t>& outMask)
    {
        outMask.assign(static_cast<size_t>(numBones > 0 ? numBones : 0), 0u);
        if ((side != -1 && side != 1) ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones)
            return 0;

        int thumbRoot = -1;
        if (!HooksNativeViewmodelHandsOnlyFindBoneByLowerSuffix(
            boneNames,
            (side < 0) ? "bip01_l_finger0" : "bip01_r_finger0",
            thumbRoot) ||
            thumbRoot < 0 ||
            thumbRoot >= numBones)
        {
            return 0;
        }

        outMask[static_cast<size_t>(thumbRoot)] = 1u;
        return 1;
    }

    inline int HooksNativeViewmodelHandsOnlyBuildLeftThumbRootMask(
        const std::vector<std::string>& boneNames,
        int numBones,
        std::vector<uint8_t>& outMask)
    {
        return HooksNativeViewmodelHandsOnlyBuildThumbRootMask(
            boneNames,
            numBones,
            -1,
            outMask);
    }

    inline int HooksNativeViewmodelHandsOnlyNeutralizeFrozenLeftFingerRotations(
        const std::vector<int>& boneParents,
        int numBones,
        const std::vector<uint8_t>& driveMask,
        const std::vector<uint8_t>& preserveLocalRotationMask,
        const std::vector<uint8_t>& freezeMask,
        std::vector<vr_vm_stabilize::Mat3x4>& inOutAnchorLocalBones)
    {
        if (numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneParents.size()) < numBones ||
            static_cast<int>(driveMask.size()) < numBones ||
            static_cast<int>(freezeMask.size()) < numBones ||
            static_cast<int>(inOutAnchorLocalBones.size()) < numBones)
        {
            return 0;
        }

        const std::vector<vr_vm_stabilize::Mat3x4> originalAnchorLocalBones = inOutAnchorLocalBones;
        std::vector<uint8_t> resolved(static_cast<size_t>(numBones), 0u);
        int applied = 0;
        for (int pass = 0; pass < numBones && applied < numBones; ++pass)
        {
            bool progressed = false;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!driveMask[static_cast<size_t>(bone)] ||
                    !freezeMask[static_cast<size_t>(bone)] ||
                    resolved[static_cast<size_t>(bone)])
                {
                    continue;
                }

                const int parent = boneParents[static_cast<size_t>(bone)];
                if (parent < 0 || parent >= numBones ||
                    !freezeMask[static_cast<size_t>(parent)])
                {
                    continue;
                }
                if (driveMask[static_cast<size_t>(parent)] &&
                    !resolved[static_cast<size_t>(parent)])
                {
                    continue;
                }
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(originalAnchorLocalBones[static_cast<size_t>(parent)]) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(originalAnchorLocalBones[static_cast<size_t>(bone)]) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(inOutAnchorLocalBones[static_cast<size_t>(parent)]))
                {
                    continue;
                }

                vr_vm_stabilize::Mat3x4 inverseOriginalParent{};
                vr_vm_stabilize::InvertTR(originalAnchorLocalBones[static_cast<size_t>(parent)], inverseOriginalParent);
                vr_vm_stabilize::Mat3x4 originalParentLocal{};
                vr_vm_stabilize::Mul(
                    inverseOriginalParent,
                    originalAnchorLocalBones[static_cast<size_t>(bone)],
                    originalParentLocal);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(originalParentLocal))
                    continue;

                const Vector offset = vr_vm_stabilize::GetOrigin(originalParentLocal);
                if (!HooksNativeViewmodelHandsOnlyVectorFinite(offset))
                    continue;

                vr_vm_stabilize::Mat3x4 selectedParentLocal = originalParentLocal;
                const bool preserveLocalRotation =
                    static_cast<int>(preserveLocalRotationMask.size()) == numBones &&
                    preserveLocalRotationMask[static_cast<size_t>(bone)] != 0u;
                if (!preserveLocalRotation)
                {
                    selectedParentLocal = vr_vm_stabilize::Mat3x4{};
                    selectedParentLocal.m[0][0] = 1.0f;
                    selectedParentLocal.m[1][1] = 1.0f;
                    selectedParentLocal.m[2][2] = 1.0f;
                    selectedParentLocal.m[0][3] = offset.x;
                    selectedParentLocal.m[1][3] = offset.y;
                    selectedParentLocal.m[2][3] = offset.z;
                }

                vr_vm_stabilize::Mat3x4 neutralAnchorLocal{};
                vr_vm_stabilize::Mul(
                    inOutAnchorLocalBones[static_cast<size_t>(parent)],
                    selectedParentLocal,
                    neutralAnchorLocal);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(neutralAnchorLocal))
                    continue;

                inOutAnchorLocalBones[static_cast<size_t>(bone)] = neutralAnchorLocal;
                resolved[static_cast<size_t>(bone)] = 1u;
                ++applied;
                progressed = true;
            }

            if (!progressed)
                break;
        }

        return applied;
    }

    inline int HooksNativeViewmodelHandsOnlyNeutralizeFrozenSideArmPose(
        VR* vr,
        void* drawState,
        int boneIndex,
        int stride,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const std::vector<uint8_t>& freezeMask,
        std::vector<vr_vm_stabilize::Mat3x4>& inOutAnchorLocalBones,
        float outWristPlaneLocal[4],
        bool& outWristPlaneValid)
    {
        outWristPlaneValid = false;
        if (!vr || !drawState || !outWristPlaneLocal ||
            numBones <= 0 || numBones > 512 ||
            keepSide.hand < 0 || keepSide.hand >= numBones ||
            keepSide.forearm < 0 || keepSide.forearm >= numBones ||
            static_cast<int>(boneParents.size()) < numBones ||
            static_cast<int>(freezeMask.size()) < numBones ||
            static_cast<int>(inOutAnchorLocalBones.size()) < numBones ||
            !freezeMask[static_cast<size_t>(keepSide.hand)] ||
            !freezeMask[static_cast<size_t>(keepSide.forearm)])
        {
            return 0;
        }

        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(inOutAnchorLocalBones[static_cast<size_t>(keepSide.hand)]))
            return 0;

        int neutralized = 0;
        int child = keepSide.hand;
        for (int guard = 0; guard < numBones && child >= 0 && child < numBones; ++guard)
        {
            const int parent = boneParents[static_cast<size_t>(child)];
            if (parent < 0 || parent >= numBones ||
                !freezeMask[static_cast<size_t>(parent)])
            {
                break;
            }

            Vector childRestOffset{};
            if (!HooksNativeViewmodelHandsOnlyReadBoneRestPos(
                drawState,
                boneIndex,
                stride,
                child,
                childRestOffset))
            {
                break;
            }

            const Vector childOrigin =
                vr_vm_stabilize::GetOrigin(inOutAnchorLocalBones[static_cast<size_t>(child)]);
            if (!HooksNativeViewmodelHandsOnlyVectorFinite(childOrigin) ||
                !HooksNativeViewmodelHandsOnlyVectorFinite(childRestOffset))
            {
                break;
            }

            const Vector parentOrigin = childOrigin - childRestOffset;
            if (!HooksNativeViewmodelHandsOnlyVectorFinite(parentOrigin))
                break;

            inOutAnchorLocalBones[static_cast<size_t>(parent)] =
                HooksNativeViewmodelHandsOnlyCollapsedBoneAt(parentOrigin);
            ++neutralized;

            if (parent == keepSide.forearm)
                break;

            child = parent;
        }

        if (neutralized <= 0 ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(inOutAnchorLocalBones[static_cast<size_t>(keepSide.hand)]) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(inOutAnchorLocalBones[static_cast<size_t>(keepSide.forearm)]))
        {
            return neutralized;
        }

        const vr_vm_stabilize::Mat3x4& handBone =
            inOutAnchorLocalBones[static_cast<size_t>(keepSide.hand)];
        const Vector handPos = vr_vm_stabilize::GetOrigin(handBone);
        const Vector forearmPos =
            vr_vm_stabilize::GetOrigin(inOutAnchorLocalBones[static_cast<size_t>(keepSide.forearm)]);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(handPos) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(forearmPos))
        {
            return neutralized;
        }

        Vector normal = handPos - forearmPos;
        Vector wristNormal = normal;
        const bool wristOnArmChain =
            keepSide.wrist >= 0 &&
            keepSide.wrist < numBones &&
            freezeMask[static_cast<size_t>(keepSide.wrist)] &&
            HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, keepSide.hand, keepSide.wrist, numBones) &&
            HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, keepSide.wrist, keepSide.forearm, numBones);
        if (wristOnArmChain &&
            HooksNativeViewmodelHandsOnlyMatrixFinite(inOutAnchorLocalBones[static_cast<size_t>(keepSide.wrist)]))
        {
            const Vector candidate =
                handPos - vr_vm_stabilize::GetOrigin(inOutAnchorLocalBones[static_cast<size_t>(keepSide.wrist)]);
            const float candidateLen = candidate.Length();
            if (std::isfinite(candidateLen) && candidateLen > 0.001f)
                wristNormal = candidate;
        }

        const float len = normal.Length();
        if (!std::isfinite(len) || len < 0.001f)
            return neutralized;

        if (keepSide.autoCanonicalCutNormal)
            return neutralized;

        normal = HooksNativeViewmodelHandsOnlyResolveArmBendNormal(
            vr,
            handBone,
            normal,
            wristNormal,
            keepSide.side,
            keepSide.cutRotationDeg);
        normal = HooksNormalizeVector(normal, handPos - forearmPos);
        if (normal.Length() <= 0.0001f)
            return neutralized;

        const float trimDistance =
            HooksNativeViewmodelHandsOnlyResolveSideTrimDistance(vr, len, keepSide.side);
        const Vector anchorPos = handPos + (normal * trimDistance);
        const float wristKeepDistance =
            HooksNativeViewmodelHandsOnlyResolveWristKeepDistance(vr, len);
        const Vector planePoint = anchorPos - (normal * wristKeepDistance);
        outWristPlaneLocal[0] = normal.x;
        outWristPlaneLocal[1] = normal.y;
        outWristPlaneLocal[2] = normal.z;
        outWristPlaneLocal[3] = -DotProduct(normal, planePoint);
        outWristPlaneValid =
            HooksNativeViewmodelHandsOnlyNormalizePlane(outWristPlaneLocal) &&
            HooksNativeViewmodelHandsOnlyPlaneFinite(outWristPlaneLocal);
        return neutralized;
    }

    struct HooksNativeViewmodelHandsOnlyFreezeCache
    {
        VR* owner = nullptr;
        std::string modelName;
        uint32_t boneLayoutSignature = 0;
        uint32_t generation = 0;
        int numBones = 0;
        int side = 0;
        int handBone = -1;
        int anchorBone = -1;
        float armBendScale = 1.0f;
        Vector cutRotationDeg = Vector(0.0f, 0.0f, 0.0f);
        bool autoCanonicalCutNormal = false;
        Vector freezePoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
        Vector freezePoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
        Vector leftPoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
        Vector leftPoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
        bool valid = false;
        vr_vm_stabilize::Mat3x4 frozenAnchorWorld{};
        bool frozenWristPlaneValid = false;
        float frozenWristPlaneWorld[4]{};
        float frozenWristPlaneLocal[4]{};
        std::vector<uint8_t> freezeMask;
        std::vector<vr_vm_stabilize::Mat3x4> frozenLocalBones;

        void Reset()
        {
            owner = nullptr;
            modelName.clear();
            boneLayoutSignature = 0;
            generation = 0;
            numBones = 0;
            side = 0;
            handBone = -1;
            anchorBone = -1;
            armBendScale = 1.0f;
            cutRotationDeg = Vector(0.0f, 0.0f, 0.0f);
            autoCanonicalCutNormal = false;
            freezePoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
            freezePoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
            leftPoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
            leftPoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
            valid = false;
            frozenAnchorWorld = vr_vm_stabilize::Mat3x4{};
            frozenWristPlaneValid = false;
            memset(frozenWristPlaneWorld, 0, sizeof(frozenWristPlaneWorld));
            memset(frozenWristPlaneLocal, 0, sizeof(frozenWristPlaneLocal));
            freezeMask.clear();
            frozenLocalBones.clear();
        }
    };

    inline HooksNativeViewmodelHandsOnlyFreezeCache& HooksNativeViewmodelHandsOnlyFreezeCacheInstance(int side)
    {
        static HooksNativeViewmodelHandsOnlyFreezeCache leftCache;
        static HooksNativeViewmodelHandsOnlyFreezeCache rightCache;
        return (side < 0) ? leftCache : rightCache;
    }

    inline uint32_t HooksNativeViewmodelHandsOnlyBuildBoneLayoutSignature(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones)
    {
        uint32_t hash = 2166136261u;
        hash = HooksFnv1aUpdate(hash, &numBones, sizeof(numBones));
        for (int bone = 0; bone < numBones; ++bone)
        {
            const int parent =
                bone < static_cast<int>(boneParents.size())
                ? boneParents[static_cast<size_t>(bone)]
                : -1;
            hash = HooksFnv1aUpdate(hash, &bone, sizeof(bone));
            hash = HooksFnv1aUpdate(hash, &parent, sizeof(parent));
            if (bone < static_cast<int>(boneNames.size()))
                hash = HooksFnv1aUpdateString(hash, boneNames[static_cast<size_t>(bone)]);
            else
                hash = HooksFnv1aUpdateString(hash, "");
        }
        return hash == 0 ? 1u : hash;
    }

    inline Vector HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(VR* vr, int side);

    inline const char* HooksNativeViewmodelHandsOnlyFreezePoseConfigPath()
    {
        return "VR\\native_viewmodel_hands_freeze_pose.txt";
    }

    inline const char* HooksNativeViewmodelHandsOnlyFreezePoseSideName(int side)
    {
        return (side < 0) ? "left" : "right";
    }

    inline std::string HooksNativeViewmodelHandsOnlySanitizePipeField(const std::string& value)
    {
        std::string out = value;
        for (char& ch : out)
        {
            if (ch == '|' || ch == '\r' || ch == '\n')
                ch = '_';
        }
        return out;
    }

    inline void HooksNativeViewmodelHandsOnlyAppendVector3(std::ostringstream& out, const Vector& value)
    {
        out << '|' << value.x << '|' << value.y << '|' << value.z;
    }

    inline void HooksNativeViewmodelHandsOnlyAppendPlane(std::ostringstream& out, const float plane[4])
    {
        out << '|' << plane[0] << '|' << plane[1] << '|' << plane[2] << '|' << plane[3];
    }

    inline void HooksNativeViewmodelHandsOnlyAppendMatrix(std::ostringstream& out, const vr_vm_stabilize::Mat3x4& matrix)
    {
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
                out << '|' << matrix.m[row][col];
        }
    }

    inline std::vector<std::string> HooksNativeViewmodelHandsOnlySplitPipeLine(const std::string& line)
    {
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= line.size())
        {
            const size_t end = line.find('|', start);
            if (end == std::string::npos)
            {
                fields.push_back(line.substr(start));
                break;
            }

            fields.push_back(line.substr(start, end - start));
            start = end + 1;
        }
        return fields;
    }

    inline std::string HooksNativeViewmodelHandsOnlyCanonicalFreezePoseBoneName(const std::string& lowerName)
    {
        const size_t bip01 = lowerName.find("bip01_");
        if (bip01 != std::string::npos)
            return lowerName.substr(bip01);

        static const char kValveBipedPrefix[] = "valvebiped.";
        const size_t valveBiped = lowerName.find(kValveBipedPrefix);
        if (valveBiped != std::string::npos)
            return lowerName.substr(valveBiped + std::strlen(kValveBipedPrefix));

        const size_t separator = lowerName.find_last_of(".:/\\");
        if (separator != std::string::npos && separator + 1u < lowerName.size())
            return lowerName.substr(separator + 1u);

        return lowerName;
    }

    inline bool HooksNativeViewmodelHandsOnlyFreezePoseBoneNameMatches(
        const std::string& currentLowerName,
        const std::string& currentCanonicalName,
        const std::string& fileLowerName,
        const std::string& fileCanonicalName)
    {
        if (fileLowerName.empty())
            return false;

        if (currentLowerName == fileLowerName ||
            currentCanonicalName == fileLowerName ||
            currentLowerName == fileCanonicalName ||
            currentCanonicalName == fileCanonicalName)
        {
            return true;
        }

        const auto endsWith = [](const std::string& value, const std::string& suffix) -> bool
            {
                return !suffix.empty() &&
                    value.size() >= suffix.size() &&
                    value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
            };

        return endsWith(currentLowerName, fileLowerName) ||
            endsWith(currentLowerName, fileCanonicalName) ||
            endsWith(fileLowerName, currentLowerName) ||
            endsWith(fileCanonicalName, currentCanonicalName);
    }

    inline bool HooksNativeViewmodelHandsOnlyTryMapFreezePoseBoneName(
        const std::vector<std::string>& boneNames,
        int numBones,
        int side,
        const std::string& fileBoneName,
        int& outBone)
    {
        outBone = -1;
        if (fileBoneName.empty() || numBones <= 0 || static_cast<int>(boneNames.size()) < numBones)
            return false;

        const std::string fileLowerName = vr_vm_stabilize::ToLowerAscii(fileBoneName);
        const std::string fileCanonicalName =
            HooksNativeViewmodelHandsOnlyCanonicalFreezePoseBoneName(fileLowerName);
        if (fileLowerName.empty() || fileCanonicalName.empty())
            return false;

        for (int bone = 0; bone < numBones; ++bone)
        {
            const std::string currentLowerName =
                vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            if (currentLowerName.empty())
                continue;

            if (side != 0)
            {
                const int currentSide = HooksNativeViewmodelHandsOnlyBoneSide(currentLowerName);
                if (currentSide != 0 && currentSide != side)
                    continue;
            }

            const std::string currentCanonicalName =
                HooksNativeViewmodelHandsOnlyCanonicalFreezePoseBoneName(currentLowerName);
            if (!HooksNativeViewmodelHandsOnlyFreezePoseBoneNameMatches(
                currentLowerName,
                currentCanonicalName,
                fileLowerName,
                fileCanonicalName))
            {
                continue;
            }

            outBone = bone;
            return true;
        }

        return false;
    }

    inline bool HooksNativeViewmodelHandsOnlyParseIntField(const std::string& value, int& out)
    {
        std::istringstream input(value);
        input >> out;
        return !input.fail();
    }

    inline bool HooksNativeViewmodelHandsOnlyParseUint32Field(const std::string& value, uint32_t& out)
    {
        unsigned long parsed = 0;
        std::istringstream input(value);
        input >> parsed;
        if (input.fail() || parsed > 0xFFFFFFFFul)
            return false;
        out = static_cast<uint32_t>(parsed);
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyParseFloatField(const std::string& value, float& out)
    {
        std::istringstream input(value);
        input >> out;
        return !input.fail() && std::isfinite(out);
    }

    inline bool HooksNativeViewmodelHandsOnlyParseVector3Fields(
        const std::vector<std::string>& fields,
        size_t first,
        Vector& out)
    {
        if (fields.size() < first + 3u)
            return false;

        return HooksNativeViewmodelHandsOnlyParseFloatField(fields[first + 0u], out.x) &&
            HooksNativeViewmodelHandsOnlyParseFloatField(fields[first + 1u], out.y) &&
            HooksNativeViewmodelHandsOnlyParseFloatField(fields[first + 2u], out.z) &&
            HooksNativeViewmodelHandsOnlyVectorFinite(out);
    }

    inline bool HooksNativeViewmodelHandsOnlyParsePlaneFields(
        const std::vector<std::string>& fields,
        size_t first,
        float out[4])
    {
        if (fields.size() < first + 4u)
            return false;

        for (int i = 0; i < 4; ++i)
        {
            if (!HooksNativeViewmodelHandsOnlyParseFloatField(fields[first + static_cast<size_t>(i)], out[i]))
                return false;
        }
        return HooksNativeViewmodelHandsOnlyPlaneFinite(out);
    }

    inline bool HooksNativeViewmodelHandsOnlyParseMatrixFields(
        const std::vector<std::string>& fields,
        size_t first,
        vr_vm_stabilize::Mat3x4& out)
    {
        if (fields.size() < first + 12u)
            return false;

        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                const size_t index = first + static_cast<size_t>(row * 4 + col);
                if (!HooksNativeViewmodelHandsOnlyParseFloatField(fields[index], out.m[row][col]))
                    return false;
            }
        }
        return HooksNativeViewmodelHandsOnlyMatrixFinite(out);
    }

    inline std::string HooksNativeViewmodelHandsOnlyBuildFreezePoseConfigSection(
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const HooksNativeViewmodelHandsOnlyFreezeCache& cache)
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(9);
        out << "side|" << HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side) << "\n";
        out << "model|" << HooksNativeViewmodelHandsOnlySanitizePipeField(lowerModel) << "\n";
        out << "boneLayoutSignature|" << cache.boneLayoutSignature << "\n";
        out << "numBones|" << numBones << "\n";
        out << "handBone|" << keepSide.hand << '|'
            << ((keepSide.hand >= 0 && keepSide.hand < static_cast<int>(boneNames.size()))
                ? HooksNativeViewmodelHandsOnlySanitizePipeField(boneNames[static_cast<size_t>(keepSide.hand)])
                : std::string()) << "\n";
        out << "anchorBone|" << cache.anchorBone << "\n";
        out << "wristBone|" << keepSide.wrist << "\n";
        out << "forearmBone|" << keepSide.forearm << "\n";
        out << "armBendScale|" << cache.armBendScale << "\n";
        out << "cutRotationDeg";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, cache.cutRotationDeg);
        out << "\n";
        out << "autoCanonicalCutNormal|" << (cache.autoCanonicalCutNormal ? 1 : 0) << "\n";
        out << "freezePoseOffsetMeters";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, cache.freezePoseOffsetMeters);
        out << "\n";
        out << "freezePoseRotationOffsetDeg";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, cache.freezePoseRotationOffsetDeg);
        out << "\n";
        out << "leftPoseOffsetMeters";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, cache.leftPoseOffsetMeters);
        out << "\n";
        out << "leftPoseRotationOffsetDeg";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, cache.leftPoseRotationOffsetDeg);
        out << "\n";
        out << "sideInfo|handPos";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, keepSide.handPos);
        out << "|anchorPos";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, keepSide.anchorPos);
        out << "|forearmPos";
        HooksNativeViewmodelHandsOnlyAppendVector3(out, keepSide.forearmPos);
        out << "\n";
        out << "frozenAnchorWorld";
        HooksNativeViewmodelHandsOnlyAppendMatrix(out, cache.frozenAnchorWorld);
        out << "\n";
        out << "frozenWristPlaneValid|" << (cache.frozenWristPlaneValid ? 1 : 0) << "\n";
        out << "frozenWristPlaneWorld";
        HooksNativeViewmodelHandsOnlyAppendPlane(out, cache.frozenWristPlaneWorld);
        out << "\n";
        out << "frozenWristPlaneLocal";
        HooksNativeViewmodelHandsOnlyAppendPlane(out, cache.frozenWristPlaneLocal);
        out << "\n";
        out << "sourceWristPlaneWorld";
        HooksNativeViewmodelHandsOnlyAppendPlane(out, keepSide.wristPlaneWorld);
        out << "\n";

        for (int bone = 0; bone < numBones; ++bone)
        {
            const int parent =
                bone < static_cast<int>(boneParents.size())
                ? boneParents[static_cast<size_t>(bone)]
                : -1;
            const bool frozen =
                bone < static_cast<int>(cache.freezeMask.size()) &&
                cache.freezeMask[static_cast<size_t>(bone)] != 0u;
            const std::string name =
                bone < static_cast<int>(boneNames.size())
                ? HooksNativeViewmodelHandsOnlySanitizePipeField(boneNames[static_cast<size_t>(bone)])
                : std::string();
            const vr_vm_stabilize::Mat3x4 matrix =
                bone < static_cast<int>(cache.frozenLocalBones.size())
                ? cache.frozenLocalBones[static_cast<size_t>(bone)]
                : vr_vm_stabilize::Mat3x4{};

            out << "bone|" << bone << '|' << parent << '|' << (frozen ? 1 : 0) << '|' << name;
            HooksNativeViewmodelHandsOnlyAppendMatrix(out, matrix);
            out << "\n";
        }
        out << "end\n";
        return out.str();
    }

    inline void HooksNativeViewmodelHandsOnlyLogFreezePoseConfigSection(
        VR* vr,
        const std::string& section)
    {
        if (!vr || !vr->m_VrHandsDebugLog)
            return;

        std::istringstream input(section);
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty())
                Game::logMsg("[VR][NativeHandsOnly][FreezePoseConfig] %s", line.c_str());
        }
    }

    inline bool HooksNativeViewmodelHandsOnlyWriteFreezePoseConfig(
        VR* vr,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const HooksNativeViewmodelHandsOnlyFreezeCache& cache)
    {
        if (!cache.valid || numBones <= 0 || numBones > 512 ||
            static_cast<int>(cache.freezeMask.size()) != numBones ||
            static_cast<int>(cache.frozenLocalBones.size()) != numBones)
        {
            return false;
        }

        CreateDirectoryA("VR", nullptr);
        const char* path = HooksNativeViewmodelHandsOnlyFreezePoseConfigPath();
        const std::string replaceSide =
            std::string("side|") + HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side);
        std::ostringstream preserved;
        {
            std::ifstream existing(path, std::ios::in);
            std::string line;
            bool inSection = false;
            std::vector<std::string> sectionLines;
            std::string sectionSide;
            std::string sectionModel;
            auto flushSection = [&]()
                {
                    if (!inSection || sectionLines.empty())
                        return;

                    const bool replaceSection =
                        sectionSide == replaceSide &&
                        sectionModel == lowerModel;
                    if (!replaceSection)
                    {
                        for (const std::string& sectionLine : sectionLines)
                            preserved << sectionLine << "\n";
                    }
                };
            while (std::getline(existing, line))
            {
                if (line.rfind("side|", 0) == 0)
                {
                    flushSection();
                    inSection = true;
                    sectionLines.clear();
                    sectionSide = line;
                    sectionModel.clear();
                    sectionLines.push_back(line);
                    continue;
                }

                if (!inSection)
                    continue;

                sectionLines.push_back(line);
                if (line.rfind("model|", 0) == 0)
                    sectionModel = line.substr(6);
                if (line == "end")
                {
                    flushSection();
                    inSection = false;
                    sectionLines.clear();
                    sectionSide.clear();
                    sectionModel.clear();
                }
            }
            flushSection();
        }

        const std::string section =
            HooksNativeViewmodelHandsOnlyBuildFreezePoseConfigSection(
                lowerModel,
                boneNames,
                boneParents,
                numBones,
                keepSide,
                cache);

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output)
            return false;

        output << "# L4D2VR NativeViewmodelHandsOnly frozen pose config\n";
        output << "# Generated at the first runtime freeze capture. Edit this file to tune the frozen pose.\n";
        output << "version|1\n";
        const std::string preservedText = preserved.str();
        if (!preservedText.empty())
            output << preservedText;
        output << section;
        output.close();

        Game::logMsg(
            "[VR][NativeHandsOnly] wrote %s-hand freeze pose config path=%s model=%s bones=%d",
            HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side),
            path,
            lowerModel.c_str(),
            numBones);
        HooksNativeViewmodelHandsOnlyLogFreezePoseConfigSection(vr, section);
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyTryLoadFreezePoseConfig(
        VR* vr,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4& targetAnchor,
        uint32_t generation,
        HooksNativeViewmodelHandsOnlyFreezeCache& cache,
        bool* outFoundSideSection = nullptr)
    {
        if (outFoundSideSection)
            *outFoundSideSection = false;

        const char* path = HooksNativeViewmodelHandsOnlyFreezePoseConfigPath();
        std::ifstream input(path, std::ios::in);
        if (!input)
            return false;

        if (numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        const std::string wantedSide =
            std::string("side|") + HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side);
        const uint32_t currentSignature =
            HooksNativeViewmodelHandsOnlyBuildBoneLayoutSignature(
                boneNames,
                boneParents,
                numBones);

        bool inSection = false;
        bool foundSection = false;
        bool sectionModelAccepted = false;
        bool haveNumBones = false;
        bool haveSignature = false;
        bool haveFrozenAnchor = false;
        bool haveWristPlaneLocal = false;
        bool haveWristPlaneWorld = false;
        bool wristPlaneValid = false;
        int fileNumBones = 0;
        int fileHandBone = -1;
        int fileAnchorBone = -1;
        int rejectedFrozenBones = 0;
        uint32_t fileSignature = 0;
        float loadedWristPlaneLocal[4]{};
        float loadedWristPlaneWorld[4]{};
        vr_vm_stabilize::Mat3x4 loadedFrozenAnchor = targetAnchor;
        std::vector<uint8_t> freezeMask(static_cast<size_t>(numBones), 0u);
        std::vector<vr_vm_stabilize::Mat3x4> frozenLocalBones(
            static_cast<size_t>(numBones),
            vr_vm_stabilize::Mat3x4{});
        auto resetCandidate = [&]()
            {
                sectionModelAccepted = false;
                haveNumBones = false;
                haveSignature = false;
                haveFrozenAnchor = false;
                haveWristPlaneLocal = false;
                haveWristPlaneWorld = false;
                wristPlaneValid = false;
                fileNumBones = 0;
                fileHandBone = -1;
                fileAnchorBone = -1;
                rejectedFrozenBones = 0;
                fileSignature = 0;
                memset(loadedWristPlaneLocal, 0, sizeof(loadedWristPlaneLocal));
                memset(loadedWristPlaneWorld, 0, sizeof(loadedWristPlaneWorld));
                loadedFrozenAnchor = targetAnchor;
                freezeMask.assign(static_cast<size_t>(numBones), 0u);
                frozenLocalBones.assign(
                    static_cast<size_t>(numBones),
                    vr_vm_stabilize::Mat3x4{});
            };

        std::string line;
        while (std::getline(input, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            if (line.rfind("side|", 0) == 0)
            {
                if (foundSection)
                    break;
                inSection = (line == wantedSide);
                if (inSection)
                    resetCandidate();
                continue;
            }

            if (!inSection)
                continue;
            if (line == "end")
            {
                if (sectionModelAccepted)
                    break;
                inSection = false;
                continue;
            }

            const std::vector<std::string> fields =
                HooksNativeViewmodelHandsOnlySplitPipeLine(line);
            if (fields.empty())
                continue;

            if (fields[0] == "model" && fields.size() >= 2u)
            {
                sectionModelAccepted = (fields[1] == lowerModel);
                foundSection = sectionModelAccepted;
                if (foundSection && outFoundSideSection)
                    *outFoundSideSection = true;
                if (!sectionModelAccepted)
                    inSection = false;
                continue;
            }
            if (!sectionModelAccepted)
                continue;

            if (fields[0] == "boneLayoutSignature" && fields.size() >= 2u)
            {
                haveSignature = HooksNativeViewmodelHandsOnlyParseUint32Field(fields[1], fileSignature);
            }
            else if (fields[0] == "numBones" && fields.size() >= 2u)
            {
                haveNumBones = HooksNativeViewmodelHandsOnlyParseIntField(fields[1], fileNumBones);
            }
            else if (fields[0] == "handBone" && fields.size() >= 2u)
            {
                HooksNativeViewmodelHandsOnlyParseIntField(fields[1], fileHandBone);
            }
            else if (fields[0] == "anchorBone" && fields.size() >= 2u)
            {
                HooksNativeViewmodelHandsOnlyParseIntField(fields[1], fileAnchorBone);
            }
            else if (fields[0] == "frozenAnchorWorld")
            {
                haveFrozenAnchor =
                    HooksNativeViewmodelHandsOnlyParseMatrixFields(fields, 1u, loadedFrozenAnchor);
            }
            else if (fields[0] == "frozenWristPlaneValid" && fields.size() >= 2u)
            {
                int valid = 0;
                if (HooksNativeViewmodelHandsOnlyParseIntField(fields[1], valid))
                    wristPlaneValid = valid != 0;
            }
            else if (fields[0] == "frozenWristPlaneLocal")
            {
                haveWristPlaneLocal =
                    HooksNativeViewmodelHandsOnlyParsePlaneFields(fields, 1u, loadedWristPlaneLocal);
            }
            else if (fields[0] == "frozenWristPlaneWorld")
            {
                haveWristPlaneWorld =
                    HooksNativeViewmodelHandsOnlyParsePlaneFields(fields, 1u, loadedWristPlaneWorld);
            }
            else if (fields[0] == "bone" && fields.size() >= 17u &&
                static_cast<int>(freezeMask.size()) == numBones &&
                static_cast<int>(frozenLocalBones.size()) == numBones)
            {
                int fileBone = -1;
                int frozen = 0;
                if (!HooksNativeViewmodelHandsOnlyParseIntField(fields[1], fileBone) ||
                    !HooksNativeViewmodelHandsOnlyParseIntField(fields[3], frozen) ||
                    frozen == 0)
                {
                    continue;
                }

                vr_vm_stabilize::Mat3x4 matrix{};
                if (!HooksNativeViewmodelHandsOnlyParseMatrixFields(fields, 5u, matrix))
                    return false;

                int currentBone = -1;
                if (!HooksNativeViewmodelHandsOnlyTryMapFreezePoseBoneName(
                    boneNames,
                    numBones,
                    keepSide.side,
                    fields[4],
                    currentBone) &&
                    haveNumBones &&
                    fileNumBones == numBones &&
                    fileBone >= 0 &&
                    fileBone < numBones)
                {
                    currentBone = fileBone;
                }

                if (currentBone < 0 || currentBone >= numBones)
                    continue;

                if (!HooksNativeViewmodelHandsOnlyFrozenAnchorLocalMatrixPlausible(matrix))
                {
                    ++rejectedFrozenBones;
                    if (rejectedFrozenBones <= 8)
                    {
                        const Vector origin = vr_vm_stabilize::GetOrigin(matrix);
                        Game::logMsg(
                            "[VR][NativeHandsOnly] ignored implausible %s-hand frozen bone model=%s bone=%d name=%s anchorLocal=(%.2f %.2f %.2f)",
                            HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side),
                            lowerModel.c_str(),
                            currentBone,
                            fields[4].c_str(),
                            origin.x,
                            origin.y,
                            origin.z);
                    }
                    continue;
                }

                freezeMask[static_cast<size_t>(currentBone)] = 1u;
                frozenLocalBones[static_cast<size_t>(currentBone)] = matrix;
            }
        }

        if (!foundSection ||
            static_cast<int>(freezeMask.size()) != numBones ||
            static_cast<int>(frozenLocalBones.size()) != numBones)
        {
            return false;
        }

        int frozenBones = 0;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!freezeMask[static_cast<size_t>(bone)])
                continue;
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(frozenLocalBones[static_cast<size_t>(bone)]))
                return false;
            ++frozenBones;
        }
        if (frozenBones <= 0)
            return false;
        if (keepSide.forearm >= 0 &&
            keepSide.forearm < numBones &&
            !freezeMask[static_cast<size_t>(keepSide.forearm)])
        {
            Game::logMsg(
                "[VR][NativeHandsOnly] ignoring %s-hand freeze pose config because forearm bone is not frozen path=%s bone=%d",
                HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side),
                path,
                keepSide.forearm);
            return false;
        }

        const bool remappedLayout =
            (haveNumBones && fileNumBones != numBones) ||
            (haveSignature && fileSignature != currentSignature) ||
            (fileHandBone >= 0 && fileHandBone != keepSide.hand) ||
            (fileAnchorBone >= 0 && fileAnchorBone != keepSide.hand);

        cache.Reset();
        cache.owner = vr;
        cache.modelName = lowerModel;
        cache.boneLayoutSignature = currentSignature;
        cache.generation = generation;
        cache.numBones = numBones;
        cache.side = keepSide.side;
        cache.handBone = keepSide.hand;
        cache.anchorBone = keepSide.hand;
        cache.armBendScale = std::clamp(vr->m_NativeViewmodelHandsOnlyArmBendScale, 0.0f, 1.0f);
        cache.cutRotationDeg = keepSide.cutRotationDeg;
        cache.autoCanonicalCutNormal = keepSide.autoCanonicalCutNormal;
        cache.freezePoseOffsetMeters = vr->m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters;
        cache.freezePoseRotationOffsetDeg =
            HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(vr, keepSide.side);
        cache.leftPoseOffsetMeters = vr->m_NativeViewmodelLeftHandPoseOffsetMeters;
        cache.leftPoseRotationOffsetDeg = vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg;
        cache.valid = true;
        cache.frozenAnchorWorld = haveFrozenAnchor ? loadedFrozenAnchor : targetAnchor;
        cache.frozenWristPlaneValid = wristPlaneValid && haveWristPlaneLocal;
        memset(cache.frozenWristPlaneWorld, 0, sizeof(cache.frozenWristPlaneWorld));
        memset(cache.frozenWristPlaneLocal, 0, sizeof(cache.frozenWristPlaneLocal));
        if (cache.frozenWristPlaneValid)
        {
            memcpy(cache.frozenWristPlaneLocal, loadedWristPlaneLocal, sizeof(cache.frozenWristPlaneLocal));
            if (haveWristPlaneWorld)
                memcpy(cache.frozenWristPlaneWorld, loadedWristPlaneWorld, sizeof(cache.frozenWristPlaneWorld));
        }
        cache.freezeMask.swap(freezeMask);
        cache.frozenLocalBones.swap(frozenLocalBones);

        Game::logMsg(
            "[VR][NativeHandsOnly] loaded %s-hand freeze pose config path=%s model=%s bones=%d frozen=%d rejected=%d remapped=%d sourceBones=%d sourceSignature=%u",
            HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side),
            path,
            lowerModel.c_str(),
            numBones,
            frozenBones,
            rejectedFrozenBones,
            remappedLayout ? 1 : 0,
            haveNumBones ? fileNumBones : 0,
            haveSignature ? fileSignature : 0u);
        return true;
    }

    inline Vector HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(VR* vr, int side)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        return (side < 0)
            ? vr->m_NativeViewmodelHandsOnlyLeftFreezePoseRotationOffsetDeg
            : vr->m_NativeViewmodelHandsOnlyRightFreezePoseRotationOffsetDeg;
    }

    inline Vector HooksNativeViewmodelHandsOnlyBaseCutRotationDeg(VR* vr, int side)
    {
        if (!vr)
            return Vector(0.0f, 0.0f, 0.0f);

        return (side < 0)
            ? vr->m_NativeViewmodelHandsOnlyLeftCutRotationDeg
            : vr->m_NativeViewmodelHandsOnlyRightCutRotationDeg;
    }

    inline bool HooksNativeViewmodelHandsOnlyTryResolveExplicitCutRotationDeg(
        VR* vr,
        int side,
        const std::string* lowerModel,
        Vector& outRotation)
    {
        if (!vr || !lowerModel || lowerModel->empty())
            return false;

        outRotation = HooksNativeViewmodelHandsOnlyBaseCutRotationDeg(vr, side);

        for (const auto& overrideEntry : vr->m_NativeViewmodelHandsOnlyCutRotationOverrides)
        {
            if (overrideEntry.modelPattern.empty() ||
                lowerModel->find(overrideEntry.modelPattern) == std::string::npos)
            {
                continue;
            }

            const bool useLeft = side < 0;
            const bool hasOverride = useLeft ? overrideEntry.hasLeft : overrideEntry.hasRight;
            if (!hasOverride)
                return false;

            outRotation = useLeft ? overrideEntry.left : overrideEntry.right;
            if (vr->m_VrHandsDebugLog)
            {
                static std::mutex s_cutRotationLogMutex;
                static std::unordered_set<std::string> s_loggedCutRotationOverrides;
                const std::string logKey =
                    *lowerModel + "|" + std::to_string(side) + "|" + overrideEntry.modelPattern;
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(s_cutRotationLogMutex);
                    shouldLog = s_loggedCutRotationOverrides.insert(logKey).second;
                }
                if (shouldLog)
                {
                    Game::logMsg(
                        "[VR][NativeHandsOnly] cut rotation override model=\"%s\" pattern=\"%s\" side=%s rotation=(%.2f %.2f %.2f)",
                        lowerModel->c_str(),
                        overrideEntry.modelPattern.c_str(),
                        useLeft ? "left" : "right",
                        outRotation.x,
                        outRotation.y,
                        outRotation.z);
                }
            }
            return true;
        }

        return false;
    }

    inline Vector HooksNativeViewmodelHandsOnlyResolveCutRotationDeg(
        VR* vr,
        int side,
        const std::string* lowerModel)
    {
        Vector explicitRotation{};
        if (HooksNativeViewmodelHandsOnlyTryResolveExplicitCutRotationDeg(
            vr,
            side,
            lowerModel,
            explicitRotation))
        {
            return explicitRotation;
        }

        return HooksNativeViewmodelHandsOnlyBaseCutRotationDeg(vr, side);
    }

    inline float HooksNativeViewmodelHandsOnlyNormalizeAngleDelta(float degrees)
    {
        if (!std::isfinite(degrees))
            return 0.0f;

        while (degrees > 180.0f)
            degrees -= 360.0f;
        while (degrees < -180.0f)
            degrees += 360.0f;
        return degrees;
    }

    inline void HooksNativeViewmodelHandsOnlyLogLeftFreezeControllerPose(
        VR* vr,
        uint32_t generation,
        const Vector& freezePoseRotationOffsetDeg)
    {
        if (!vr)
            return;

        const QAngle controllerAngles = vr->GetLeftControllerAbsAngle();
        const Vector controllerPos = vr->GetLeftControllerAbsPos();
        const Vector hmdAngles = vr->GetViewAngle();
        const float deltaPitch = HooksNativeViewmodelHandsOnlyNormalizeAngleDelta(controllerAngles.x - hmdAngles.x);
        const float deltaYaw = HooksNativeViewmodelHandsOnlyNormalizeAngleDelta(controllerAngles.y - hmdAngles.y);
        const float deltaRoll = HooksNativeViewmodelHandsOnlyNormalizeAngleDelta(controllerAngles.z - hmdAngles.z);

        Game::logMsg(
            "[VR][NativeHandsOnly] left-hand freeze controller pose generation=%u angleAbs=(%.2f %.2f %.2f) hmdAngle=(%.2f %.2f %.2f) angleMinusHmd=(%.2f %.2f %.2f) posAbs=(%.2f %.2f %.2f) leftFreezePoseRotation=(%.2f %.2f %.2f) leftNativePoseRotation=(%.2f %.2f %.2f)",
            generation,
            controllerAngles.x,
            controllerAngles.y,
            controllerAngles.z,
            hmdAngles.x,
            hmdAngles.y,
            hmdAngles.z,
            deltaPitch,
            deltaYaw,
            deltaRoll,
            controllerPos.x,
            controllerPos.y,
            controllerPos.z,
            freezePoseRotationOffsetDeg.x,
            freezePoseRotationOffsetDeg.y,
            freezePoseRotationOffsetDeg.z,
            vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg.x,
            vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg.y,
            vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg.z);
    }

    inline bool HooksNativeViewmodelHandsOnlyTryBuildCanonicalFreezeTargetDelta(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        vr_vm_stabilize::Mat3x4& outAnchor,
        vr_vm_stabilize::Mat3x4& outDelta);

    inline bool HooksNativeViewmodelHandsOnlyBuildCanonicalFreezeWristPlaneWorld(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& canonicalAnchor,
        const vr_vm_stabilize::Mat3x4& canonicalDelta,
        float outPlane[4]);

    inline bool HooksNativeViewmodelHandsOnlyShouldFreezeSideHandBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const vr_vm_stabilize::Mat3x4* captureBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        int bone)
    {
        if (keepSide.side == 0 || bone < 0 || bone >= numBones || keepSide.hand < 0 || keepSide.hand >= numBones)
            return false;

        const bool haveName = bone < static_cast<int>(boneNames.size());
        const std::string lowerName = haveName
            ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)])
            : std::string();
        const int boneSide = lowerName.empty() ? 0 : HooksNativeViewmodelHandsOnlyBoneSide(lowerName);
        const bool belongsToSide = boneSide == 0 || boneSide == keepSide.side;

        if (belongsToSide &&
            (bone == keepSide.forearm ||
                bone == keepSide.wrist ||
                (keepSide.forearm >= 0 &&
                    HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, keepSide.hand, bone, numBones) &&
                    HooksNativeViewmodelHandsOnlyIsAncestor(boneParents, bone, keepSide.forearm, numBones))))
        {
            return true;
        }

        if (belongsToSide &&
            !lowerName.empty() &&
            (HooksNativeViewmodelHandsOnlyBoneNameLooksArmKeep(lowerName) ||
                HooksNativeViewmodelHandsOnlyBoneNameLooksWristBlend(lowerName)) &&
            HooksNativeViewmodelHandsOnlyBoneNearSideHandRegion(captureBones, bone, keepSide))
        {
            return true;
        }

        if (keepSide.side > 0)
        {
            if (HooksNativeViewmodelHandsOnlyBoneOrAncestorLooksFingerOnly(
                boneNames,
                boneParents,
                numBones,
                bone))
            {
                return false;
            }

            return HooksNativeViewmodelHandsOnlyShouldKeepBone(
                boneNames,
                boneParents,
                numBones,
                captureBones,
                keepSide,
                keepSide.wristKeepDistance,
                bone);
        }

        return HooksNativeViewmodelHandsOnlyShouldKeepBone(
            boneNames,
            boneParents,
            numBones,
            captureBones,
            keepSide,
            keepSide.wristKeepDistance,
            bone);
    }

    inline bool HooksNativeViewmodelHandsOnlyCaptureFrozenSideHandPose(
        VR* vr,
        void* drawState,
        int boneIndex,
        int stride,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* captureBones,
        const vr_vm_stabilize::Mat3x4& targetAnchor,
        const vr_vm_stabilize::Mat3x4& targetDelta,
        const float* captureWristPlaneWorld,
        uint32_t generation,
        HooksNativeViewmodelHandsOnlyFreezeCache& cache)
    {
        if (!vr || !captureBones || keepSide.side == 0 || numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones || static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(targetDelta))
            return false;

        (void)drawState;
        (void)boneIndex;
        (void)stride;

        vr_vm_stabilize::Mat3x4 inverseAnchor{};
        vr_vm_stabilize::InvertTR(targetAnchor, inverseAnchor);

        std::vector<uint8_t> frozenFingerDriveMask;
        std::vector<uint8_t> thumbRootPreserveMask;
        if (keepSide.side == -1 && vr->m_NativeViewmodelLeftHandOpenVRSkeleton)
        {
            HooksNativeViewmodelHandsOnlyBuildLeftFingerDriveMask(
                boneNames,
                boneParents,
                numBones,
                nullptr,
                0.0f,
                0.0f,
                frozenFingerDriveMask,
                nullptr,
                nullptr);
            HooksNativeViewmodelHandsOnlyBuildLeftThumbRootMask(
                boneNames,
                numBones,
                thumbRootPreserveMask);
        }

        std::vector<uint8_t> freezeMask(static_cast<size_t>(numBones), 0u);
        std::vector<vr_vm_stabilize::Mat3x4> frozenLocalBones(static_cast<size_t>(numBones));
        int frozenBones = 0;
        int rejectedFrozenBones = 0;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!HooksNativeViewmodelHandsOnlyShouldFreezeSideHandBone(
                boneNames,
                boneParents,
                numBones,
                captureBones,
                keepSide,
                bone))
            {
                continue;
            }

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(captureBones + bone, boneWorld) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(boneWorld))
            {
                return false;
            }

            vr_vm_stabilize::Mat3x4 finalWorld{};
            vr_vm_stabilize::Mul(targetDelta, boneWorld, finalWorld);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(finalWorld))
                return false;

            vr_vm_stabilize::Mat3x4 local{};
            vr_vm_stabilize::Mul(inverseAnchor, finalWorld, local);
            if (!HooksNativeViewmodelHandsOnlyFrozenAnchorLocalMatrixPlausible(local))
            {
                ++rejectedFrozenBones;
                continue;
            }

            freezeMask[static_cast<size_t>(bone)] = 1u;
            frozenLocalBones[static_cast<size_t>(bone)] = local;
            ++frozenBones;
        }

        if (frozenBones <= 0)
            return false;
        if (keepSide.hand < 0 || keepSide.hand >= numBones ||
            keepSide.forearm < 0 || keepSide.forearm >= numBones ||
            !freezeMask[static_cast<size_t>(keepSide.hand)] ||
            !freezeMask[static_cast<size_t>(keepSide.forearm)])
        {
            return false;
        }

        int neutralFingerBones = 0;
        if (static_cast<int>(frozenFingerDriveMask.size()) == numBones)
        {
            neutralFingerBones = HooksNativeViewmodelHandsOnlyNeutralizeFrozenLeftFingerRotations(
                boneParents,
                numBones,
                frozenFingerDriveMask,
                thumbRootPreserveMask,
                freezeMask,
                frozenLocalBones);
        }

        float neutralArmPlaneLocal[4]{};
        bool neutralArmPlaneValid = false;
        const int neutralArmBones = HooksNativeViewmodelHandsOnlyNeutralizeFrozenSideArmPose(
            vr,
            drawState,
            boneIndex,
            stride,
            boneParents,
            numBones,
            keepSide,
            freezeMask,
            frozenLocalBones,
            neutralArmPlaneLocal,
            neutralArmPlaneValid);

        cache.owner = vr;
        cache.modelName = lowerModel;
        cache.boneLayoutSignature =
            HooksNativeViewmodelHandsOnlyBuildBoneLayoutSignature(
                boneNames,
                boneParents,
                numBones);
        cache.generation = generation;
        cache.numBones = numBones;
        cache.side = keepSide.side;
        cache.handBone = keepSide.hand;
        cache.anchorBone = keepSide.hand;
        cache.armBendScale = std::clamp(vr->m_NativeViewmodelHandsOnlyArmBendScale, 0.0f, 1.0f);
        cache.cutRotationDeg = keepSide.cutRotationDeg;
        cache.autoCanonicalCutNormal = keepSide.autoCanonicalCutNormal;
        cache.freezePoseOffsetMeters = vr->m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters;
        cache.freezePoseRotationOffsetDeg =
            HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(vr, keepSide.side);
        cache.leftPoseOffsetMeters = vr->m_NativeViewmodelLeftHandPoseOffsetMeters;
        cache.leftPoseRotationOffsetDeg = vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg;
        cache.valid = true;
        cache.frozenAnchorWorld = targetAnchor;
        cache.frozenWristPlaneValid = false;
        memset(cache.frozenWristPlaneWorld, 0, sizeof(cache.frozenWristPlaneWorld));
        memset(cache.frozenWristPlaneLocal, 0, sizeof(cache.frozenWristPlaneLocal));
        if (neutralArmPlaneValid)
        {
            memcpy(cache.frozenWristPlaneLocal, neutralArmPlaneLocal, sizeof(cache.frozenWristPlaneLocal));
            cache.frozenWristPlaneValid = true;

            float neutralArmPlaneWorld[4]{};
            if (HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
                targetAnchor,
                neutralArmPlaneLocal,
                neutralArmPlaneWorld))
            {
                memcpy(cache.frozenWristPlaneWorld, neutralArmPlaneWorld, sizeof(cache.frozenWristPlaneWorld));
            }
        }
        else if (captureWristPlaneWorld && HooksNativeViewmodelHandsOnlyPlaneFinite(captureWristPlaneWorld))
        {
            bool storedWorldPlane = false;
            float frozenPlaneWorld[4] = {
                captureWristPlaneWorld[0],
                captureWristPlaneWorld[1],
                captureWristPlaneWorld[2],
                captureWristPlaneWorld[3],
            };
            if (HooksNativeViewmodelHandsOnlyNormalizePlane(frozenPlaneWorld))
            {
                memcpy(cache.frozenWristPlaneWorld, frozenPlaneWorld, sizeof(cache.frozenWristPlaneWorld));
                storedWorldPlane = true;
            }

            vr_vm_stabilize::Mat3x4 inversePlaneAnchor{};
            vr_vm_stabilize::InvertTR(targetAnchor, inversePlaneAnchor);
            float localPlane[4]{};
            if (storedWorldPlane &&
                HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
                    inversePlaneAnchor,
                    frozenPlaneWorld,
                    localPlane))
            {
                memcpy(cache.frozenWristPlaneLocal, localPlane, sizeof(cache.frozenWristPlaneLocal));
                cache.frozenWristPlaneValid = true;
            }
        }
        cache.freezeMask.swap(freezeMask);
        cache.frozenLocalBones.swap(frozenLocalBones);

        if (vr->m_VrHandsDebugLog)
        {
            Game::logMsg(
                "[VR][NativeHandsOnly] captured frozen %s-hand animation pose model=%s bones=%d frozen=%d rejected=%d neutralFingers=%d neutralArm=%d hand=%d plane=%d generation=%u",
                (keepSide.side < 0) ? "left" : "right",
                lowerModel.c_str(),
                numBones,
                frozenBones,
                rejectedFrozenBones,
                neutralFingerBones,
                neutralArmBones,
                keepSide.hand,
                cache.frozenWristPlaneValid ? 1 : 0,
                generation);
        }
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyApplyFrozenSideHandPose(
        VR* vr,
        void* drawState,
        int boneIndex,
        int stride,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* captureBones,
        const vr_vm_stabilize::Mat3x4& targetAnchor,
        const vr_vm_stabilize::Mat3x4& targetDelta,
        vr_vm_stabilize::Mat3x4* currentBones,
        float* inOutWristPlaneWorld)
    {
        if (!vr || !captureBones || !currentBones || keepSide.side == 0 ||
            (keepSide.side < 0 && vr->IsVrHandsTwoHandedGripPoseActive()) ||
            vr->m_NativeViewmodelLeftHandFreezeReady.load(std::memory_order_acquire) == 0u)
        {
            return false;
        }

        const uint32_t generation =
            vr->m_NativeViewmodelLeftHandFreezeGeneration.load(std::memory_order_acquire);
        const float armBendScale = std::clamp(vr->m_NativeViewmodelHandsOnlyArmBendScale, 0.0f, 1.0f);
        const Vector cutRotationDeg = keepSide.cutRotationDeg;
        const bool autoCanonicalCutNormal = keepSide.autoCanonicalCutNormal;
        const uint32_t boneLayoutSignature =
            HooksNativeViewmodelHandsOnlyBuildBoneLayoutSignature(
                boneNames,
                boneParents,
                numBones);
        const Vector freezePoseOffsetMeters = vr->m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters;
        const Vector freezePoseRotationOffsetDeg =
            HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(vr, keepSide.side);
        const Vector leftPoseOffsetMeters = vr->m_NativeViewmodelLeftHandPoseOffsetMeters;
        const Vector leftPoseRotationOffsetDeg = vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg;
        static std::mutex s_leftHandFreezeMutex;
        std::lock_guard<std::mutex> lock(s_leftHandFreezeMutex);

        HooksNativeViewmodelHandsOnlyFreezeCache& cache =
            HooksNativeViewmodelHandsOnlyFreezeCacheInstance(keepSide.side);
        if (cache.owner != vr)
            cache.Reset();

        const bool cacheMatches =
            cache.valid &&
            cache.owner == vr &&
            cache.modelName == lowerModel &&
            cache.boneLayoutSignature == boneLayoutSignature &&
            cache.generation == generation &&
            cache.side == keepSide.side &&
            std::fabs(cache.armBendScale - armBendScale) <= 0.0001f &&
            (cache.cutRotationDeg - cutRotationDeg).LengthSqr() <= 0.0001f &&
            cache.autoCanonicalCutNormal == autoCanonicalCutNormal &&
            (cache.freezePoseOffsetMeters - freezePoseOffsetMeters).LengthSqr() <= 0.000001f &&
            (cache.freezePoseRotationOffsetDeg - freezePoseRotationOffsetDeg).LengthSqr() <= 0.0001f &&
            (cache.leftPoseOffsetMeters - leftPoseOffsetMeters).LengthSqr() <= 0.000001f &&
            (cache.leftPoseRotationOffsetDeg - leftPoseRotationOffsetDeg).LengthSqr() <= 0.0001f &&
            cache.numBones == numBones &&
            cache.handBone == keepSide.hand &&
            cache.anchorBone == keepSide.hand &&
            static_cast<int>(cache.freezeMask.size()) == numBones &&
            static_cast<int>(cache.frozenLocalBones.size()) == numBones;

        if (!cacheMatches)
        {
            cache.Reset();
            bool foundFreezePoseSideSection = false;
            if (!HooksNativeViewmodelHandsOnlyTryLoadFreezePoseConfig(
                vr,
                lowerModel,
                boneNames,
                boneParents,
                numBones,
                keepSide,
                targetAnchor,
                generation,
                cache,
                &foundFreezePoseSideSection))
            {
                vr_vm_stabilize::Mat3x4 captureTargetAnchor = targetAnchor;
                vr_vm_stabilize::Mat3x4 captureTargetDelta = targetDelta;
                float captureWristPlaneWorld[4]{};
                const float* captureWristPlane = inOutWristPlaneWorld;
                bool canonicalCapture = false;
                if (HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, keepSide.side))
                {
                    canonicalCapture = HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                        vr,
                        keepSide,
                        lowerModel,
                        captureBones,
                        numBones,
                        &captureTargetAnchor,
                        &captureTargetDelta,
                        captureWristPlaneWorld);
                    if (!canonicalCapture)
                        return false;
                    captureWristPlane = captureWristPlaneWorld;
                }
                if (canonicalCapture)
                {
                    HooksNativeViewmodelHandsOnlyNormalizePlane(captureWristPlaneWorld);
                }

                if (!HooksNativeViewmodelHandsOnlyCaptureFrozenSideHandPose(
                    vr,
                    drawState,
                    boneIndex,
                    stride,
                    lowerModel,
                    boneNames,
                    boneParents,
                    numBones,
                    keepSide,
                    captureBones,
                    captureTargetAnchor,
                    captureTargetDelta,
                    captureWristPlane,
                    generation,
                    cache))
                {
                    return false;
                }
                if (canonicalCapture &&
                    captureWristPlane &&
                    HooksNativeViewmodelHandsOnlyPlaneFinite(captureWristPlane) &&
                    HooksNativeViewmodelHandsOnlyMatrixFinite(captureTargetAnchor))
                {
                    float fixedWorldPlane[4] = {
                        captureWristPlane[0],
                        captureWristPlane[1],
                        captureWristPlane[2],
                        captureWristPlane[3],
                    };
                    if (HooksNativeViewmodelHandsOnlyNormalizePlane(fixedWorldPlane))
                    {
                        vr_vm_stabilize::Mat3x4 inverseCaptureAnchor{};
                        vr_vm_stabilize::InvertTR(captureTargetAnchor, inverseCaptureAnchor);
                        float fixedLocalPlane[4]{};
                        if (HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
                            inverseCaptureAnchor,
                            fixedWorldPlane,
                            fixedLocalPlane))
                        {
                            memcpy(cache.frozenWristPlaneWorld, fixedWorldPlane, sizeof(cache.frozenWristPlaneWorld));
                            memcpy(cache.frozenWristPlaneLocal, fixedLocalPlane, sizeof(cache.frozenWristPlaneLocal));
                            cache.frozenWristPlaneValid = true;
                        }
                    }
                }
                if (canonicalCapture && vr->m_VrHandsDebugLog)
                {
                    Game::logMsg(
                        "[VR][NativeHandsOnly] captured %s-hand freeze with canonical HMD-front horizontal anchor",
                        (keepSide.side < 0) ? "left" : "right");
                }
                if (foundFreezePoseSideSection)
                {
                    Game::logMsg(
                        "[VR][NativeHandsOnly] kept existing %s-hand freeze pose config path=%s model=%s; using temporary runtime capture because the side section could not be mapped",
                        HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side),
                        HooksNativeViewmodelHandsOnlyFreezePoseConfigPath(),
                        lowerModel.c_str());
                }
                else if (!HooksNativeViewmodelHandsOnlyWriteFreezePoseConfig(
                    vr,
                    lowerModel,
                    boneNames,
                    boneParents,
                    numBones,
                    keepSide,
                    cache))
                {
                    Game::logMsg(
                        "[VR][NativeHandsOnly] failed to write %s-hand freeze pose config path=%s",
                        HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side),
                        HooksNativeViewmodelHandsOnlyFreezePoseConfigPath());
                }
                if (keepSide.side < 0)
                {
                    HooksNativeViewmodelHandsOnlyLogLeftFreezeControllerPose(
                        vr,
                        generation,
                        freezePoseRotationOffsetDeg);
                }
            }
        }

        int frozenBones = 0;
        int rejectedFrozenBones = 0;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!cache.freezeMask[static_cast<size_t>(bone)])
                continue;
            if (!HooksNativeViewmodelHandsOnlyFrozenAnchorLocalMatrixPlausible(
                cache.frozenLocalBones[static_cast<size_t>(bone)]))
            {
                cache.freezeMask[static_cast<size_t>(bone)] = 0u;
                ++rejectedFrozenBones;
                continue;
            }
            ++frozenBones;
        }
        if (rejectedFrozenBones > 0)
        {
            Game::logMsg(
                "[VR][NativeHandsOnly] discarded %d implausible cached %s-hand frozen bones model=%s",
                rejectedFrozenBones,
                HooksNativeViewmodelHandsOnlyFreezePoseSideName(keepSide.side),
                lowerModel.c_str());
        }
        if (frozenBones <= 0)
            return false;

        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor))
            return false;

        int appliedBones = 0;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!cache.freezeMask[static_cast<size_t>(bone)])
                continue;

            vr_vm_stabilize::Mat3x4 frozenWorld{};
            vr_vm_stabilize::Mul(
                targetAnchor,
                cache.frozenLocalBones[static_cast<size_t>(bone)],
                frozenWorld);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(frozenWorld))
                return false;

            currentBones[bone] = frozenWorld;
            ++appliedBones;
        }

        if (inOutWristPlaneWorld)
        {
            bool fixedPlaneApplied = false;
            if (HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, keepSide.side))
            {
                float lockedWorldPlane[4]{};
                if (!HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                    vr,
                    keepSide,
                    lowerModel,
                    captureBones,
                    numBones,
                    nullptr,
                    nullptr,
                    lockedWorldPlane,
                    &targetAnchor))
                {
                    return false;
                }
                memcpy(inOutWristPlaneWorld, lockedWorldPlane, sizeof(lockedWorldPlane));
                fixedPlaneApplied = true;
            }

            if (!fixedPlaneApplied)
            {
                if (cache.frozenWristPlaneValid)
                {
                    float anchoredPlaneWorld[4]{};
                    if (HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
                        targetAnchor,
                        cache.frozenWristPlaneLocal,
                        anchoredPlaneWorld))
                    {
                        memcpy(inOutWristPlaneWorld, anchoredPlaneWorld, sizeof(anchoredPlaneWorld));
                    }
                }
                else
                {
                    HooksNativeViewmodelHandsOnlyReanchorPlaneToTargetHand(
                        targetAnchor,
                        keepSide.handPos,
                        inOutWristPlaneWorld);
                }
            }
        }

        return appliedBones == frozenBones;
    }

    inline bool HooksNativeViewmodelHandsOnlyBoneNameEndsWith(
        const std::string& lowerName,
        const char* lowerSuffix)
    {
        if (!lowerSuffix || !*lowerSuffix)
            return false;

        const size_t nameLen = lowerName.size();
        const size_t suffixLen = std::strlen(lowerSuffix);
        return nameLen >= suffixLen &&
            lowerName.compare(nameLen - suffixLen, suffixLen, lowerSuffix) == 0;
    }

    inline bool HooksNativeViewmodelHandsOnlyFindBoneByLowerSuffix(
        const std::vector<std::string>& boneNames,
        const char* lowerSuffix,
        int& outBone)
    {
        outBone = -1;
        if (!lowerSuffix || !*lowerSuffix)
            return false;

        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const std::string lowerName =
                vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            if (lowerName == lowerSuffix ||
                HooksNativeViewmodelHandsOnlyBoneNameEndsWith(lowerName, lowerSuffix))
            {
                outBone = bone;
                return true;
            }
        }
        return false;
    }

    inline void HooksNativeViewmodelHandsOnlyPublishLeftWristAnchor(
        VR* vr,
        const std::vector<std::string>& boneNames,
        int numBones,
        const vr_vm_stabilize::Mat3x4* currentBones)
    {
        if (!vr || !currentBones || numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones)
        {
            return;
        }

        int anchorBone = -1;
        if (!HooksNativeViewmodelHandsOnlyFindBoneByLowerSuffix(
            boneNames,
            "bip01_l_wrist",
            anchorBone) ||
            anchorBone < 0 ||
            anchorBone >= numBones)
        {
            if (!HooksNativeViewmodelHandsOnlyFindBoneByLowerSuffix(
                boneNames,
                "bip01_l_hand",
                anchorBone) ||
                anchorBone < 0 ||
                anchorBone >= numBones)
            {
                return;
            }
        }

        vr_vm_stabilize::Mat3x4 anchorWorld{};
        if (!vr_vm_stabilize::SafeRead(currentBones + anchorBone, anchorWorld) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(anchorWorld))
        {
            return;
        }

        vr->PublishMagazineInteractionNativeLeftWristAnchor(
            vr_vm_stabilize::GetOrigin(anchorWorld),
            HooksMatrixAxis(anchorWorld, 0),
            HooksMatrixAxis(anchorWorld, 1),
            HooksMatrixAxis(anchorWorld, 2));
    }

    inline vr_vm_stabilize::Mat3x4 HooksNativeViewmodelHandsOnlyMakeLocalAxisRotation(
        int axis,
        float radians)
    {
        vr_vm_stabilize::Mat3x4 out{};
        out.m[0][0] = 1.0f;
        out.m[1][1] = 1.0f;
        out.m[2][2] = 1.0f;

        const float c = std::cos(radians);
        const float s = std::sin(radians);
        switch (std::clamp(axis, 0, 2))
        {
        case 0:
            out.m[1][1] = c;
            out.m[1][2] = -s;
            out.m[2][1] = s;
            out.m[2][2] = c;
            break;
        case 1:
            out.m[0][0] = c;
            out.m[0][2] = s;
            out.m[2][0] = -s;
            out.m[2][2] = c;
            break;
        default:
            out.m[0][0] = c;
            out.m[0][1] = -s;
            out.m[1][0] = s;
            out.m[1][1] = c;
            break;
        }
        return out;
    }

    inline vr_vm_stabilize::Mat3x4 HooksNativeViewmodelHandsOnlyApplyThumbRootAdjust(
        VR* vr,
        int side,
        const vr_vm_stabilize::Mat3x4& local)
    {
        if (!vr || (side != -1 && side != 1))
            return local;

        vr_vm_stabilize::Mat3x4 adjusted = local;
        const Vector offset = (side < 0)
            ? vr->m_NativeViewmodelLeftHandOpenVRThumbRootOffsetUnits
            : vr->m_NativeViewmodelRightHandOpenVRThumbRootOffsetUnits;
        if (std::isfinite(offset.x) && std::isfinite(offset.y) && std::isfinite(offset.z))
        {
            adjusted.m[0][3] += offset.x;
            adjusted.m[1][3] += offset.y;
            adjusted.m[2][3] += offset.z;
        }

        const Vector rotation = (side < 0)
            ? vr->m_NativeViewmodelLeftHandOpenVRThumbRootRotationOffsetDeg
            : vr->m_NativeViewmodelRightHandOpenVRThumbRootRotationOffsetDeg;
        if (std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z))
        {
            constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float degrees = std::clamp(HooksVectorComponent(rotation, axis), -90.0f, 90.0f);
                if (std::fabs(degrees) <= 0.0001f)
                    continue;

                const vr_vm_stabilize::Mat3x4 delta =
                    HooksNativeViewmodelHandsOnlyMakeLocalAxisRotation(axis, degrees * kDegToRad);
                vr_vm_stabilize::Mat3x4 tmp{};
                vr_vm_stabilize::Mul(adjusted, delta, tmp);
                adjusted = tmp;
            }
        }

        return adjusted;
    }

    inline bool HooksNativeViewmodelHandsOnlyReadOpenVRLeftFingerCurls(
        VR* vr,
        std::array<float, 5>& outCurls)
    {
        outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        if (!vr || !vr->m_NativeViewmodelLeftHandOpenVRSkeleton ||
            !vr->GetNativeViewmodelLeftHandOpenVRFingerCurls(outCurls))
        {
            return false;
        }

        constexpr float kOpenVRThumbMinCurl = 0.0f;
        constexpr float kOpenVRThumbMaxCurl = 2.00f;
        constexpr float kOpenVRFingerMaxCurl = 2.00f;
        const float curlScale = std::clamp(vr->m_NativeViewmodelLeftHandOpenVRCurlScale, 0.0f, 2.0f);

        for (int finger = 0; finger < 5; ++finger)
        {
            const float baseCurl = std::clamp(outCurls[static_cast<size_t>(finger)], 0.0f, 1.0f) * curlScale;
            const float initialCurl =
                (finger < static_cast<int>(vr->m_NativeViewmodelLeftHandOpenVRInitialCurl.size()))
                ? vr->m_NativeViewmodelLeftHandOpenVRInitialCurl[static_cast<size_t>(finger)]
                : 0.0f;
            const float minCurl = (finger == 0) ? kOpenVRThumbMinCurl : 0.0f;
            const float maxCurl = (finger == 0) ? kOpenVRThumbMaxCurl : kOpenVRFingerMaxCurl;
            outCurls[static_cast<size_t>(finger)] = std::clamp(baseCurl + initialCurl, minCurl, maxCurl);
        }

        if (vr->m_MagazineInteractionLeftHandPoseActive.load(std::memory_order_relaxed) != 0)
        {
            static const float kMagazineGripMinCurl[5] =
            {
                kOpenVRThumbMinCurl, 0.60f, 0.66f, 0.68f, 0.68f,
            };
            static const float kMagazineGripMaxCurl[5] =
            {
                kOpenVRThumbMaxCurl,
                kOpenVRFingerMaxCurl,
                kOpenVRFingerMaxCurl,
                kOpenVRFingerMaxCurl,
                kOpenVRFingerMaxCurl,
            };
            for (int finger = 0; finger < 5; ++finger)
            {
                outCurls[static_cast<size_t>(finger)] = std::clamp(
                    outCurls[static_cast<size_t>(finger)],
                    kMagazineGripMinCurl[finger],
                    kMagazineGripMaxCurl[finger]);
            }
        }
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyReadOpenVRRightFingerCurls(
        VR* vr,
        std::array<float, 5>& outCurls)
    {
        outCurls = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        if (!vr || !vr->m_NativeViewmodelLeftHandOpenVRSkeleton)
            return false;

        const bool haveLiveCurls =
            vr->GetNativeViewmodelRightHandOpenVRFingerCurls(outCurls);
        const bool emptyHandsPlaceholderActive =
            vr->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
        if (!haveLiveCurls && !emptyHandsPlaceholderActive)
            return false;

        constexpr float kOpenVRThumbMaxCurl = 2.00f;
        constexpr float kOpenVRFingerMaxCurl = 2.00f;
        const float curlScale =
            std::clamp(vr->m_NativeViewmodelLeftHandOpenVRCurlScale, 0.0f, 2.0f);
        for (int finger = 0; finger < 5; ++finger)
        {
            const float baseCurl =
                std::clamp(outCurls[static_cast<size_t>(finger)], 0.0f, 1.0f) * curlScale;
            const float initialCurl =
                (finger < static_cast<int>(vr->m_NativeViewmodelLeftHandOpenVRInitialCurl.size()))
                ? vr->m_NativeViewmodelLeftHandOpenVRInitialCurl[static_cast<size_t>(finger)]
                : 0.0f;
            const float maxCurl = (finger == 0) ? kOpenVRThumbMaxCurl : kOpenVRFingerMaxCurl;
            outCurls[static_cast<size_t>(finger)] =
                std::clamp(baseCurl + initialCurl, 0.0f, maxCurl);
        }
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyApplyOpenVRFingerPose(
        VR* vr,
        void* drawState,
        int boneIndex,
        int stride,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* sourcePoseBones,
        vr_vm_stabilize::Mat3x4* currentBones)
    {
        const bool emptyHandsPlaceholderActive =
            vr && vr->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
        const bool applyLeftHand = keepSide.side == -1;
        const bool applyEmptyRightHand =
            keepSide.side == 1 && emptyHandsPlaceholderActive;
        if (!vr || !currentBones || (!applyLeftHand && !applyEmptyRightHand) ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }
        (void)boneIndex;
        (void)stride;

        std::array<float, 5> curls{};
        const bool haveCurls = applyLeftHand
            ? HooksNativeViewmodelHandsOnlyReadOpenVRLeftFingerCurls(vr, curls)
            : HooksNativeViewmodelHandsOnlyReadOpenVRRightFingerCurls(vr, curls);
        if (!haveCurls)
            return false;

        const float strength = std::clamp(vr->m_NativeViewmodelLeftHandOpenVRCurlStrength, 0.0f, 2.0f);
        const float direction =
            std::clamp(vr->m_NativeViewmodelLeftHandOpenVRCurlDirection, -1.0f, 1.0f);
        if (strength <= 0.0001f || std::fabs(direction) <= 0.0001f)
            return false;

        std::vector<uint8_t> driveMask;
        std::vector<uint8_t> hasAngle;
        std::vector<float> angleByBone;
        const int mappedSegments = HooksNativeViewmodelHandsOnlyBuildFingerDriveMask(
            boneNames,
            boneParents,
            numBones,
            keepSide.side,
            &curls,
            strength,
            direction,
            driveMask,
            &hasAngle,
            &angleByBone);
        if (mappedSegments <= 0)
            return false;

        std::vector<uint8_t> thumbRootMask;
        HooksNativeViewmodelHandsOnlyBuildThumbRootMask(
            boneNames,
            numBones,
            keepSide.side,
            thumbRootMask);

        std::vector<vr_vm_stabilize::Mat3x4> baseWorld(static_cast<size_t>(numBones));
        std::vector<vr_vm_stabilize::Mat3x4> baseLocal(static_cast<size_t>(numBones));
        const vr_vm_stabilize::Mat3x4* localPoseSource =
            (applyEmptyRightHand && sourcePoseBones) ? sourcePoseBones : currentBones;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!vr_vm_stabilize::SafeRead(
                localPoseSource + bone,
                baseWorld[static_cast<size_t>(bone)]) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(baseWorld[static_cast<size_t>(bone)]))
            {
                return false;
            }
        }

        for (int bone = 0; bone < numBones; ++bone)
        {
            const int parent = boneParents[static_cast<size_t>(bone)];
            if (parent >= 0 && parent < numBones)
            {
                vr_vm_stabilize::Mat3x4 inverseParent{};
                vr_vm_stabilize::InvertTR(baseWorld[static_cast<size_t>(parent)], inverseParent);
                vr_vm_stabilize::Mul(
                    inverseParent,
                    baseWorld[static_cast<size_t>(bone)],
                    baseLocal[static_cast<size_t>(bone)]);
            }
            else
            {
                baseLocal[static_cast<size_t>(bone)] = baseWorld[static_cast<size_t>(bone)];
            }
        }

        std::vector<uint8_t> resolved(static_cast<size_t>(numBones), 0u);
        int resolvedCount = 0;
        for (int pass = 0; pass < numBones && resolvedCount < numBones; ++pass)
        {
            bool progressed = false;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!driveMask[static_cast<size_t>(bone)] || resolved[static_cast<size_t>(bone)])
                    continue;

                const int parent = boneParents[static_cast<size_t>(bone)];
                if (parent >= 0 && parent < numBones &&
                    driveMask[static_cast<size_t>(parent)] &&
                    !resolved[static_cast<size_t>(parent)])
                {
                    continue;
                }

                vr_vm_stabilize::Mat3x4 local = baseLocal[static_cast<size_t>(bone)];
                const bool thumbRoot =
                    bone < static_cast<int>(thumbRootMask.size()) &&
                    thumbRootMask[static_cast<size_t>(bone)] != 0u;
                if (applyEmptyRightHand &&
                    !thumbRoot &&
                    hasAngle[static_cast<size_t>(bone)])
                {
                    vr_vm_stabilize::Mat3x4 restLocal{};
                    if (HooksNativeViewmodelHandsOnlyReadBoneRestLocalTransform(
                        drawState,
                        boneIndex,
                        stride,
                        bone,
                        restLocal))
                    {
                        restLocal.m[0][3] = local.m[0][3];
                        restLocal.m[1][3] = local.m[1][3];
                        restLocal.m[2][3] = local.m[2][3];
                        local = restLocal;
                    }
                }
                if (thumbRoot)
                {
                    local = HooksNativeViewmodelHandsOnlyApplyThumbRootAdjust(
                        vr,
                        keepSide.side,
                        local);
                }
                if (hasAngle[static_cast<size_t>(bone)])
                {
                    const vr_vm_stabilize::Mat3x4 rotation =
                        HooksNativeViewmodelHandsOnlyMakeLocalAxisRotation(
                            vr->m_NativeViewmodelLeftHandOpenVRCurlAxis,
                            angleByBone[static_cast<size_t>(bone)]);
                    vr_vm_stabilize::Mat3x4 adjusted{};
                    vr_vm_stabilize::Mul(local, rotation, adjusted);
                    local = adjusted;
                }

                if (parent >= 0 && parent < numBones)
                {
                    vr_vm_stabilize::Mat3x4 world{};
                    vr_vm_stabilize::Mul(currentBones[parent], local, world);
                    if (!HooksNativeViewmodelHandsOnlyMatrixFinite(world))
                        return false;
                    currentBones[bone] = world;
                }
                else
                {
                    if (!HooksNativeViewmodelHandsOnlyMatrixFinite(local))
                        return false;
                    currentBones[bone] = local;
                }

                resolved[static_cast<size_t>(bone)] = 1u;
                ++resolvedCount;
                progressed = true;
            }

            if (!progressed)
                break;
        }

        int applied = 0;
        for (uint8_t mask : driveMask)
        {
            if (mask)
                ++applied;
        }

        if (vr->m_VrHandsDebugLog)
        {
            static DWORD s_lastOpenVRLeftLogTick = 0;
            static DWORD s_lastOpenVRRightLogTick = 0;
            const DWORD now = GetTickCount();
            DWORD& lastLogTick =
                applyEmptyRightHand ? s_lastOpenVRRightLogTick : s_lastOpenVRLeftLogTick;
            if (now - lastLogTick >= 1000u)
            {
                lastLogTick = now;
                Game::logMsg(
                    "[VR][NativeHandsOnly] OpenVR %s fingers mapped=%d drivenBones=%d curl=(%.2f %.2f %.2f %.2f %.2f) axis=%d strength=%.2f dir=%.2f restUnlocked=%d",
                    applyEmptyRightHand ? "empty right" : "left",
                    mappedSegments,
                    applied,
                    curls[0],
                    curls[1],
                    curls[2],
                    curls[3],
                    curls[4],
                    vr->m_NativeViewmodelLeftHandOpenVRCurlAxis,
                    strength,
                    direction,
                    applyEmptyRightHand ? 1 : 0);
            }
        }

        return mappedSegments > 0;
    }

    inline float HooksNativeViewmodelHandsOnlyProjectionXScale(float horizontalFovDeg)
    {
        constexpr float kPi = 3.14159265358979323846f;
        const float fov = std::clamp(horizontalFovDeg, 1.0f, 179.0f) * kPi / 180.0f;
        return 1.0f / std::tan(fov * 0.5f);
    }

    inline float HooksNativeViewmodelHandsOnlyResolveAspect(float preferred, uint32_t width, uint32_t height)
    {
        if (preferred > 0.001f)
            return preferred;
        if (height > 0)
            return static_cast<float>(width) / static_cast<float>(height);
        return 1.0f;
    }

    inline bool HooksNativeViewmodelHandsOnlyResolveViewFrame(
        VR* vr,
        Vector& outViewOrigin,
        Vector& outForward,
        Vector& outRight,
        Vector& outUp)
    {
        if (!vr)
            return false;

        const Vector viewAngles = vr->GetViewAngle();
        const Vector viewLeft = vr->GetViewOriginLeft();
        const Vector viewRight = vr->GetViewOriginRight();

        QAngle eyeAngles(viewAngles.x, viewAngles.y, viewAngles.z);
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(eyeAngles, &forward, &right, &up);
        forward = VrHandMath::Normalize(forward);
        right = VrHandMath::Normalize(right);
        up = VrHandMath::Normalize(up);
        if (forward.Length() <= 0.0001f || right.Length() <= 0.0001f || up.Length() <= 0.0001f)
            return false;

        const Vector viewOrigin = (viewLeft + viewRight) * 0.5f;
        if (!std::isfinite(viewOrigin.x) || !std::isfinite(viewOrigin.y) || !std::isfinite(viewOrigin.z))
            return false;

        outViewOrigin = viewOrigin;
        outForward = forward;
        outRight = right;
        outUp = up;
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyReprojectScenePointToViewmodelLayer(
        VR* vr,
        const Vector& scenePoint,
        Vector& outViewmodelPoint)
    {
        if (!vr)
            return false;

        const float sceneFov = (vr->m_Fov > 0.001f) ? vr->m_Fov : 90.0f;
        const float viewmodelFov = sceneFov;
        const float sceneAspect = HooksNativeViewmodelHandsOnlyResolveAspect(
            vr->m_Aspect,
            vr->m_RenderWidth,
            vr->m_RenderHeight);
        const float viewmodelAspect = (vr->m_RenderHeight > 0)
            ? static_cast<float>(vr->m_RenderWidth) / static_cast<float>(vr->m_RenderHeight)
            : sceneAspect;

        const float sceneXScale = HooksNativeViewmodelHandsOnlyProjectionXScale(sceneFov);
        const float viewmodelXScale = HooksNativeViewmodelHandsOnlyProjectionXScale(viewmodelFov);
        const float sceneYScale = sceneXScale * sceneAspect;
        const float viewmodelYScale = viewmodelXScale * viewmodelAspect;
        if (!(viewmodelXScale > 0.0001f) || !(viewmodelYScale > 0.0001f))
            return false;

        Vector viewOrigin{};
        Vector forward{};
        Vector right{};
        Vector up{};
        if (!HooksNativeViewmodelHandsOnlyResolveViewFrame(vr, viewOrigin, forward, right, up))
            return false;

        const Vector delta = scenePoint - viewOrigin;
        const float viewX = VrHandMath::Dot(delta, right);
        const float viewY = VrHandMath::Dot(delta, up);
        const float viewZ = VrHandMath::Dot(delta, forward);
        if (!std::isfinite(viewX) || !std::isfinite(viewY) || !std::isfinite(viewZ))
            return false;

        outViewmodelPoint =
            viewOrigin +
            right * (viewX * (sceneXScale / viewmodelXScale)) +
            up * (viewY * (sceneYScale / viewmodelYScale)) +
            forward * viewZ;
        return true;
    }

    inline VrHandMatrix4 HooksNativeViewmodelHandsOnlyBuildLocalTransform(
        float sourceUnitsPerMeter,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = localRotationOffsetDeg.x * kDegToRad;
        const float ry = localRotationOffsetDeg.y * kDegToRad;
        const float rz = localRotationOffsetDeg.z * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);

        VrHandMatrix4 local = VrHandMath::Identity();
        VrHandMath::Set(local, 0, 0, cz * cy);
        VrHandMath::Set(local, 0, 1, cz * sy * sx - sz * cx);
        VrHandMath::Set(local, 0, 2, cz * sy * cx + sz * sx);
        VrHandMath::Set(local, 1, 0, sz * cy);
        VrHandMath::Set(local, 1, 1, sz * sy * sx + cz * cx);
        VrHandMath::Set(local, 1, 2, sz * sy * cx - cz * sx);
        VrHandMath::Set(local, 2, 0, -sy);
        VrHandMath::Set(local, 2, 1, cy * sx);
        VrHandMath::Set(local, 2, 2, cy * cx);
        VrHandMath::Set(local, 0, 3, localPositionOffsetMeters.x * sourceUnitsPerMeter);
        VrHandMath::Set(local, 1, 3, localPositionOffsetMeters.y * sourceUnitsPerMeter);
        VrHandMath::Set(local, 2, 3, localPositionOffsetMeters.z * sourceUnitsPerMeter);
        return local;
    }

    inline VrHandMatrix4 HooksNativeViewmodelHandsOnlyBuildControllerWorldFromAxes(
        const Vector& origin,
        const Vector& forwardIn,
        const Vector& rightIn,
        const Vector& upIn)
    {
        Vector forward = VrHandMath::Normalize(forwardIn);
        Vector right = VrHandMath::Normalize(rightIn);
        Vector up = VrHandMath::Normalize(upIn);
        if (forward.Length() <= 0.0001f)
            forward = Vector(1.0f, 0.0f, 0.0f);
        if (right.Length() <= 0.0001f)
            right = Vector(0.0f, -1.0f, 0.0f);
        if (up.Length() <= 0.0001f)
            up = Vector(0.0f, 0.0f, 1.0f);

        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, right.x);
        VrHandMath::Set(out, 1, 0, right.y);
        VrHandMath::Set(out, 2, 0, right.z);
        VrHandMath::Set(out, 0, 1, up.x);
        VrHandMath::Set(out, 1, 1, up.y);
        VrHandMath::Set(out, 2, 1, up.z);
        VrHandMath::Set(out, 0, 2, -forward.x);
        VrHandMath::Set(out, 1, 2, -forward.y);
        VrHandMath::Set(out, 2, 2, -forward.z);
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildLeftControllerTargetAnchor(
        VR* vr,
        vr_vm_stabilize::Mat3x4& outAnchor)
    {
        if (!vr || !vr->m_IsVREnabled)
            return false;

        Vector origin = vr->GetLeftControllerAbsPos();
        Vector reprojectedOrigin{};
        if (HooksNativeViewmodelHandsOnlyReprojectScenePointToViewmodelLayer(vr, origin, reprojectedOrigin))
            origin = reprojectedOrigin;

        const QAngle angles = vr->GetLeftControllerAbsAngle();
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(angles, &forward, &right, &up);

        const VrHandMatrix4 controllerWorld =
            HooksNativeViewmodelHandsOnlyBuildControllerWorldFromAxes(origin, forward, right, up);
        const VrHandMatrix4 localCorrection = HooksNativeViewmodelHandsOnlyBuildLocalTransform(
            vr->m_VRScale,
            vr->m_NativeViewmodelLeftHandPoseOffsetMeters,
            vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg);
        outAnchor = HooksVrHandMatrixToMat3x4(
            VrHandMath::Multiply(controllerWorld, localCorrection));
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outAnchor);
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildRightControllerWorldAnchor(
        VR* vr,
        vr_vm_stabilize::Mat3x4& outAnchor)
    {
        if (!vr || !vr->m_IsVREnabled)
            return false;

        Vector origin = vr->GetRightControllerAbsPos();
        Vector reprojectedOrigin{};
        if (HooksNativeViewmodelHandsOnlyReprojectScenePointToViewmodelLayer(
            vr,
            origin,
            reprojectedOrigin))
        {
            origin = reprojectedOrigin;
        }

        const QAngle angles = vr->GetRightControllerAbsAngle();
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(angles, &forward, &right, &up);

        const VrHandMatrix4 controllerWorld =
            HooksNativeViewmodelHandsOnlyBuildControllerWorldFromAxes(
                origin,
                forward,
                right,
                up);
        outAnchor = HooksVrHandMatrixToMat3x4(controllerWorld);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outAnchor);
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildCanonicalFreezeTargetAnchor(
        VR* vr,
        int side,
        vr_vm_stabilize::Mat3x4& outAnchor)
    {
        if (!vr || side == 0)
            return false;

        Vector viewOrigin{};
        Vector viewForward{};
        Vector viewRight{};
        Vector viewUp{};
        if (!HooksNativeViewmodelHandsOnlyResolveViewFrame(vr, viewOrigin, viewForward, viewRight, viewUp))
            return false;

        const Vector worldUp(0.0f, 0.0f, 1.0f);
        Vector horizontalForward = viewForward - worldUp * DotProduct(viewForward, worldUp);
        horizontalForward = HooksNormalizeVector(horizontalForward, Vector(1.0f, 0.0f, 0.0f));
        if (horizontalForward.Length() <= 0.0001f)
            return false;

        Vector horizontalRight = viewRight - worldUp * DotProduct(viewRight, worldUp);
        horizontalRight = HooksNormalizeVector(horizontalRight, Vector(0.0f, -1.0f, 0.0f));
        if (horizontalRight.Length() <= 0.0001f)
            return false;

        const float sourceUnitsPerMeter =
            (std::isfinite(vr->m_VRScale) && vr->m_VRScale > 0.001f) ? vr->m_VRScale : 50.0f;
        const Vector poseOffsetMeters = vr->m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters;
        const Vector origin =
            viewOrigin +
            horizontalForward * (sourceUnitsPerMeter * poseOffsetMeters.x) +
            horizontalRight * (sourceUnitsPerMeter * poseOffsetMeters.y * static_cast<float>(side)) +
            worldUp * (sourceUnitsPerMeter * poseOffsetMeters.z);

        const VrHandMatrix4 controllerWorld =
            HooksNativeViewmodelHandsOnlyBuildControllerWorldFromAxes(
                origin,
                horizontalForward,
                horizontalRight,
                worldUp);
        VrHandMatrix4 finalWorld = controllerWorld;
        const VrHandMatrix4 poseRotation = HooksNativeViewmodelHandsOnlyBuildLocalTransform(
            sourceUnitsPerMeter,
            Vector(0.0f, 0.0f, 0.0f),
            HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(vr, side));
        finalWorld = VrHandMath::Multiply(finalWorld, poseRotation);
        if (side < 0)
        {
            const VrHandMatrix4 localCorrection = HooksNativeViewmodelHandsOnlyBuildLocalTransform(
                sourceUnitsPerMeter,
                vr->m_NativeViewmodelLeftHandPoseOffsetMeters,
                vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg);
            finalWorld = VrHandMath::Multiply(finalWorld, localCorrection);
        }

        outAnchor = HooksVrHandMatrixToMat3x4(finalWorld);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outAnchor);
    }

    inline bool HooksNativeViewmodelHandsOnlyTryBuildCanonicalFreezeTargetDelta(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        vr_vm_stabilize::Mat3x4& outAnchor,
        vr_vm_stabilize::Mat3x4& outDelta)
    {
        if (!vr || !sourceBones || keepSide.side == 0 ||
            keepSide.hand < 0 || keepSide.hand >= numBones)
        {
            return false;
        }

        if (!HooksNativeViewmodelHandsOnlyBuildCanonicalFreezeTargetAnchor(vr, keepSide.side, outAnchor))
            return false;

        vr_vm_stabilize::Mat3x4 sourceAnchor{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + keepSide.hand, sourceAnchor) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(sourceAnchor))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 inverseSourceAnchor{};
        vr_vm_stabilize::InvertTR(sourceAnchor, inverseSourceAnchor);
        vr_vm_stabilize::Mul(outAnchor, inverseSourceAnchor, outDelta);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outDelta);
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildCanonicalFreezeWristPlaneWorld(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& canonicalAnchor,
        const vr_vm_stabilize::Mat3x4& canonicalDelta,
        float outPlane[4])
    {
        if (!vr || !sourceBones || !outPlane || keepSide.side == 0 ||
            numBones <= 0 || numBones > 512 ||
            keepSide.hand < 0 || keepSide.hand >= numBones ||
            keepSide.forearm < 0 || keepSide.forearm >= numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(canonicalAnchor) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(canonicalDelta))
        {
            return false;
        }

        auto readCanonicalBone = [&](int bone, vr_vm_stabilize::Mat3x4& outBone) -> bool
            {
                if (bone < 0 || bone >= numBones)
                    return false;

                vr_vm_stabilize::Mat3x4 source{};
                if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(source))
                {
                    return false;
                }

                vr_vm_stabilize::Mul(canonicalDelta, source, outBone);
                return HooksNativeViewmodelHandsOnlyMatrixFinite(outBone);
            };

        vr_vm_stabilize::Mat3x4 handBone{};
        vr_vm_stabilize::Mat3x4 forearmBone{};
        if (!readCanonicalBone(keepSide.hand, handBone) ||
            !readCanonicalBone(keepSide.forearm, forearmBone))
        {
            return false;
        }

        const Vector handPos = vr_vm_stabilize::GetOrigin(handBone);
        const Vector forearmPos = vr_vm_stabilize::GetOrigin(forearmBone);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(handPos) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(forearmPos))
        {
            return false;
        }

        Vector normal = handPos - forearmPos;
        Vector wristNormal = normal;
        if (keepSide.wrist >= 0 && keepSide.wrist < numBones)
        {
            vr_vm_stabilize::Mat3x4 wristBone{};
            if (readCanonicalBone(keepSide.wrist, wristBone))
            {
                const Vector candidate = handPos - vr_vm_stabilize::GetOrigin(wristBone);
                const float candidateLen = candidate.Length();
                if (std::isfinite(candidateLen) && candidateLen > 0.001f)
                    wristNormal = candidate;
            }
        }

        float wristLength = normal.Length();
        if (!std::isfinite(wristLength) || wristLength <= 0.001f)
            return false;

        normal = HooksNativeViewmodelHandsOnlyResolveArmBendNormal(
            vr,
            handBone,
            normal,
            wristNormal,
            keepSide.side,
            keepSide.cutRotationDeg);
        normal = HooksNormalizeVector(normal, handPos - forearmPos);
        if (normal.Length() <= 0.0001f)
            return false;

        const float trimDistance =
            HooksNativeViewmodelHandsOnlyResolveSideTrimDistance(vr, wristLength, keepSide.side);
        const float wristKeepDistance =
            HooksNativeViewmodelHandsOnlyResolveWristKeepDistance(vr, wristLength);
        const Vector planePoint = handPos + normal * (trimDistance - wristKeepDistance);
        outPlane[0] = normal.x;
        outPlane[1] = normal.y;
        outPlane[2] = normal.z;
        outPlane[3] = -DotProduct(normal, planePoint);
        return HooksNativeViewmodelHandsOnlyNormalizePlane(outPlane);
    }

    inline bool HooksNativeViewmodelHandsOnlyUseCanonicalFreezeLock(VR* vr)
    {
        return vr &&
            vr->m_NativeViewmodelHandsOnly &&
            !vr->IsVrHandsTwoHandedGripPoseActive() &&
            vr->m_NativeViewmodelHandsOnlyFreezePoseLock &&
            vr->m_NativeViewmodelLeftHandFreezePending &&
            vr->m_NativeViewmodelLeftHandFreezeReady.load(std::memory_order_acquire) == 0u;
    }

    inline bool HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLock(VR* vr)
    {
        return vr &&
            vr->m_NativeViewmodelHandsOnly &&
            vr->m_NativeViewmodelHandsOnlyFreezePoseLock &&
            (vr->m_NativeViewmodelLeftHandFreezePending ||
                vr->m_NativeViewmodelLeftHandFreezeReady.load(std::memory_order_acquire) != 0u);
    }

    inline bool HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(VR* vr, int side)
    {
        if (!HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLock(vr))
            return false;

        return !(side < 0 && vr && vr->IsVrHandsTwoHandedGripPoseActive());
    }

    struct HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock
    {
        VR* owner = nullptr;
        std::string modelName;
        uint32_t generation = 0;
        int numBones = 0;
        int side = 0;
        int hand = -1;
        int wrist = -1;
        int forearm = -1;
        bool valid = false;
        float armBendScale = 1.0f;
        Vector cutRotationDeg = Vector(0.0f, 0.0f, 0.0f);
        bool autoCanonicalCutNormal = false;
        Vector freezePoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
        Vector freezePoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
        Vector leftPoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
        Vector leftPoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
        vr_vm_stabilize::Mat3x4 targetAnchor{};
        float wristPlaneLocal[4]{};

        void Reset()
        {
            owner = nullptr;
            modelName.clear();
            generation = 0;
            numBones = 0;
            side = 0;
            hand = -1;
            wrist = -1;
            forearm = -1;
            valid = false;
            armBendScale = 1.0f;
            cutRotationDeg = Vector(0.0f, 0.0f, 0.0f);
            autoCanonicalCutNormal = false;
            freezePoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
            freezePoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
            leftPoseOffsetMeters = Vector(0.0f, 0.0f, 0.0f);
            leftPoseRotationOffsetDeg = Vector(0.0f, 0.0f, 0.0f);
            targetAnchor = vr_vm_stabilize::Mat3x4{};
            memset(wristPlaneLocal, 0, sizeof(wristPlaneLocal));
        }
    };

    inline std::mutex& HooksNativeViewmodelHandsOnlyFixedFreezePlaneMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    inline HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock&
        HooksNativeViewmodelHandsOnlyFixedFreezePlaneLockInstance(int side)
    {
        static HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock leftLock;
        static HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock rightLock;
        return (side < 0) ? leftLock : rightLock;
    }

    inline void HooksNativeViewmodelHandsOnlyResetFixedFreezePlaneLocks(VR* owner)
    {
        std::lock_guard<std::mutex> lock(HooksNativeViewmodelHandsOnlyFixedFreezePlaneMutex());
        HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock& leftLock =
            HooksNativeViewmodelHandsOnlyFixedFreezePlaneLockInstance(-1);
        HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock& rightLock =
            HooksNativeViewmodelHandsOnlyFixedFreezePlaneLockInstance(1);
        if (!owner || leftLock.owner == owner)
            leftLock.Reset();
        if (!owner || rightLock.owner == owner)
            rightLock.Reset();
    }

    inline bool HooksNativeViewmodelHandsOnlyVectorNearlyEqual(
        const Vector& a,
        const Vector& b,
        float maxLengthSqr)
    {
        return (a - b).LengthSqr() <= maxLengthSqr;
    }

    inline bool HooksNativeViewmodelHandsOnlyFixedFreezePlaneLockMatches(
        const HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock& state,
        VR* vr,
        int side,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const std::string& lowerModel,
        int numBones,
        uint32_t generation)
    {
        if (!state.valid ||
            state.owner != vr ||
            state.modelName != lowerModel ||
            state.generation != generation ||
            state.side != side ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(state.targetAnchor) ||
            !HooksNativeViewmodelHandsOnlyPlaneFinite(state.wristPlaneLocal))
        {
            return false;
        }

        const float armBendScale = std::clamp(
            vr ? vr->m_NativeViewmodelHandsOnlyArmBendScale : 1.0f,
            0.0f,
            1.0f);
        const bool configMatches =
            std::fabs(state.armBendScale - armBendScale) <= 0.0001f &&
            HooksNativeViewmodelHandsOnlyVectorNearlyEqual(
                state.cutRotationDeg,
                keepSide.cutRotationDeg,
                0.0001f) &&
            HooksNativeViewmodelHandsOnlyVectorNearlyEqual(
                state.freezePoseOffsetMeters,
                vr->m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters,
                0.000001f) &&
            HooksNativeViewmodelHandsOnlyVectorNearlyEqual(
                state.freezePoseRotationOffsetDeg,
                HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(vr, side),
                0.0001f) &&
            HooksNativeViewmodelHandsOnlyVectorNearlyEqual(
                state.leftPoseOffsetMeters,
                vr->m_NativeViewmodelLeftHandPoseOffsetMeters,
                0.000001f) &&
            HooksNativeViewmodelHandsOnlyVectorNearlyEqual(
                state.leftPoseRotationOffsetDeg,
                vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg,
                0.0001f);
        if (!configMatches)
            return false;

        return state.numBones == numBones &&
            state.hand == keepSide.hand &&
            state.wrist == keepSide.wrist &&
            state.forearm == keepSide.forearm &&
            state.autoCanonicalCutNormal == keepSide.autoCanonicalCutNormal;
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildDeltaToTargetAnchor(
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const vr_vm_stabilize::Mat3x4& targetAnchor,
        vr_vm_stabilize::Mat3x4& outDelta)
    {
        if (!sourceBones ||
            keepSide.hand < 0 ||
            keepSide.hand >= numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 sourceAnchor{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + keepSide.hand, sourceAnchor) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(sourceAnchor))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 inverseSourceAnchor{};
        vr_vm_stabilize::InvertTR(sourceAnchor, inverseSourceAnchor);
        vr_vm_stabilize::Mul(targetAnchor, inverseSourceAnchor, outDelta);
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outDelta);
    }

    inline bool HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const std::string& lowerModel,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        vr_vm_stabilize::Mat3x4* outTargetAnchor,
        vr_vm_stabilize::Mat3x4* outTargetDelta,
        float outPlane[4],
        const vr_vm_stabilize::Mat3x4* planeAnchor)
    {
        if (!HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, keepSide.side) ||
            keepSide.side == 0 ||
            keepSide.hand < 0 ||
            keepSide.hand >= numBones ||
            !sourceBones ||
            numBones <= 0 ||
            numBones > 512)
        {
            return false;
        }

        const uint32_t generation =
            vr->m_NativeViewmodelHandsOnlyFreezePlaneGeneration.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(HooksNativeViewmodelHandsOnlyFixedFreezePlaneMutex());
        HooksNativeViewmodelHandsOnlyFixedFreezePlaneLock& state =
            HooksNativeViewmodelHandsOnlyFixedFreezePlaneLockInstance(keepSide.side);
        if (state.owner != vr)
            state.Reset();

        if (!HooksNativeViewmodelHandsOnlyFixedFreezePlaneLockMatches(
            state,
            vr,
            keepSide.side,
            keepSide,
            lowerModel,
            numBones,
            generation))
        {
            state.Reset();

            vr_vm_stabilize::Mat3x4 targetAnchor{};
            if (!HooksNativeViewmodelHandsOnlyBuildCanonicalFreezeTargetAnchor(
                vr,
                keepSide.side,
                targetAnchor))
            {
                return false;
            }

            vr_vm_stabilize::Mat3x4 targetDelta{};
            if (!HooksNativeViewmodelHandsOnlyBuildDeltaToTargetAnchor(
                keepSide,
                sourceBones,
                numBones,
                targetAnchor,
                targetDelta))
            {
                return false;
            }

            float fixedPlaneLocal[4]{};
            bool haveLocalPlane = false;
            if (keepSide.deterministicWristPlaneLocalValid &&
                HooksNativeViewmodelHandsOnlyPlaneFinite(keepSide.deterministicWristPlaneLocal))
            {
                memcpy(fixedPlaneLocal, keepSide.deterministicWristPlaneLocal, sizeof(fixedPlaneLocal));
                haveLocalPlane =
                    HooksNativeViewmodelHandsOnlyNormalizePlane(fixedPlaneLocal) &&
                    HooksNativeViewmodelHandsOnlyPlaneFinite(fixedPlaneLocal);
            }

            if (!haveLocalPlane)
            {
                float fixedPlaneWorld[4]{};
                bool havePlane =
                    HooksNativeViewmodelHandsOnlyBuildCanonicalFreezeWristPlaneWorld(
                        vr,
                        keepSide,
                        sourceBones,
                        numBones,
                        targetAnchor,
                        targetDelta,
                        fixedPlaneWorld);
                if (!havePlane)
                {
                    memcpy(fixedPlaneWorld, keepSide.wristPlaneWorld, sizeof(fixedPlaneWorld));
                    havePlane =
                        HooksNativeViewmodelHandsOnlyReanchorPlaneToTargetHand(
                            targetAnchor,
                            keepSide.handPos,
                            fixedPlaneWorld);
                }
                if (!havePlane ||
                    !HooksNativeViewmodelHandsOnlyNormalizePlane(fixedPlaneWorld) ||
                    !HooksNativeViewmodelHandsOnlyPlaneFinite(fixedPlaneWorld))
                {
                    return false;
                }

                vr_vm_stabilize::Mat3x4 inverseTargetAnchor{};
                vr_vm_stabilize::InvertTR(targetAnchor, inverseTargetAnchor);
                if (!HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
                    inverseTargetAnchor,
                    fixedPlaneWorld,
                    fixedPlaneLocal))
                {
                    return false;
                }
            }

            state.owner = vr;
            state.modelName = lowerModel;
            state.generation = generation;
            state.numBones = numBones;
            state.side = keepSide.side;
            state.hand = keepSide.hand;
            state.wrist = keepSide.wrist;
            state.forearm = keepSide.forearm;
            state.valid = true;
            state.armBendScale = std::clamp(vr->m_NativeViewmodelHandsOnlyArmBendScale, 0.0f, 1.0f);
            state.cutRotationDeg = keepSide.cutRotationDeg;
            state.autoCanonicalCutNormal = keepSide.autoCanonicalCutNormal;
            state.freezePoseOffsetMeters = vr->m_NativeViewmodelHandsOnlyFreezePoseOffsetMeters;
            state.freezePoseRotationOffsetDeg =
                HooksNativeViewmodelHandsOnlyResolveFreezePoseRotationOffsetDeg(vr, keepSide.side);
            state.leftPoseOffsetMeters = vr->m_NativeViewmodelLeftHandPoseOffsetMeters;
            state.leftPoseRotationOffsetDeg = vr->m_NativeViewmodelLeftHandPoseRotationOffsetDeg;
            state.targetAnchor = targetAnchor;
            memcpy(state.wristPlaneLocal, fixedPlaneLocal, sizeof(state.wristPlaneLocal));
        }

        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(state.targetAnchor) ||
            !HooksNativeViewmodelHandsOnlyPlaneFinite(state.wristPlaneLocal))
        {
            state.Reset();
            return false;
        }

        if (outTargetAnchor)
            *outTargetAnchor = state.targetAnchor;
        if (outTargetDelta)
        {
            if (!HooksNativeViewmodelHandsOnlyBuildDeltaToTargetAnchor(
                keepSide,
                sourceBones,
                numBones,
                state.targetAnchor,
                *outTargetDelta))
            {
                return false;
            }
        }
        if (outPlane)
        {
            const vr_vm_stabilize::Mat3x4& resolvedPlaneAnchor =
                planeAnchor ? *planeAnchor : state.targetAnchor;
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(resolvedPlaneAnchor) ||
                !HooksNativeViewmodelHandsOnlyTransformPlaneByMatrix(
                    resolvedPlaneAnchor,
                    state.wristPlaneLocal,
                    outPlane))
            {
                return false;
            }
        }
        return true;
    }

    inline void HooksNativeViewmodelHandsOnlyTransformPlane(
        const vr_vm_stabilize::Mat3x4& delta,
        float plane[4])
    {
        if (!plane)
            return;

        Vector normal(plane[0], plane[1], plane[2]);
        float normalLen = normal.Length();
        if (!std::isfinite(normalLen) || normalLen < 0.001f)
            return;
        normal *= (1.0f / normalLen);
        const Vector point = normal * (-plane[3] / normalLen);

        Vector movedPoint = HooksTransformPoint(delta, point);
        Vector movedNormal = HooksTransformVector(delta, normal);
        const float movedNormalLen = movedNormal.Length();
        if (!std::isfinite(movedNormalLen) || movedNormalLen < 0.001f)
            return;
        movedNormal *= (1.0f / movedNormalLen);

        plane[0] = movedNormal.x;
        plane[1] = movedNormal.y;
        plane[2] = movedNormal.z;
        plane[3] = -DotProduct(movedNormal, movedPoint);
        HooksNativeViewmodelHandsOnlyNormalizePlane(plane);
    }

    inline bool HooksNativeViewmodelHandsOnlyReanchorPlaneToTargetHand(
        const vr_vm_stabilize::Mat3x4& targetAnchor,
        const Vector& sourceHandPos,
        float plane[4])
    {
        if (!plane ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(sourceHandPos))
        {
            return false;
        }

        if (!HooksNativeViewmodelHandsOnlyNormalizePlane(plane))
            return false;

        Vector normal(plane[0], plane[1], plane[2]);
        const float normalLen = normal.Length();
        if (!std::isfinite(normalLen) || normalLen < 0.001f)
            return false;
        normal *= (1.0f / normalLen);

        const Vector targetHandPos = vr_vm_stabilize::GetOrigin(targetAnchor);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(targetHandPos))
            return false;

        const float sourceSignedDistance = DotProduct(normal, sourceHandPos) + plane[3];
        if (!std::isfinite(sourceSignedDistance))
            return false;

        plane[0] = normal.x;
        plane[1] = normal.y;
        plane[2] = normal.z;
        plane[3] = sourceSignedDistance - DotProduct(normal, targetHandPos);
        return HooksNativeViewmodelHandsOnlyNormalizePlane(plane);
    }

    inline bool HooksNativeViewmodelHandsOnlyTryBuildSideTargetDelta(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        vr_vm_stabilize::Mat3x4& outDelta,
        vr_vm_stabilize::Mat3x4* outTargetAnchor = nullptr)
    {
        const bool emptyHandsPlaceholderActive =
            vr && vr->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
        const bool buildLeftControllerTarget = keepSide.side == -1;
        const bool buildEmptyRightControllerTarget =
            keepSide.side == 1 && emptyHandsPlaceholderActive;
        if (!vr || !sourceBones ||
            (!buildLeftControllerTarget && !buildEmptyRightControllerTarget) ||
            keepSide.hand < 0 || keepSide.hand >= numBones)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 sourceAnchor{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + keepSide.hand, sourceAnchor) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(sourceAnchor))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 targetAnchor{};
        if (buildLeftControllerTarget)
        {
            if (!HooksNativeViewmodelHandsOnlyBuildLeftControllerTargetAnchor(vr, targetAnchor))
                return false;
        }
        else
        {
            vr_vm_stabilize::Mat3x4 controllerAnchor{};
            if (!HooksNativeViewmodelHandsOnlyBuildRightControllerWorldAnchor(
                vr,
                controllerAnchor))
            {
                return false;
            }

            struct RightControllerHandCalibration
            {
                VR* owner = nullptr;
                uint32_t generation = 0;
                int numBones = 0;
                int handBone = -1;
                bool valid = false;
                vr_vm_stabilize::Mat3x4 controllerToHand{};
            };
            static std::mutex s_rightControllerHandCalibrationMutex;
            static RightControllerHandCalibration s_rightControllerHandCalibration;
            std::lock_guard<std::mutex> lock(s_rightControllerHandCalibrationMutex);

            const uint32_t generation =
                vr->m_NativeViewmodelLeftHandFreezeGeneration.load(std::memory_order_acquire);
            const bool calibrationMatches =
                s_rightControllerHandCalibration.valid &&
                s_rightControllerHandCalibration.owner == vr &&
                s_rightControllerHandCalibration.generation == generation &&
                s_rightControllerHandCalibration.numBones == numBones &&
                s_rightControllerHandCalibration.handBone == keepSide.hand;
            if (!calibrationMatches)
            {
                vr_vm_stabilize::Mat3x4 inverseController{};
                vr_vm_stabilize::InvertTR(controllerAnchor, inverseController);
                vr_vm_stabilize::Mul(
                    inverseController,
                    sourceAnchor,
                    s_rightControllerHandCalibration.controllerToHand);
                // Preserve only the model-specific hand-axis correction. The stock
                // pistol viewmodel also carries a downward hand translation; keeping
                // that translation would place the empty hand below the controller.
                s_rightControllerHandCalibration.controllerToHand.m[0][3] = 0.0f;
                s_rightControllerHandCalibration.controllerToHand.m[1][3] = 0.0f;
                s_rightControllerHandCalibration.controllerToHand.m[2][3] = 0.0f;
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    s_rightControllerHandCalibration.controllerToHand))
                {
                    s_rightControllerHandCalibration = RightControllerHandCalibration{};
                    return false;
                }

                s_rightControllerHandCalibration.owner = vr;
                s_rightControllerHandCalibration.generation = generation;
                s_rightControllerHandCalibration.numBones = numBones;
                s_rightControllerHandCalibration.handBone = keepSide.hand;
                s_rightControllerHandCalibration.valid = true;
                if (vr->m_VrHandsDebugLog)
                {
                    Game::logMsg(
                        "[VR][NativeHandsOnly] calibrated empty right hand directly to controller bone hand=%d bones=%d generation=%u",
                        keepSide.hand,
                        numBones,
                        generation);
                }
            }

            const VrHandMatrix4 poseCorrection =
                HooksNativeViewmodelHandsOnlyBuildLocalTransform(
                    vr->m_VRScale,
                    vr->m_NativeViewmodelRightHandPoseOffsetMeters,
                    vr->m_NativeViewmodelRightHandPoseRotationOffsetDeg);
            const vr_vm_stabilize::Mat3x4 poseCorrectionMat =
                HooksVrHandMatrixToMat3x4(poseCorrection);
            vr_vm_stabilize::Mat3x4 adjustedControllerAnchor{};
            vr_vm_stabilize::Mul(
                controllerAnchor,
                poseCorrectionMat,
                adjustedControllerAnchor);
            vr_vm_stabilize::Mul(
                adjustedControllerAnchor,
                s_rightControllerHandCalibration.controllerToHand,
                targetAnchor);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor))
                return false;
        }

        vr_vm_stabilize::Mat3x4 inverseSourceAnchor{};
        vr_vm_stabilize::InvertTR(sourceAnchor, inverseSourceAnchor);
        vr_vm_stabilize::Mul(targetAnchor, inverseSourceAnchor, outDelta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(outDelta))
            return false;

        if (outTargetAnchor)
            *outTargetAnchor = targetAnchor;
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildIsolatedSideBones(
        VR* vr,
        void* drawState,
        int boneIndex,
        int stride,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        const HooksNativeViewmodelHandsOnlySideInfo& keepSide,
        vr_vm_stabilize::Mat3x4*& outBones,
        float* inOutWristPlaneWorld)
    {
        outBones = nullptr;
        if (!vr || !sourceBones || numBones <= 0 || numBones > 512 ||
            keepSide.side == 0 || keepSide.hand < 0 || keepSide.hand >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
        seqEven &= ~1u;
        if (seqEven == 0)
            seqEven = 2;

        vr_vm_stabilize::Mat3x4* isolated = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!isolated)
            return false;

        const float wristKeepDistance = std::clamp(keepSide.wristKeepDistance, 0.0f, 16.0f);

        int keptBones = 0;
        std::vector<uint8_t> keepMask(static_cast<size_t>(numBones), 0u);
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (HooksNativeViewmodelHandsOnlyShouldKeepBone(
                boneNames,
                boneParents,
                numBones,
                sourceBones,
                keepSide,
                wristKeepDistance,
                bone))
            {
                keepMask[static_cast<size_t>(bone)] = 1u;
                ++keptBones;
            }
        }
        if (keptBones <= 0)
            return false;

        vr_vm_stabilize::Mat3x4 targetDelta = vr_vm_stabilize::Identity();
        vr_vm_stabilize::Mat3x4 targetAnchor{};
        bool useTargetDelta = false;
        bool haveTargetAnchor = false;

        float workingWristPlaneWorld[4] = {
            keepSide.wristPlaneWorld[0],
            keepSide.wristPlaneWorld[1],
            keepSide.wristPlaneWorld[2],
            keepSide.wristPlaneWorld[3],
        };

        const bool useFixedFreezePlaneLock =
            HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, keepSide.side);
        const bool useCanonicalFreezeLock =
            HooksNativeViewmodelHandsOnlyUseCanonicalFreezeLock(vr);
        const bool keepSourceViewmodelAnimation =
            keepSide.side == -1 && vr->IsVrHandsTwoHandedGripPoseActive();
        if (useFixedFreezePlaneLock)
        {
            if (useCanonicalFreezeLock)
            {
                useTargetDelta = HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                    vr,
                    keepSide,
                    lowerModel,
                    sourceBones,
                    numBones,
                    &targetAnchor,
                    &targetDelta,
                    workingWristPlaneWorld);
                haveTargetAnchor = useTargetDelta;
            }
            else
            {
                useTargetDelta = false;
                haveTargetAnchor = false;
            }

            if (useCanonicalFreezeLock && inOutWristPlaneWorld)
                memcpy(inOutWristPlaneWorld, workingWristPlaneWorld, sizeof(workingWristPlaneWorld));
        }
        else if (useCanonicalFreezeLock)
        {
            useTargetDelta = HooksNativeViewmodelHandsOnlyTryBuildCanonicalFreezeTargetDelta(
                vr,
                keepSide,
                sourceBones,
                numBones,
                targetAnchor,
                targetDelta);
            haveTargetAnchor = useTargetDelta;
            if (useTargetDelta)
            {
                bool haveCanonicalPlane =
                    HooksNativeViewmodelHandsOnlyBuildCanonicalFreezeWristPlaneWorld(
                        vr,
                        keepSide,
                        sourceBones,
                        numBones,
                        targetAnchor,
                        targetDelta,
                        workingWristPlaneWorld);
                if (!haveCanonicalPlane)
                {
                    workingWristPlaneWorld[0] = keepSide.wristPlaneWorld[0];
                    workingWristPlaneWorld[1] = keepSide.wristPlaneWorld[1];
                    workingWristPlaneWorld[2] = keepSide.wristPlaneWorld[2];
                    workingWristPlaneWorld[3] = keepSide.wristPlaneWorld[3];
                    haveCanonicalPlane =
                        HooksNativeViewmodelHandsOnlyReanchorPlaneToTargetHand(
                            targetAnchor,
                            keepSide.handPos,
                            workingWristPlaneWorld);
                }
                if (haveCanonicalPlane && inOutWristPlaneWorld)
                    memcpy(inOutWristPlaneWorld, workingWristPlaneWorld, sizeof(workingWristPlaneWorld));
            }
        }
        if (useFixedFreezePlaneLock && !useCanonicalFreezeLock)
        {
            if (!useTargetDelta)
            {
                useTargetDelta = HooksNativeViewmodelHandsOnlyTryBuildSideTargetDelta(
                    vr,
                    keepSide,
                    sourceBones,
                    numBones,
                    targetDelta,
                    &targetAnchor);
                haveTargetAnchor = useTargetDelta;
            }
            if (!haveTargetAnchor)
            {
                if (vr_vm_stabilize::SafeRead(sourceBones + keepSide.hand, targetAnchor) &&
                    HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor))
                {
                    haveTargetAnchor = true;
                }
            }
            if (haveTargetAnchor)
            {
                float lockedWorldPlane[4]{};
                if (HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                    vr,
                    keepSide,
                    lowerModel,
                    sourceBones,
                    numBones,
                    nullptr,
                    nullptr,
                    lockedWorldPlane,
                    &targetAnchor))
                {
                    memcpy(workingWristPlaneWorld, lockedWorldPlane, sizeof(workingWristPlaneWorld));
                    if (inOutWristPlaneWorld)
                        memcpy(inOutWristPlaneWorld, lockedWorldPlane, sizeof(lockedWorldPlane));
                }
            }
        }

        const bool haveWorkingWristPlane =
            HooksNativeViewmodelHandsOnlyNormalizePlane(workingWristPlaneWorld);
        Vector normal(
            workingWristPlaneWorld[0],
            workingWristPlaneWorld[1],
            workingWristPlaneWorld[2]);
        float normalLen = normal.Length();
        if (!haveWorkingWristPlane || !std::isfinite(normalLen) || normalLen < 0.001f)
        {
            normal = keepSide.anchorPos - keepSide.forearmPos;
            normalLen = normal.Length();
        }
        if (!std::isfinite(normalLen) || normalLen < 0.001f)
            return false;
        normal *= (1.0f / normalLen);

        Vector planePoint = keepSide.anchorPos - (normal * wristKeepDistance);
        if (haveWorkingWristPlane)
        {
            const Vector referencePoint =
                haveTargetAnchor ? vr_vm_stabilize::GetOrigin(targetAnchor) : keepSide.handPos;
            if (HooksNativeViewmodelHandsOnlyVectorFinite(referencePoint))
            {
                const float signedDistance =
                    DotProduct(normal, referencePoint) + workingWristPlaneWorld[3];
                if (std::isfinite(signedDistance))
                    planePoint = referencePoint - (normal * signedDistance);
            }
        }
        const float wristLength = (keepSide.anchorPos - keepSide.forearmPos).Length();
        const float hideBehindDistance = std::clamp(
            (std::isfinite(wristLength) && wristLength > 0.001f) ? (wristLength * 2.0f) : 32.0f,
            24.0f,
            96.0f);
        const Vector hiddenOrigin = planePoint - (normal * hideBehindDistance);
        const vr_vm_stabilize::Mat3x4 hiddenBone =
            HooksNativeViewmodelHandsOnlyCollapsedBoneAt(hiddenOrigin);

        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return false;

            const vr_vm_stabilize::Mat3x4& selected =
                keepMask[static_cast<size_t>(bone)] ? source : hiddenBone;
            isolated[bone] = selected;
        }

        if (!useTargetDelta && !keepSourceViewmodelAnimation)
        {
            useTargetDelta = HooksNativeViewmodelHandsOnlyTryBuildSideTargetDelta(
                vr,
                keepSide,
                isolated,
                numBones,
                targetDelta,
                &targetAnchor);
            haveTargetAnchor = useTargetDelta;
        }
        if (!haveTargetAnchor)
        {
            if (vr_vm_stabilize::SafeRead(isolated + keepSide.hand, targetAnchor) &&
                HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchor))
            {
                haveTargetAnchor = true;
            }
        }
        if (!useFixedFreezePlaneLock &&
            !useCanonicalFreezeLock &&
            useTargetDelta &&
            inOutWristPlaneWorld)
        {
            HooksNativeViewmodelHandsOnlyReanchorPlaneToTargetHand(
                targetAnchor,
                keepSide.handPos,
                inOutWristPlaneWorld);
        }
        else if (useFixedFreezePlaneLock &&
            !useCanonicalFreezeLock &&
            haveTargetAnchor &&
            inOutWristPlaneWorld)
        {
            float lockedWorldPlane[4]{};
            if (HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                vr,
                keepSide,
                lowerModel,
                sourceBones,
                numBones,
                nullptr,
                nullptr,
                lockedWorldPlane,
                &targetAnchor))
            {
                memcpy(inOutWristPlaneWorld, lockedWorldPlane, sizeof(lockedWorldPlane));
            }
        }

        if (useTargetDelta)
        {
            for (int bone = 0; bone < numBones; ++bone)
            {
                vr_vm_stabilize::Mat3x4 transformed{};
                vr_vm_stabilize::Mul(targetDelta, isolated[bone], transformed);
                isolated[bone] = transformed;
            }
        }

        bool appliedFrozenPose = false;
        if (haveTargetAnchor && !keepSourceViewmodelAnimation)
        {
            appliedFrozenPose =
                HooksNativeViewmodelHandsOnlyApplyFrozenSideHandPose(
                    vr,
                    drawState,
                    boneIndex,
                    stride,
                    lowerModel,
                    boneNames,
                    boneParents,
                    numBones,
                    keepSide,
                    sourceBones,
                    targetAnchor,
                    targetDelta,
                    isolated,
                    inOutWristPlaneWorld);
        }

        if (!keepSourceViewmodelAnimation)
        {
            HooksNativeViewmodelHandsOnlyApplyOpenVRFingerPose(
                vr,
                drawState,
                boneIndex,
                stride,
                boneNames,
                boneParents,
                numBones,
                keepSide,
                sourceBones,
                isolated);
        }

        HooksNativeViewmodelHandsOnlyLogIsolationDebug(
            vr,
            lowerModel,
            boneNames,
            boneParents,
            numBones,
            sourceBones,
            keepSide,
            keepMask,
            keptBones,
            workingWristPlaneWorld,
            planePoint,
            normal,
            hiddenOrigin,
            useTargetDelta,
            haveTargetAnchor,
            targetDelta,
            targetAnchor,
            useFixedFreezePlaneLock,
            useCanonicalFreezeLock,
            appliedFrozenPose,
            isolated);

        if (keepSide.side == -1)
        {
            HooksNativeViewmodelHandsOnlyPublishLeftWristAnchor(
                vr,
                boneNames,
                numBones,
                isolated);
        }

        outBones = isolated;
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildSideInfo(
        VR* vr,
        void* drawState,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        int boneIndex,
        int stride,
        const vr_vm_stabilize::Mat3x4* bones,
        const std::string& lowerModel,
        int side,
        HooksNativeViewmodelHandsOnlySideInfo& outInfo)
    {
        outInfo = HooksNativeViewmodelHandsOnlySideInfo{};
        outInfo.side = side;
        if (!vr || !drawState || !bones || side == 0)
            return false;

        const std::vector<const char*> handNeedles = {
            "bip01_r_hand", "_r_hand", ".r.hand", "r.hand",
            "bip01_l_hand", "_l_hand", ".l.hand", "l.hand",
        };
        const std::vector<const char*> wristNeedles = {
            "bip01_r_wrist", "_r_wrist", ".r.wrist", "r.wrist",
            "bip01_l_wrist", "_l_wrist", ".l.wrist", "l.wrist",
        };
        if (!HooksNativeViewmodelHandsOnlyFindNamedBone(boneNames, handNeedles, side, outInfo.hand) ||
            !HooksNativeViewmodelHandsOnlyFindBestForearmBone(
                boneNames,
                boneParents,
                numBones,
                outInfo.hand,
                side,
                outInfo.forearm))
        {
            return false;
        }

        if (outInfo.hand < 0 || outInfo.hand >= numBones || outInfo.forearm < 0 || outInfo.forearm >= numBones)
            return false;
        HooksNativeViewmodelHandsOnlyFindBestWristBone(
            boneNames,
            boneParents,
            numBones,
            outInfo.hand,
            wristNeedles,
            side,
            outInfo.wrist);

        vr_vm_stabilize::Mat3x4 handBone{};
        vr_vm_stabilize::Mat3x4 forearmBone{};
        if (!vr_vm_stabilize::SafeRead(bones + outInfo.hand, handBone) ||
            !vr_vm_stabilize::SafeRead(bones + outInfo.forearm, forearmBone))
        {
            return false;
        }

        outInfo.handPos = vr_vm_stabilize::GetOrigin(handBone);
        outInfo.anchorPos = outInfo.handPos;
        outInfo.forearmPos = vr_vm_stabilize::GetOrigin(forearmBone);

        Vector normal = outInfo.anchorPos - outInfo.forearmPos;
        Vector wristNormal = normal;
        if (outInfo.wrist >= 0 && outInfo.wrist < numBones)
        {
            vr_vm_stabilize::Mat3x4 wristBone{};
            if (vr_vm_stabilize::SafeRead(bones + outInfo.wrist, wristBone))
            {
                const Vector candidate = outInfo.anchorPos - vr_vm_stabilize::GetOrigin(wristBone);
                const float candidateLen = candidate.Length();
                if (std::isfinite(candidateLen) && candidateLen > 0.001f)
                    wristNormal = candidate;
            }
        }
        float len = normal.Length();
        if (!std::isfinite(len) || len < 0.001f)
        {
            outInfo.anchorPos = outInfo.handPos;
            normal = outInfo.anchorPos - outInfo.forearmPos;
            wristNormal = normal;
            len = normal.Length();
        }
        if (!std::isfinite(len) || len < 0.001f)
            return false;
        bool explicitCutRotation =
            HooksNativeViewmodelHandsOnlyTryResolveExplicitCutRotationDeg(
                vr,
                side,
                &lowerModel,
                outInfo.cutRotationDeg);
        if (!explicitCutRotation)
            outInfo.cutRotationDeg = HooksNativeViewmodelHandsOnlyBaseCutRotationDeg(vr, side);

        normal = HooksNativeViewmodelHandsOnlyResolveArmBendNormalForRig(
            vr,
            handBone,
            normal,
            wristNormal,
            side,
            lowerModel,
            outInfo.cutRotationDeg,
            !explicitCutRotation,
            &outInfo.autoCanonicalCutNormal);

        const float trimDistance =
            HooksNativeViewmodelHandsOnlyResolveSideTrimDistance(vr, len, side);
        outInfo.anchorPos = outInfo.handPos + (normal * trimDistance);

        float wristKeepDistance = HooksNativeViewmodelHandsOnlyResolveWristKeepDistance(vr, len);
        outInfo.wristKeepDistance = wristKeepDistance;
        const Vector planePoint = outInfo.anchorPos - (normal * wristKeepDistance);
        outInfo.wristPlaneWorld[0] = normal.x;
        outInfo.wristPlaneWorld[1] = normal.y;
        outInfo.wristPlaneWorld[2] = normal.z;
        outInfo.wristPlaneWorld[3] = -DotProduct(normal, planePoint);
        if (!HooksNativeViewmodelHandsOnlyNormalizePlane(outInfo.wristPlaneWorld))
            return false;

        if (outInfo.autoCanonicalCutNormal)
        {
            outInfo.deterministicWristPlaneLocalValid =
                HooksNativeViewmodelHandsOnlyBuildPlaneLocalToAnchor(
                    handBone,
                    outInfo.wristPlaneWorld,
                    outInfo.deterministicWristPlaneLocal);
        }
        else
        {
            outInfo.deterministicWristPlaneLocalValid =
                HooksNativeViewmodelHandsOnlyBuildDeterministicWristPlaneLocal(
                    vr,
                    drawState,
                    boneParents,
                    numBones,
                    boneIndex,
                    stride,
                    outInfo,
                    outInfo.deterministicWristPlaneLocal);
        }
        return true;
    }


    struct HooksFirstPersonBodyViewmodelSkeleton
    {
        VR* owner = nullptr;
        const HooksFirstPersonBodyEyeSceneState* sceneState = nullptr;
        std::uint64_t sceneSerial = 0;
        std::uint64_t playerGeneration = 0;
        int localPlayerIndex = -1;
        bool forceNativeViewmodelArmCropping = false;
        std::vector<std::string> boneNames;
        std::vector<vr_vm_stabilize::Mat3x4> bones;
    };

    inline int HooksFirstPersonBodyEyeSlot(int eyeIndex)
    {
        if (eyeIndex == 1)
            return 0;
        if (eyeIndex == 2)
            return 1;
        return -1;
    }

    inline std::mutex& HooksFirstPersonBodyViewmodelSkeletonMutex()
    {
        static std::mutex s_mutex;
        return s_mutex;
    }

    inline HooksFirstPersonBodyViewmodelSkeleton*
        HooksFirstPersonBodyViewmodelSkeletonStorage()
    {
        static HooksFirstPersonBodyViewmodelSkeleton s_skeletons[2]{};
        return s_skeletons;
    }

    inline void HooksFirstPersonBodyPublishViewmodelSkeleton(
        VR* vr,
        const HooksFirstPersonBodyEyeSceneState* bodyState,
        bool forceNativeViewmodelArmCropping,
        const std::vector<std::string>& boneNames,
        const std::vector<vr_vm_stabilize::Mat3x4>& bones)
    {
        if (!vr || !bodyState || !bodyState->bodyActive ||
            bodyState->sceneSerial == 0 || bodyState->playerGeneration == 0 ||
            boneNames.empty() || boneNames.size() != bones.size() ||
            boneNames.size() > 128u)
        {
            return;
        }

        const int slot = HooksFirstPersonBodyEyeSlot(bodyState->eyeIndex);
        if (slot < 0)
            return;

        for (size_t bone = 0; bone < bones.size(); ++bone)
        {
            if (boneNames[bone].empty() ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(bones[bone]))
            {
                return;
            }
        }

        HooksFirstPersonBodyViewmodelSkeleton snapshot{};
        snapshot.owner = vr;
        snapshot.sceneState = bodyState;
        snapshot.sceneSerial = bodyState->sceneSerial;
        snapshot.playerGeneration = bodyState->playerGeneration;
        snapshot.localPlayerIndex = bodyState->localPlayerIndex;
        snapshot.forceNativeViewmodelArmCropping =
            forceNativeViewmodelArmCropping;
        snapshot.boneNames = boneNames;
        snapshot.bones = bones;

        std::lock_guard<std::mutex> lock(
            HooksFirstPersonBodyViewmodelSkeletonMutex());
        HooksFirstPersonBodyViewmodelSkeletonStorage()[slot] =
            std::move(snapshot);
    }

    inline bool HooksFirstPersonBodyCurrentSceneWantsViewmodelSkeleton(
        VR* vr,
        const HooksFirstPersonBodyEyeSceneState*& outBodyState)
    {
        outBodyState = nullptr;
        if (!vr || !vr->m_IsVREnabled || vr->m_IsThirdPersonCamera ||
            !vr->m_FirstPersonBodyEnabled ||
            !g_FirstPersonBodyPlayerReady.load(std::memory_order_acquire) ||
            !g_FirstPersonBodyActualFirstPerson.load(std::memory_order_acquire) ||
            InterlockedCompareExchange(
                &g_FirstPersonBodyEyeSceneActive, 0, 0) == 0)
        {
            return false;
        }

        HooksFirstPersonBodyEyeSceneState* const bodyState =
            g_FirstPersonBodyPublishedState.load(std::memory_order_acquire);
        if (!bodyState || !bodyState->bodyActive ||
            bodyState->sceneSerial == 0 || bodyState->playerGeneration == 0 ||
            bodyState->playerGeneration !=
            g_FirstPersonBodyPlayerGeneration.load(std::memory_order_acquire))
        {
            return false;
        }

        outBodyState = bodyState;
        return true;
    }

    inline bool HooksFirstPersonBodyCurrentSceneForcesNativeViewmodelArmCropping(
        VR* vr)
    {
        const HooksFirstPersonBodyEyeSceneState* bodyState = nullptr;
        if (!HooksFirstPersonBodyCurrentSceneWantsViewmodelSkeleton(
                vr, bodyState) || !bodyState)
        {
            return false;
        }

        const int slot = HooksFirstPersonBodyEyeSlot(bodyState->eyeIndex);
        if (slot < 0)
            return false;

        std::lock_guard<std::mutex> lock(
            HooksFirstPersonBodyViewmodelSkeletonMutex());
        const HooksFirstPersonBodyViewmodelSkeleton& snapshot =
            HooksFirstPersonBodyViewmodelSkeletonStorage()[slot];
        return snapshot.owner == vr &&
            snapshot.sceneState == bodyState &&
            snapshot.sceneSerial == bodyState->sceneSerial &&
            snapshot.playerGeneration == bodyState->playerGeneration &&
            snapshot.localPlayerIndex == bodyState->localPlayerIndex &&
            snapshot.forceNativeViewmodelArmCropping;
    }

    inline bool HooksNativeViewmodelEffectiveArmCroppingEnabled(VR* vr)
    {
        return vr &&
            (vr->m_NativeViewmodelArmCroppingEnabled ||
                HooksFirstPersonBodyCurrentSceneForcesNativeViewmodelArmCropping(vr));
    }

    inline bool HooksFirstPersonBodyReadViewmodelSkeleton(
        VR* vr,
        const HooksFirstPersonBodyEyeSceneState* bodyState,
        std::vector<std::string>& outBoneNames,
        std::vector<vr_vm_stabilize::Mat3x4>& outBones)
    {
        outBoneNames.clear();
        outBones.clear();
        if (!vr || !bodyState)
            return false;

        const int slot = HooksFirstPersonBodyEyeSlot(bodyState->eyeIndex);
        if (slot < 0)
            return false;

        HooksFirstPersonBodyViewmodelSkeleton snapshot{};
        {
            std::lock_guard<std::mutex> lock(
                HooksFirstPersonBodyViewmodelSkeletonMutex());
            snapshot = HooksFirstPersonBodyViewmodelSkeletonStorage()[slot];
        }

        if (snapshot.owner != vr || snapshot.sceneState != bodyState ||
            snapshot.sceneSerial != bodyState->sceneSerial ||
            snapshot.playerGeneration != bodyState->playerGeneration ||
            snapshot.localPlayerIndex != bodyState->localPlayerIndex)
        {
            return false;
        }

        if (snapshot.boneNames.empty() ||
            snapshot.boneNames.size() != snapshot.bones.size() ||
            snapshot.boneNames.size() > 128u)
        {
            return false;
        }
        for (const vr_vm_stabilize::Mat3x4& bone : snapshot.bones)
        {
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(bone))
                return false;
        }

        outBoneNames = std::move(snapshot.boneNames);
        outBones = std::move(snapshot.bones);
        return true;
    }

    inline float HooksTrackedBodyWrapYaw(float yaw)
    {
        if (!std::isfinite(yaw))
            return 0.0f;
        yaw -= 360.0f * std::floor((yaw + 180.0f) / 360.0f);
        return yaw;
    }

    struct HooksTrackedBodyYawReference
    {
        const VR* owner = nullptr;
        std::uint32_t poseLockGeneration = 0u;
        bool poseLockReady = false;
        bool valid = false;
        float physicalYawAtPoseLock = 0.0f;
    };

    // Resolve the local player's rendered torso yaw independently from HMD yaw.
    // Thumbstick/mouse turning rotates the body immediately through RotationOffset,
    // while physical head yaw gets a comfort cone around the direction captured
    // at delayed pose lock. Inside the cone the torso stays fixed; beyond it the
    // torso follows only enough to keep the head at the edge.
    // This is shared by first-person body anchoring and local third-person IK.
    inline float HooksTrackedBodyResolveVisualYaw(
        const VR* vr,
        float headWorldYaw)
    {
        headWorldYaw = HooksTrackedBodyWrapYaw(headWorldYaw);
        if (!vr)
            return headWorldYaw;

        float turnYaw =
            vr->m_RenderRotationOffset.load(std::memory_order_acquire);
        if (!std::isfinite(turnYaw))
            turnYaw = vr->m_RotationOffset;
        if (!std::isfinite(turnYaw))
            return headWorldYaw;
        turnYaw = HooksTrackedBodyWrapYaw(turnYaw);

        const std::uint32_t poseLockGeneration =
            vr->m_NativeViewmodelLeftHandFreezeGeneration.load(
                std::memory_order_acquire);
        const bool poseLockReady =
            vr->m_NativeViewmodelLeftHandFreezeReady.load(
                std::memory_order_acquire) != 0u;
        float physicalYawAtPoseLock = 0.0f;
        {
            static std::mutex s_referenceMutex;
            static HooksTrackedBodyYawReference s_reference;
            std::lock_guard<std::mutex> lock(s_referenceMutex);

            const bool identityChanged =
                s_reference.owner != vr ||
                s_reference.poseLockGeneration != poseLockGeneration;
            const bool poseLockBecameReady =
                poseLockReady && !s_reference.poseLockReady;
            if (identityChanged || !s_reference.valid || poseLockBecameReady)
            {
                s_reference.owner = vr;
                s_reference.poseLockGeneration = poseLockGeneration;
                s_reference.valid = true;
                s_reference.physicalYawAtPoseLock =
                    HooksTrackedBodyWrapYaw(headWorldYaw - turnYaw);
                if (poseLockReady)
                {
                    Game::logMsg(
                        "[VR][TrackedBodyYaw] pose-lock reference headYaw=%.1f turnYaw=%.1f physicalYaw=%.1f generation=%u",
                        headWorldYaw,
                        turnYaw,
                        s_reference.physicalYawAtPoseLock,
                        poseLockGeneration);
                }
            }
            s_reference.poseLockReady = poseLockReady;
            physicalYawAtPoseLock = s_reference.physicalYawAtPoseLock;
        }

        const float neutralBodyYaw = HooksTrackedBodyWrapYaw(
            turnYaw + physicalYawAtPoseLock);

        const float deadzone = std::clamp(
            vr->m_WorldModelVRPoseBodyYawDeadzoneDeg,
            0.0f,
            90.0f);
        const float headRelativeYaw = HooksTrackedBodyWrapYaw(
            headWorldYaw - neutralBodyYaw);
        if (std::fabs(headRelativeYaw) <= deadzone)
            return neutralBodyYaw;

        return HooksTrackedBodyWrapYaw(
            headWorldYaw - std::copysign(deadzone, headRelativeYaw));
    }

    inline Vector HooksTrackedBodyClampPlanarLeanMeters(
        const VR* vr,
        const Vector& localOffsetMeters,
        float* outFraction = nullptr)
    {
        if (outFraction)
            *outFraction = 0.0f;
        if (!vr ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(localOffsetMeters))
        {
            return Vector{};
        }

        Vector planar(localOffsetMeters.x, localOffsetMeters.y, 0.0f);
        const float length = std::sqrt(
            planar.x * planar.x + planar.y * planar.y);
        const float maxOffset = std::clamp(
            vr->m_BodyLeanMaxOffsetMeters,
            0.0f,
            0.75f);
        if (!std::isfinite(length) || length <= 0.000001f ||
            maxOffset <= 0.000001f)
        {
            return Vector{};
        }

        if (length > maxOffset)
            planar *= maxOffset / length;
        if (outFraction)
            *outFraction = std::clamp(length / maxOffset, 0.0f, 1.0f);
        return planar;
    }

    // Shared analytic two-bone IK for native first-person viewmodel arms and
    // survivor world-model arms. The caller supplies the final shoulder and
    // owns branch isolation, temporal continuity and result publication.
    struct HooksNativeViewmodelArmIkChain
    {
        int side = 0;
        int upperArm = -1;
        int forearm = -1;
        int hand = -1;
    };

    struct HooksNativeViewmodelArmIkSolution
    {
        Vector elbow{};
        Vector hand{};
        Vector bendDirection{};
    };

    inline bool HooksNativeViewmodelArmIkNormalize(
        const Vector& value,
        Vector& outNormalized,
        float* outLength = nullptr)
    {
        const float length = value.Length();
        if (!std::isfinite(length) || length <= 0.0001f)
            return false;

        outNormalized = value * (1.0f / length);
        if (outLength)
            *outLength = length;
        return HooksNativeViewmodelHandsOnlyVectorFinite(outNormalized);
    }

    inline bool HooksNativeViewmodelArmIkProjectOntoPlane(
        const Vector& value,
        const Vector& planeNormal,
        Vector& outProjected)
    {
        return HooksNativeViewmodelArmIkNormalize(
            value - planeNormal * DotProduct(value, planeNormal),
            outProjected);
    }

    inline float HooksNativeViewmodelArmIkWrapRadians(float radians)
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = kPi * 2.0f;
        if (!std::isfinite(radians))
            return radians;

        radians = std::fmod(radians + kPi, kTwoPi);
        if (radians < 0.0f)
            radians += kTwoPi;
        return radians - kPi;
    }

    inline bool HooksNativeViewmodelArmIkFindChain(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& sideInfo,
        HooksNativeViewmodelArmIkChain& outChain)
    {
        outChain = HooksNativeViewmodelArmIkChain{};
        outChain.side = sideInfo.side;
        outChain.hand = sideInfo.hand;
        if (sideInfo.side == 0 || sideInfo.hand < 0 || sideInfo.hand >= numBones ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        const std::vector<const char*> forearmNeedles = {
            "forearm", "fore_arm", "lowerarm", "lower_arm", "lower-arm",
            "ulna", "radius", "elbow",
        };
        const std::vector<const char*> upperArmNeedles = {
            "upperarm", "upper_arm", "upper-arm", "uparm", "up_arm", "humerus",
        };

        int forearm = -1;
        HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
            boneNames,
            boneParents,
            numBones,
            sideInfo.hand,
            forearmNeedles,
            sideInfo.side,
            forearm);

        if (forearm < 0 && sideInfo.forearm >= 0 && sideInfo.forearm < numBones &&
            sideInfo.forearm != sideInfo.hand &&
            HooksNativeViewmodelHandsOnlyIsAncestor(
                boneParents,
                sideInfo.hand,
                sideInfo.forearm,
                numBones))
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(
                boneNames[static_cast<size_t>(sideInfo.forearm)]);
            if (!HooksNativeViewmodelHandsOnlyBoneNameMatchesAny(lowerName, upperArmNeedles))
                forearm = sideInfo.forearm;
        }

        int upperArm = -1;
        if (forearm >= 0)
        {
            HooksNativeViewmodelHandsOnlyFindNamedBoneOnAncestorChain(
                boneNames,
                boneParents,
                numBones,
                forearm,
                upperArmNeedles,
                sideInfo.side,
                upperArm);
        }

        // Some replacement rigs leave the lower-arm bone unnamed. In that case,
        // the direct child below a named upper arm is the lower segment root.
        if (upperArm < 0)
        {
            int child = sideInfo.hand;
            int current = boneParents[static_cast<size_t>(sideInfo.hand)];
            for (int guard = 0;
                guard < numBones && current >= 0 && current < numBones;
                ++guard)
            {
                const std::string lowerName = vr_vm_stabilize::ToLowerAscii(
                    boneNames[static_cast<size_t>(current)]);
                if (HooksNativeViewmodelHandsOnlyBoneSide(lowerName) == sideInfo.side &&
                    HooksNativeViewmodelHandsOnlyBoneNameMatchesAny(lowerName, upperArmNeedles))
                {
                    upperArm = current;
                    if (forearm < 0)
                        forearm = child;
                    break;
                }
                child = current;
                current = boneParents[static_cast<size_t>(current)];
            }
        }

        if (forearm >= 0 && upperArm < 0)
        {
            const int parent = boneParents[static_cast<size_t>(forearm)];
            if (parent >= 0 && parent < numBones && parent != sideInfo.hand)
            {
                const std::string parentName = vr_vm_stabilize::ToLowerAscii(
                    boneNames[static_cast<size_t>(parent)]);
                const int parentSide =
                    HooksNativeViewmodelHandsOnlyBoneSide(parentName);
                const bool explicitlyNamedUpperArm =
                    HooksNativeViewmodelHandsOnlyBoneNameMatchesAny(
                        parentName,
                        upperArmNeedles);
                if (parentSide == sideInfo.side ||
                    (parentSide == 0 && explicitlyNamedUpperArm))
                {
                    upperArm = parent;
                }
            }
        }

        if (upperArm < 0 || forearm < 0 || upperArm == forearm ||
            forearm == sideInfo.hand ||
            !HooksNativeViewmodelHandsOnlyIsAncestor(
                boneParents,
                sideInfo.hand,
                forearm,
                numBones) ||
            !HooksNativeViewmodelHandsOnlyIsAncestor(
                boneParents,
                forearm,
                upperArm,
                numBones))
        {
            return false;
        }

        outChain.upperArm = upperArm;
        outChain.forearm = forearm;
        return true;
    }

    inline bool HooksNativeViewmodelArmIkApplyDeltaToBranch(
        const std::vector<int>& boneParents,
        int numBones,
        int rootBone,
        const vr_vm_stabilize::Mat3x4& delta,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones || rootBone < 0 || rootBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(delta))
        {
            return false;
        }

        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!HooksNativeViewmodelHandsOnlyIsAncestor(
                boneParents,
                bone,
                rootBone,
                numBones))
            {
                continue;
            }

            vr_vm_stabilize::Mat3x4 transformed{};
            vr_vm_stabilize::Mul(delta, bones[bone], transformed);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(transformed))
                return false;
            bones[bone] = transformed;
        }
        return true;
    }

    inline bool HooksNativeViewmodelArmIkApplyDeltaToSingleBone(
        int numBones,
        int boneIndex,
        const vr_vm_stabilize::Mat3x4& delta,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones || boneIndex < 0 || boneIndex >= numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(delta))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 transformed{};
        vr_vm_stabilize::Mul(delta, bones[boneIndex], transformed);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(transformed))
            return false;
        bones[boneIndex] = transformed;
        return true;
    }

    inline bool HooksNativeViewmodelArmIkBuildAlignmentDelta(
        const Vector& pivot,
        const Vector& fromDirection,
        const Vector& toDirection,
        const Vector& fallbackAxis,
        vr_vm_stabilize::Mat3x4& outDelta)
    {
        outDelta = vr_vm_stabilize::Identity();
        Vector from{};
        Vector to{};
        if (!HooksNativeViewmodelArmIkNormalize(fromDirection, from) ||
            !HooksNativeViewmodelArmIkNormalize(toDirection, to) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(pivot))
        {
            return false;
        }

        const float cosine = std::clamp(DotProduct(from, to), -1.0f, 1.0f);
        Vector axis = CrossProduct(from, to);
        float sine = axis.Length();
        if (!std::isfinite(sine))
            return false;

        if (sine < 0.00001f)
        {
            if (cosine > 0.99999f)
                return true;

            axis = CrossProduct(from, fallbackAxis);
            if (VectorNormalize(axis) == 0.0f)
            {
                axis = CrossProduct(from, Vector(0.0f, 0.0f, 1.0f));
                if (VectorNormalize(axis) == 0.0f)
                {
                    axis = CrossProduct(from, Vector(1.0f, 0.0f, 0.0f));
                    if (VectorNormalize(axis) == 0.0f)
                        return false;
                }
            }
            sine = 0.0f;
        }
        else
        {
            axis *= (1.0f / sine);
        }

        const float oneMinusCosine = 1.0f - cosine;
        const float x = axis.x;
        const float y = axis.y;
        const float z = axis.z;
        outDelta.m[0][0] = cosine + x * x * oneMinusCosine;
        outDelta.m[0][1] = x * y * oneMinusCosine - z * sine;
        outDelta.m[0][2] = x * z * oneMinusCosine + y * sine;
        outDelta.m[1][0] = y * x * oneMinusCosine + z * sine;
        outDelta.m[1][1] = cosine + y * y * oneMinusCosine;
        outDelta.m[1][2] = y * z * oneMinusCosine - x * sine;
        outDelta.m[2][0] = z * x * oneMinusCosine - y * sine;
        outDelta.m[2][1] = z * y * oneMinusCosine + x * sine;
        outDelta.m[2][2] = cosine + z * z * oneMinusCosine;

        const Vector rotatedPivot(
            outDelta.m[0][0] * pivot.x +
            outDelta.m[0][1] * pivot.y +
            outDelta.m[0][2] * pivot.z,
            outDelta.m[1][0] * pivot.x +
            outDelta.m[1][1] * pivot.y +
            outDelta.m[1][2] * pivot.z,
            outDelta.m[2][0] * pivot.x +
            outDelta.m[2][1] * pivot.y +
            outDelta.m[2][2] * pivot.z);
        const Vector translation = pivot - rotatedPivot;
        outDelta.m[0][3] = translation.x;
        outDelta.m[1][3] = translation.y;
        outDelta.m[2][3] = translation.z;
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outDelta);
    }

    inline bool HooksNativeViewmodelArmIkBuildAxisRotationDelta(
        const Vector& pivot,
        const Vector& axisDirection,
        float radians,
        vr_vm_stabilize::Mat3x4& outDelta)
    {
        outDelta = vr_vm_stabilize::Identity();
        Vector axis{};
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(pivot) ||
            !std::isfinite(radians) ||
            !HooksNativeViewmodelArmIkNormalize(axisDirection, axis))
        {
            return false;
        }

        if (std::fabs(radians) <= 0.000001f)
            return true;

        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        if (!std::isfinite(cosine) || !std::isfinite(sine))
            return false;

        const float oneMinusCosine = 1.0f - cosine;
        const float x = axis.x;
        const float y = axis.y;
        const float z = axis.z;
        outDelta.m[0][0] = cosine + x * x * oneMinusCosine;
        outDelta.m[0][1] = x * y * oneMinusCosine - z * sine;
        outDelta.m[0][2] = x * z * oneMinusCosine + y * sine;
        outDelta.m[1][0] = y * x * oneMinusCosine + z * sine;
        outDelta.m[1][1] = cosine + y * y * oneMinusCosine;
        outDelta.m[1][2] = y * z * oneMinusCosine - x * sine;
        outDelta.m[2][0] = z * x * oneMinusCosine - y * sine;
        outDelta.m[2][1] = z * y * oneMinusCosine + x * sine;
        outDelta.m[2][2] = cosine + z * z * oneMinusCosine;

        const Vector rotatedPivot(
            outDelta.m[0][0] * pivot.x +
            outDelta.m[0][1] * pivot.y +
            outDelta.m[0][2] * pivot.z,
            outDelta.m[1][0] * pivot.x +
            outDelta.m[1][1] * pivot.y +
            outDelta.m[1][2] * pivot.z,
            outDelta.m[2][0] * pivot.x +
            outDelta.m[2][1] * pivot.y +
            outDelta.m[2][2] * pivot.z);
        const Vector translation = pivot - rotatedPivot;
        outDelta.m[0][3] = translation.x;
        outDelta.m[1][3] = translation.y;
        outDelta.m[2][3] = translation.z;
        return HooksNativeViewmodelHandsOnlyMatrixFinite(outDelta);
    }

    inline bool HooksNativeViewmodelArmIkBuildAnchorRotation(
        const Vector& baseForward,
        const Vector& baseRight,
        const Vector& baseUp,
        const Vector& rotationOffsetDeg,
        Vector& outForward,
        Vector& outRight,
        Vector& outUp,
        vr_vm_stabilize::Mat3x4& outRotationDelta)
    {
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(baseForward) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(baseRight) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(baseUp) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(rotationOffsetDeg) ||
            !HooksNativeViewmodelArmIkNormalize(baseForward, outForward) ||
            !HooksNativeViewmodelArmIkNormalize(baseRight, outRight) ||
            !HooksNativeViewmodelArmIkNormalize(baseUp, outUp))
        {
            return false;
        }

        outRotationDelta = vr_vm_stabilize::Identity();
        auto applyRotation = [&](Vector axis, float degrees) -> bool
            {
                if (std::fabs(degrees) <= 0.0001f)
                    return true;

                vr_vm_stabilize::Mat3x4 rotation{};
                if (!HooksNativeViewmodelArmIkBuildAxisRotationDelta(
                    Vector(0.0f, 0.0f, 0.0f),
                    axis,
                    DEG2RAD(degrees),
                    rotation))
                {
                    return false;
                }

                vr_vm_stabilize::Mat3x4 combined{};
                vr_vm_stabilize::Mul(rotation, outRotationDelta, combined);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(combined))
                    return false;
                outRotationDelta = combined;

                outForward = HooksTransformVector(rotation, outForward);
                outRight = HooksTransformVector(rotation, outRight);
                outUp = HooksTransformVector(rotation, outUp);
                return
                    HooksNativeViewmodelArmIkNormalize(outForward, outForward) &&
                    HooksNativeViewmodelArmIkNormalize(outRight, outRight) &&
                    HooksNativeViewmodelArmIkNormalize(outUp, outUp);
            };

        // Match Source-style pitch/yaw/roll semantics in the current torso frame:
        // yaw turns around local up, pitch turns around local right (positive
        // Source pitch looks downward, hence the sign), and roll turns around
        // the already-rotated local forward axis.
        if (!applyRotation(outUp, rotationOffsetDeg.y) ||
            !applyRotation(outRight, -rotationOffsetDeg.x) ||
            !applyRotation(outForward, rotationOffsetDeg.z))
        {
            return false;
        }

        return HooksNativeViewmodelHandsOnlyMatrixFinite(outRotationDelta);
    }

    inline bool HooksTrackedBodyBuildLeanDelta(
        const VR* vr,
        const Vector& pivot,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp,
        const Vector& localOffsetMeters,
        float weight,
        vr_vm_stabilize::Mat3x4& outDelta)
    {
        outDelta = vr_vm_stabilize::Identity();
        if (!vr ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(pivot) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(bodyForward) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(bodyRight) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(bodyUp))
        {
            return false;
        }

        float leanFraction = 0.0f;
        const Vector clampedLocal = HooksTrackedBodyClampPlanarLeanMeters(
            vr,
            localOffsetMeters,
            &leanFraction);
        const float maxAngleDeg = std::clamp(
            vr->m_BodyLeanMaxAngleDeg,
            0.0f,
            35.0f);
        weight = std::clamp(weight, 0.0f, 1.0f);
        if (leanFraction <= 0.000001f ||
            maxAngleDeg <= 0.000001f ||
            weight <= 0.000001f)
        {
            return false;
        }

        const Vector leanDirectionWorld =
            bodyForward * clampedLocal.x +
            bodyRight * clampedLocal.y;
        Vector leanAxis = CrossProduct(bodyUp, leanDirectionWorld);
        if (!HooksNativeViewmodelArmIkNormalize(leanAxis, leanAxis))
            return false;

        constexpr float kDegToRad =
            3.14159265358979323846f / 180.0f;
        const float radians =
            maxAngleDeg * kDegToRad * leanFraction * weight;
        return HooksNativeViewmodelArmIkBuildAxisRotationDelta(
            pivot,
            leanAxis,
            radians,
            outDelta);
    }

    inline bool HooksNativeViewmodelArmIkBuildForearmTwistDelta(
        const Vector& elbow,
        const Vector& forearmDirection,
        const vr_vm_stabilize::Mat3x4& currentHand,
        const vr_vm_stabilize::Mat3x4& desiredHand,
        vr_vm_stabilize::Mat3x4& outDelta,
        float* outRadians = nullptr)
    {
        outDelta = vr_vm_stabilize::Identity();
        if (outRadians)
            *outRadians = 0.0f;
        Vector axis{};
        vr_vm_stabilize::Mat3x4 currentRigid{};
        vr_vm_stabilize::Mat3x4 desiredRigid{};
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(elbow) ||
            !HooksNativeViewmodelArmIkNormalize(forearmDirection, axis) ||
            !HooksViewmodelAutoGripNormalizeRigidMatrix(
                currentHand,
                currentRigid) ||
            !HooksViewmodelAutoGripNormalizeRigidMatrix(
                desiredHand,
                desiredRigid))
        {
            return false;
        }

        // Use all three corresponding hand axes instead of selecting one axis.
        // This avoids a 90-degree roll jump when the best projected axis changes
        // as the controller passes through a near-parallel orientation.
        float weightedCosine = 0.0f;
        float weightedSine = 0.0f;
        float totalProjectionWeight = 0.0f;
        for (int column = 0; column < 3; ++column)
        {
            const Vector currentAxis =
                HooksViewmodelAutoGripMatrixAxis(currentRigid, column);
            const Vector desiredAxis =
                HooksViewmodelAutoGripMatrixAxis(desiredRigid, column);
            const Vector currentProjected =
                currentAxis - axis * DotProduct(currentAxis, axis);
            const Vector desiredProjected =
                desiredAxis - axis * DotProduct(desiredAxis, axis);
            const float currentLength = currentProjected.Length();
            const float desiredLength = desiredProjected.Length();
            if (!std::isfinite(currentLength) ||
                !std::isfinite(desiredLength))
            {
                return false;
            }

            const float projectionWeight = currentLength * desiredLength;
            if (projectionWeight <= 0.000001f)
                continue;

            Vector normalizedCurrent{};
            Vector normalizedDesired{};
            if (!HooksNativeViewmodelArmIkNormalize(
                currentProjected,
                normalizedCurrent) ||
                !HooksNativeViewmodelArmIkNormalize(
                    desiredProjected,
                    normalizedDesired))
            {
                continue;
            }

            const float cosine = std::clamp(
                DotProduct(normalizedCurrent, normalizedDesired),
                -1.0f,
                1.0f);
            const float sine = std::clamp(
                DotProduct(
                    axis,
                    CrossProduct(normalizedCurrent, normalizedDesired)),
                -1.0f,
                1.0f);
            weightedCosine += cosine * projectionWeight;
            weightedSine += sine * projectionWeight;
            totalProjectionWeight += projectionWeight;
        }

        if (totalProjectionWeight <= 0.000001f ||
            !std::isfinite(weightedCosine) ||
            !std::isfinite(weightedSine) ||
            std::fabs(weightedCosine) + std::fabs(weightedSine) <= 0.000001f)
        {
            return false;
        }

        const float radians = std::atan2(weightedSine, weightedCosine);
        if (!std::isfinite(radians))
            return false;
        if (outRadians)
            *outRadians = radians;

        return HooksNativeViewmodelArmIkBuildAxisRotationDelta(
            elbow,
            axis,
            radians,
            outDelta);
    }

    inline bool HooksNativeViewmodelArmIkSolveTwoBone(
        const Vector& shoulder,
        const Vector& target,
        float upperLength,
        float lowerLength,
        const Vector& poleDirection,
        const Vector* previousBendDirection,
        const Vector& animatedUpperDirection,
        bool enforcePoleHemisphere,
        HooksNativeViewmodelArmIkSolution& outSolution)
    {
        outSolution = HooksNativeViewmodelArmIkSolution{};
        if (!std::isfinite(upperLength) || !std::isfinite(lowerLength) ||
            upperLength < 0.01f || lowerLength < 0.01f)
        {
            return false;
        }

        Vector targetDirection{};
        float targetDistance = 0.0f;
        if (!HooksNativeViewmodelArmIkNormalize(
            target - shoulder,
            targetDirection,
            &targetDistance))
        {
            return false;
        }

        const float reachEpsilon = std::max(
            0.01f,
            (upperLength + lowerLength) * 0.0005f);
        const float minimumReach =
            std::fabs(upperLength - lowerLength) + reachEpsilon;
        const float maximumReach =
            upperLength + lowerLength - reachEpsilon;
        if (!(minimumReach < maximumReach))
            return false;

        const float solvedDistance = std::clamp(
            targetDistance,
            minimumReach,
            maximumReach);
        const float along =
            (upperLength * upperLength - lowerLength * lowerLength +
                solvedDistance * solvedDistance) /
            (2.0f * solvedDistance);
        const float bendHeight = std::sqrt(std::max(
            0.0f,
            upperLength * upperLength - along * along));

        Vector previousProjected{};
        const bool previousValid =
            previousBendDirection &&
            HooksNativeViewmodelArmIkProjectOntoPlane(
                *previousBendDirection,
                targetDirection,
                previousProjected);

        const Vector rawPoleProjection =
            poleDirection -
            targetDirection * DotProduct(poleDirection, targetDirection);
        Vector poleProjected{};
        const bool poleValid = HooksNativeViewmodelArmIkNormalize(
            rawPoleProjection,
            poleProjected);
        const float poleLengthSq = DotProduct(poleDirection, poleDirection);
        const float poleStrength =
            poleValid && std::isfinite(poleLengthSq) && poleLengthSq > 0.000001f
            ? std::sqrt(
                DotProduct(rawPoleProjection, rawPoleProjection) /
                poleLengthSq)
            : 0.0f;

        Vector bendDirection{};
        if (poleValid)
        {
            bendDirection = poleProjected;
            Vector previousForBlend = previousProjected;
            if (previousValid && enforcePoleHemisphere &&
                DotProduct(poleProjected, previousForBlend) < 0.0f)
            {
                // First-person elbows must stay in the anatomical pole
                // hemisphere. Continuity may choose the nearest direction, but
                // it cannot preserve an inside-out elbow across a singular pose.
                previousForBlend *= -1.0f;
            }
            else if (previousValid &&
                DotProduct(bendDirection, previousForBlend) < 0.0f)
            {
                bendDirection *= -1.0f;
            }

            if (previousValid)
            {
                const float poleWeight = std::clamp(
                    (poleStrength - 0.05f) / 0.30f,
                    0.0f,
                    1.0f);
                Vector blended{};
                if (HooksNativeViewmodelArmIkNormalize(
                    previousForBlend * (1.0f - poleWeight) +
                    bendDirection * poleWeight,
                    blended))
                {
                    bendDirection = blended;
                }
            }
            if (enforcePoleHemisphere &&
                DotProduct(bendDirection, poleProjected) < 0.0f)
            {
                bendDirection *= -1.0f;
            }
        }
        else if (previousValid)
        {
            bendDirection = previousProjected;
        }
        else if (!HooksNativeViewmodelArmIkProjectOntoPlane(
            animatedUpperDirection,
            targetDirection,
            bendDirection))
        {
            const Vector reference =
                std::fabs(targetDirection.z) < 0.9f
                ? Vector(0.0f, 0.0f, -1.0f)
                : Vector(1.0f, 0.0f, 0.0f);
            if (!HooksNativeViewmodelArmIkProjectOntoPlane(
                reference,
                targetDirection,
                bendDirection))
            {
                return false;
            }
        }

        outSolution.elbow =
            shoulder + targetDirection * along + bendDirection * bendHeight;
        outSolution.hand = shoulder + targetDirection * solvedDistance;
        outSolution.bendDirection = bendDirection;
        return HooksNativeViewmodelHandsOnlyVectorFinite(outSolution.elbow) &&
            HooksNativeViewmodelHandsOnlyVectorFinite(outSolution.hand) &&
            HooksNativeViewmodelHandsOnlyVectorFinite(outSolution.bendDirection);
    }

    inline Vector HooksNativeViewmodelArmIkWorldDirectionToBodyLocal(
        const Vector& direction,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp)
    {
        return Vector(
            DotProduct(direction, bodyForward),
            DotProduct(direction, bodyRight),
            DotProduct(direction, bodyUp));
    }

    inline Vector HooksNativeViewmodelArmIkBodyLocalDirectionToWorld(
        const Vector& local,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp)
    {
        return bodyForward * local.x + bodyRight * local.y + bodyUp * local.z;
    }

    inline bool HooksNativeViewmodelArmIkResolveBodyFrame(
        VR* vr,
        Vector& outViewOrigin,
        Vector& outBodyForward,
        Vector& outBodyRight,
        Vector& outBodyUp)
    {
        Vector ignoredViewForward{};
        Vector ignoredViewRight{};
        Vector ignoredViewUp{};
        if (!HooksNativeViewmodelHandsOnlyResolveViewFrame(
            vr,
            outViewOrigin,
            ignoredViewForward,
            ignoredViewRight,
            ignoredViewUp) ||
            !vr)
        {
            return false;
        }

        // ViewOrigin still supplies the tracked head position for the no-body
        // fallback, but horizontal arm orientation must never consume HMD yaw.
        // Use only the artificial body-turn frame here. When the first-person
        // survivor body is active, its published shoulder line replaces this
        // fallback before the analytic solve.
        float bodyYawDeg =
            vr->m_RenderRotationOffset.load(std::memory_order_acquire);
        if (!std::isfinite(bodyYawDeg))
            bodyYawDeg = vr->m_RotationOffset;
        if (!std::isfinite(bodyYawDeg))
            return false;
        bodyYawDeg = HooksTrackedBodyWrapYaw(bodyYawDeg);

        Vector turnForward{};
        Vector turnRight{};
        Vector turnUp{};
        QAngle::AngleVectors(
            QAngle(0.0f, bodyYawDeg, 0.0f),
            &turnForward,
            &turnRight,
            &turnUp);

        outBodyUp = Vector(0.0f, 0.0f, 1.0f);
        if (!HooksNativeViewmodelArmIkNormalize(
            turnForward - outBodyUp * DotProduct(turnForward, outBodyUp),
            outBodyForward))
        {
            return false;
        }

        Vector horizontalRight =
            turnRight - outBodyUp * DotProduct(turnRight, outBodyUp);
        horizontalRight -=
            outBodyForward * DotProduct(horizontalRight, outBodyForward);
        if (!HooksNativeViewmodelArmIkNormalize(horizontalRight, outBodyRight))
        {
            outBodyRight = CrossProduct(outBodyForward, outBodyUp);
            if (!HooksNativeViewmodelArmIkNormalize(outBodyRight, outBodyRight))
                return false;
        }
        return true;
    }

    inline bool HooksNativeViewmodelArmIkResolveTurnFrame(
        VR* vr,
        Vector& outTurnForward,
        Vector& outTurnRight,
        Vector& outTurnUp)
    {
        if (!vr)
            return false;

        // Continuity and the animation-free bind branch stay in the artificial
        // snap/smooth-turn frame. Physical HMD yaw is intentionally absent from
        // this frame so turning only the head cannot accumulate upper-arm twist.
        float turnYawDeg =
            vr->m_RenderRotationOffset.load(std::memory_order_acquire);
        if (!std::isfinite(turnYawDeg))
            turnYawDeg = vr->m_RotationOffset;
        if (!std::isfinite(turnYawDeg))
            return false;
        turnYawDeg -=
            360.0f * std::floor((turnYawDeg + 180.0f) / 360.0f);

        QAngle turnYawOnly(0.0f, turnYawDeg, 0.0f);
        Vector turnForward{};
        Vector turnRight{};
        Vector turnUp{};
        QAngle::AngleVectors(
            turnYawOnly,
            &turnForward,
            &turnRight,
            &turnUp);

        outTurnUp = Vector(0.0f, 0.0f, 1.0f);
        if (!HooksNativeViewmodelArmIkNormalize(
            turnForward -
            outTurnUp * DotProduct(turnForward, outTurnUp),
            outTurnForward))
        {
            return false;
        }

        Vector horizontalRight =
            turnRight - outTurnUp * DotProduct(turnRight, outTurnUp);
        horizontalRight -=
            outTurnForward * DotProduct(horizontalRight, outTurnForward);
        if (!HooksNativeViewmodelArmIkNormalize(
            horizontalRight,
            outTurnRight))
        {
            outTurnRight = CrossProduct(outTurnForward, outTurnUp);
            if (!HooksNativeViewmodelArmIkNormalize(
                outTurnRight,
                outTurnRight))
            {
                return false;
            }
        }
        return true;
    }

    inline bool HooksNativeViewmodelArmIkBuildHandTarget(
        VR* vr,
        const HooksNativeViewmodelHandsOnlySideInfo& sideInfo,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        vr_vm_stabilize::Mat3x4& outTarget)
    {
        if (!vr || !sourceBones || sideInfo.hand < 0 || sideInfo.hand >= numBones)
            return false;

        const bool leftUsesWeaponPose =
            sideInfo.side == -1 && vr->IsVrHandsTwoHandedGripPoseActive();
        const bool rightUsesWeaponPose =
            sideInfo.side == 1 &&
            !vr->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
        if (leftUsesWeaponPose || rightUsesWeaponPose)
        {
            return vr_vm_stabilize::SafeRead(
                sourceBones + sideInfo.hand,
                outTarget) &&
                HooksNativeViewmodelHandsOnlyMatrixFinite(outTarget);
        }

        vr_vm_stabilize::Mat3x4 ignoredDelta{};
        return HooksNativeViewmodelHandsOnlyTryBuildSideTargetDelta(
            vr,
            sideInfo,
            sourceBones,
            numBones,
            ignoredDelta,
            &outTarget);
    }

    inline bool HooksNativeViewmodelArmIkApplyArm(
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelArmIkChain& chain,
        const Vector& shoulderTarget,
        const vr_vm_stabilize::Mat3x4& handTarget,
        const Vector& bodyForward,
        const Vector& bodyRight,
        const Vector& bodyUp,
        const Vector* previousBendDirection,
        bool stretchUpperArmToTarget,
        vr_vm_stabilize::Mat3x4* bones,
        Vector& outBendDirection,
        const float* previousTwistRadians = nullptr,
        float* outTwistRadians = nullptr,
        bool stabilizeNonStretchingArm = false,
        float sharedUpperArmStretchScale = 1.0f,
        bool enforceAnatomicalBend = false,
        const Vector* anatomicalPoleBias = nullptr,
        const char** outFailureStage = nullptr)
    {
        if (outFailureStage)
            *outFailureStage = nullptr;
        auto fail = [&](const char* stage) -> bool
            {
                if (outFailureStage)
                    *outFailureStage = stage;
                return false;
            };
        const bool stabilizeArmState =
            stretchUpperArmToTarget || stabilizeNonStretchingArm;
        if (outTwistRadians)
            *outTwistRadians = 0.0f;
        if (!bones || chain.side == 0 || chain.upperArm < 0 ||
            chain.forearm < 0 || chain.hand < 0 ||
            chain.upperArm >= numBones || chain.forearm >= numBones ||
            chain.hand >= numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(handTarget))
        {
            return fail("input");
        }

        const Vector currentShoulder =
            vr_vm_stabilize::GetOrigin(bones[chain.upperArm]);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(currentShoulder) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(shoulderTarget))
        {
            return fail("shoulder-input");
        }

        vr_vm_stabilize::Mat3x4 shoulderTranslation =
            vr_vm_stabilize::Identity();
        const Vector shoulderDelta = shoulderTarget - currentShoulder;
        shoulderTranslation.m[0][3] = shoulderDelta.x;
        shoulderTranslation.m[1][3] = shoulderDelta.y;
        shoulderTranslation.m[2][3] = shoulderDelta.z;
        if (!HooksNativeViewmodelArmIkApplyDeltaToBranch(
            boneParents,
            numBones,
            chain.upperArm,
            shoulderTranslation,
            bones))
        {
            return fail("shoulder-translate");
        }

        const Vector shoulder =
            vr_vm_stabilize::GetOrigin(bones[chain.upperArm]);
        const Vector animatedElbow =
            vr_vm_stabilize::GetOrigin(bones[chain.forearm]);
        const Vector animatedHand =
            vr_vm_stabilize::GetOrigin(bones[chain.hand]);
        const Vector animatedUpperDirection = animatedElbow - shoulder;
        const float upperLength = animatedUpperDirection.Length();
        const float lowerLength = (animatedHand - animatedElbow).Length();
        const Vector targetPosition = vr_vm_stabilize::GetOrigin(handTarget);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(shoulder) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(animatedElbow) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(animatedHand) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(targetPosition) ||
            !std::isfinite(upperLength) || !std::isfinite(lowerLength) ||
            upperLength < 0.25f || lowerLength < 0.25f ||
            upperLength > 256.0f || lowerLength > 256.0f)
        {
            return fail("rest-geometry");
        }

        float solveUpperLength = upperLength;
        if (stretchUpperArmToTarget)
        {
            if (std::isfinite(sharedUpperArmStretchScale) &&
                sharedUpperArmStretchScale > 1.0f)
            {
                solveUpperLength *= sharedUpperArmStretchScale;
            }

            const float targetDistance = (targetPosition - shoulder).Length();
            if (!std::isfinite(targetDistance) || targetDistance < 0.01f)
                return fail("target-distance");

            // Keep the authored forearm length. If the controller/weapon hand is
            // outside the normal two-bone reach, add only the missing distance to
            // the shoulder-to-elbow segment. The solver keeps a tiny non-zero bend
            // margin, so grow the upper length until its own maximum-reach formula
            // includes the real hand target.
            for (int iteration = 0; iteration < 3; ++iteration)
            {
                const float reachEpsilon = std::max(
                    0.01f,
                    (solveUpperLength + lowerLength) * 0.0005f);
                const float maximumReach =
                    solveUpperLength + lowerLength - reachEpsilon;
                if (targetDistance <= maximumReach)
                    break;

                solveUpperLength +=
                    targetDistance - maximumReach + 0.001f;
            }

            const float finalReachEpsilon = std::max(
                0.01f,
                (solveUpperLength + lowerLength) * 0.0005f);
            if (!std::isfinite(solveUpperLength) ||
                solveUpperLength < upperLength ||
                targetDistance >
                solveUpperLength + lowerLength - finalReachEpsilon)
            {
                return fail("stretch-reach");
            }
        }

        // A tracked resting arm must bend primarily with gravity.  The old
        // side-dominant pole lifted the elbow outward when the hand was resting
        // in front of, or below, the torso; changing shoulder height could not
        // correct that bend plane.  Keep only a small outward/backward component
        // so a nearly vertical shoulder-to-hand line still has a stable pole
        // after the solver projects gravity out of that line.
        //
        // First- and third-person tracked arms can share one user-tuned local
        // pole. Callers that do not opt into the anatomical constraint retain
        // the historical pole for compatibility.
        Vector configuredPoleLocal(-0.05f, 0.20f, -1.0f);
        if (anatomicalPoleBias &&
            HooksNativeViewmodelHandsOnlyVectorFinite(*anatomicalPoleBias) &&
            DotProduct(*anatomicalPoleBias, *anatomicalPoleBias) > 0.000001f)
        {
            configuredPoleLocal = *anatomicalPoleBias;
        }
        const Vector poleDirection = enforceAnatomicalBend
            ? bodyForward * configuredPoleLocal.x +
                bodyRight * (configuredPoleLocal.y *
                    static_cast<float>(chain.side)) +
                bodyUp * configuredPoleLocal.z
            : bodyRight * static_cast<float>(chain.side) -
                bodyUp * 0.45f - bodyForward * 0.15f;

        // The previous elbow direction is continuity only; it must never become
        // the permanent pole. A large native fire/melee animation can move the
        // hand through the opposite bend hemisphere. Keeping that transient bend
        // as the next frame's pole makes the arm remain twisted after the stock
        // animation has returned. Re-center a strongly opposed previous bend to
        // the anatomical pole whenever that pole is well-defined.
        Vector sanitizedPreviousBend{};
        const Vector* previousBendForSolve = previousBendDirection;
        if (stabilizeArmState && previousBendDirection)
        {
            Vector targetDirection{};
            Vector previousProjected{};
            Vector poleProjected{};
            if (!HooksNativeViewmodelArmIkNormalize(
                *previousBendDirection,
                sanitizedPreviousBend))
            {
                previousBendForSolve = nullptr;
            }
            else if (HooksNativeViewmodelArmIkNormalize(
                targetPosition - shoulder,
                targetDirection) &&
                HooksNativeViewmodelArmIkProjectOntoPlane(
                    sanitizedPreviousBend,
                    targetDirection,
                    previousProjected) &&
                HooksNativeViewmodelArmIkProjectOntoPlane(
                    poleDirection,
                    targetDirection,
                    poleProjected))
            {
                const Vector rawPoleProjection =
                    poleDirection -
                    targetDirection * DotProduct(
                        poleDirection,
                        targetDirection);
                const float poleLength = poleDirection.Length();
                const float projectedPoleLength = rawPoleProjection.Length();
                const float poleStrength =
                    std::isfinite(poleLength) && poleLength > 0.0001f &&
                    std::isfinite(projectedPoleLength)
                    ? projectedPoleLength / poleLength
                    : 0.0f;
                if (poleStrength >= 0.15f &&
                    DotProduct(previousProjected, poleProjected) < 0.0f)
                {
                    sanitizedPreviousBend *= -1.0f;
                }
                previousBendForSolve = &sanitizedPreviousBend;
            }
            else
            {
                previousBendForSolve = &sanitizedPreviousBend;
            }
        }

        HooksNativeViewmodelArmIkSolution solution{};
        if (!HooksNativeViewmodelArmIkSolveTwoBone(
            shoulder,
            targetPosition,
            solveUpperLength,
            lowerLength,
            poleDirection,
            previousBendForSolve,
            animatedUpperDirection,
            enforceAnatomicalBend,
            solution))
        {
            return fail("two-bone");
        }
        const Vector solvedHandTarget =
            stretchUpperArmToTarget ? targetPosition : solution.hand;

        vr_vm_stabilize::Mat3x4 upperDelta{};
        if (!HooksNativeViewmodelArmIkBuildAlignmentDelta(
            shoulder,
            animatedUpperDirection,
            solution.elbow - shoulder,
            poleDirection,
            upperDelta) ||
            !HooksNativeViewmodelArmIkApplyDeltaToBranch(
                boneParents,
                numBones,
                chain.upperArm,
                upperDelta,
                bones))
        {
            return fail("upper-align");
        }

        Vector solvedElbow =
            vr_vm_stabilize::GetOrigin(bones[chain.forearm]);
        Vector solvedHandBeforeForearm =
            vr_vm_stabilize::GetOrigin(bones[chain.hand]);

        if (!stabilizeArmState)
        {
            // Legacy animation-seeded arms may still need a secondary plane
            // correction. Stable first-person and third-person rest-chain solves
            // deliberately skip it: near full extension both plane normals become
            // ill-conditioned, so tracker noise can flip their sign every frame and
            // make the upper arm oscillate violently.
            Vector currentPlaneNormal = CrossProduct(
                solvedElbow - shoulder,
                solvedHandBeforeForearm - solvedElbow);
            Vector desiredPlaneNormal = CrossProduct(
                solution.elbow - shoulder,
                solvedHandTarget - solution.elbow);
            const bool currentPlaneValid =
                HooksNativeViewmodelArmIkNormalize(
                    currentPlaneNormal,
                    currentPlaneNormal);
            const bool desiredPlaneValid =
                HooksNativeViewmodelArmIkNormalize(
                    desiredPlaneNormal,
                    desiredPlaneNormal);
            if (currentPlaneValid && desiredPlaneValid)
            {
                vr_vm_stabilize::Mat3x4 upperTwistDelta{};
                if (!HooksNativeViewmodelArmIkBuildAlignmentDelta(
                    shoulder,
                    currentPlaneNormal,
                    desiredPlaneNormal,
                    solution.elbow - shoulder,
                    upperTwistDelta) ||
                    !HooksNativeViewmodelArmIkApplyDeltaToBranch(
                        boneParents,
                        numBones,
                        chain.upperArm,
                        upperTwistDelta,
                        bones))
                {
                    return fail("upper-plane-align");
                }

                solvedElbow =
                    vr_vm_stabilize::GetOrigin(bones[chain.forearm]);
                solvedHandBeforeForearm =
                    vr_vm_stabilize::GetOrigin(bones[chain.hand]);
            }
        }

        if (stretchUpperArmToTarget)
        {
            // Rotating the authored upper arm preserves its original length. Move
            // the complete forearm branch to the analytic elbow instead, which
            // lengthens only the upper-arm segment while preserving every
            // elbow-to-hand distance in the forearm branch.
            const Vector elbowDelta = solution.elbow - solvedElbow;
            vr_vm_stabilize::Mat3x4 upperStretchTranslation =
                vr_vm_stabilize::Identity();
            upperStretchTranslation.m[0][3] = elbowDelta.x;
            upperStretchTranslation.m[1][3] = elbowDelta.y;
            upperStretchTranslation.m[2][3] = elbowDelta.z;
            if (!HooksNativeViewmodelArmIkApplyDeltaToBranch(
                boneParents,
                numBones,
                chain.forearm,
                upperStretchTranslation,
                bones))
            {
                return fail("upper-stretch");
            }

            solvedElbow =
                vr_vm_stabilize::GetOrigin(bones[chain.forearm]);
            solvedHandBeforeForearm =
                vr_vm_stabilize::GetOrigin(bones[chain.hand]);
        }

        vr_vm_stabilize::Mat3x4 forearmDelta{};
        if (!HooksNativeViewmodelArmIkBuildAlignmentDelta(
            solvedElbow,
            solvedHandBeforeForearm - solvedElbow,
            solvedHandTarget - solvedElbow,
            poleDirection,
            forearmDelta) ||
            !HooksNativeViewmodelArmIkApplyDeltaToBranch(
                boneParents,
                numBones,
                chain.forearm,
                forearmDelta,
                bones))
        {
            return fail("forearm-align");
        }

        vr_vm_stabilize::Mat3x4 desiredHandRigid{};
        vr_vm_stabilize::Mat3x4 currentHandRigid{};
        if (!HooksViewmodelAutoGripNormalizeRigidMatrix(
            handTarget,
            desiredHandRigid) ||
            !HooksViewmodelAutoGripNormalizeRigidMatrix(
                bones[chain.hand],
                currentHandRigid))
        {
            return fail("hand-rigid");
        }

        // Applying the complete controller rotation only at the hand leaves the
        // lower arm static and twists the wrist skin into a corkscrew. Extract
        // the signed controller roll around the solved elbow-to-wrist axis and
        // apply it to the whole forearm branch before welding the palm. Direction
        // plus this roll fully determines the forearm, so Source's native
        // fire/reload/locomotion forearm rotation cannot survive the IK solve.
        vr_vm_stabilize::Mat3x4 forearmTwistDelta{};
        float principalTwistRadians = 0.0f;
        if (!HooksNativeViewmodelArmIkBuildForearmTwistDelta(
            solvedElbow,
            solvedHandTarget - solvedElbow,
            currentHandRigid,
            desiredHandRigid,
            forearmTwistDelta,
            &principalTwistRadians))
        {
            return fail("forearm-twist-build");
        }

        // Keep temporal twist in one bounded revolution. The previous unwrapped
        // accumulator could gain an extra 2*pi turn when a stock fire/melee
        // animation crossed the +/-pi boundary. Once wound up, a returned hand
        // pose was mapped to the same extra revolution and the upper arm stayed
        // twisted until the arm continuity state was reset.
        const float principalTwistBounded =
            HooksNativeViewmodelArmIkWrapRadians(principalTwistRadians);
        if (!std::isfinite(principalTwistBounded))
            return fail("forearm-twist-wrap");

        float stabilizedUpperTwistRadians = principalTwistBounded;
        if (stabilizeArmState && previousTwistRadians &&
            std::isfinite(*previousTwistRadians))
        {
            // The palm and forearm still use the exact current hand rotation.
            // Only the cosmetic upper-arm share is filtered, and its state is
            // always wrapped back to [-pi, pi]. A very large animation jump is
            // re-seeded immediately instead of being dragged through stale twist.
            // Keep these constants shared by first- and third-person arms. Using
            // a slower world-model filter made its upper arm visibly lag the same
            // controller roll, concentrating transient twist at the elbow.
            constexpr float kUpperTwistDeadbandRadians = 0.0035f;
            constexpr float kUpperTwistMaximumStepRadians = 0.3500f;
            constexpr float kUpperTwistFollow = 0.50f;
            constexpr float kUpperTwistReseedRadians = 2.09439510239f;
            const float previousBounded =
                HooksNativeViewmodelArmIkWrapRadians(*previousTwistRadians);
            float delta = HooksNativeViewmodelArmIkWrapRadians(
                principalTwistBounded - previousBounded);
            if (!std::isfinite(previousBounded) || !std::isfinite(delta))
                return fail("forearm-twist-continuity");

            if (std::fabs(delta) >= kUpperTwistReseedRadians)
            {
                stabilizedUpperTwistRadians = principalTwistBounded;
            }
            else if (std::fabs(delta) <= kUpperTwistDeadbandRadians)
            {
                stabilizedUpperTwistRadians = previousBounded;
            }
            else
            {
                delta = std::clamp(
                    delta,
                    -kUpperTwistMaximumStepRadians,
                    kUpperTwistMaximumStepRadians);
                stabilizedUpperTwistRadians =
                    HooksNativeViewmodelArmIkWrapRadians(
                        previousBounded + delta * kUpperTwistFollow);
            }
        }
        if (outTwistRadians)
            *outTwistRadians = stabilizedUpperTwistRadians;

        if (stabilizeArmState &&
            std::fabs(stabilizedUpperTwistRadians) > 0.000001f)
        {
            // There are no dedicated twist bones. Keep the full roll on the
            // forearm so the palm reaches the tracked orientation, and rotate
            // only the upper-arm absolute matrix by a visual share of the stable
            // roll. Child positions stay solved while elbow skinning shares the
            // twist. First- and third-person must use exactly the same share: a
            // smaller world-model share produces a different elbow crease for the
            // same tracked palm. Preserve most of the share near full extension
            // as well; dropping it to zero leaves the entire roll discontinuity at
            // the elbow precisely when that joint has the least geometry to hide
            // the deformation.
            constexpr float kPi = 3.14159265358979323846f;
            constexpr float kUpperArmTwistShare = 0.75f;
            constexpr float kStraightArmTwistWeight = 0.80f;
            constexpr float kMaximumUpperArmTwist = kPi * 0.75f;

            Vector normalizedUpper{};
            Vector normalizedLower{};
            float elbowBendWeight = 0.0f;
            if (HooksNativeViewmodelArmIkNormalize(
                solvedElbow - shoulder,
                normalizedUpper) &&
                HooksNativeViewmodelArmIkNormalize(
                    solvedHandTarget - solvedElbow,
                    normalizedLower))
            {
                const float bendSine = CrossProduct(
                    normalizedUpper,
                    normalizedLower).Length();
                if (std::isfinite(bendSine))
                {
                    constexpr float bendStart = 0.03f;
                    constexpr float bendEnd = 0.25f;
                    elbowBendWeight = std::clamp(
                        (bendSine - bendStart) /
                            (bendEnd - bendStart),
                        0.0f,
                        1.0f);
                    // Smoothstep prevents tiny tracking noise around the
                    // straight-arm threshold from switching upper-arm roll on
                    // and off from one frame to the next.
                    elbowBendWeight =
                        elbowBendWeight * elbowBendWeight *
                        (3.0f - 2.0f * elbowBendWeight);
                    elbowBendWeight =
                        kStraightArmTwistWeight +
                        (1.0f - kStraightArmTwistWeight) *
                            elbowBendWeight;
                }
            }

            const float upperArmTwistRadians = std::clamp(
                stabilizedUpperTwistRadians *
                kUpperArmTwistShare * elbowBendWeight,
                -kMaximumUpperArmTwist,
                kMaximumUpperArmTwist);

            vr_vm_stabilize::Mat3x4 upperArmTwistDelta{};
            if (!HooksNativeViewmodelArmIkBuildAxisRotationDelta(
                shoulder,
                solvedElbow - shoulder,
                upperArmTwistRadians,
                upperArmTwistDelta) ||
                !HooksNativeViewmodelArmIkApplyDeltaToSingleBone(
                    numBones,
                    chain.upperArm,
                    upperArmTwistDelta,
                    bones))
            {
                return fail("upper-twist-share");
            }
        }

        if (!HooksNativeViewmodelArmIkApplyDeltaToBranch(
            boneParents,
            numBones,
            chain.forearm,
            forearmTwistDelta,
            bones) ||
            !HooksViewmodelAutoGripNormalizeRigidMatrix(
                bones[chain.hand],
                currentHandRigid))
        {
            return fail("forearm-twist-apply");
        }

        if (stretchUpperArmToTarget)
        {
            // The forearm solve now reaches the real weapon/controller target with
            // its authored length. Correct any floating-point residue by moving the
            // whole forearm branch, never the hand alone, so the lower segment is
            // not stretched. The remaining hand delta is rotation-only.
            const Vector currentHandPosition =
                vr_vm_stabilize::GetOrigin(bones[chain.hand]);
            const Vector handPositionDelta =
                targetPosition - currentHandPosition;
            if (!HooksNativeViewmodelHandsOnlyVectorFinite(currentHandPosition) ||
                !HooksNativeViewmodelHandsOnlyVectorFinite(handPositionDelta))
            {
                return fail("hand-weld-input");
            }

            vr_vm_stabilize::Mat3x4 handWeldTranslation =
                vr_vm_stabilize::Identity();
            handWeldTranslation.m[0][3] = handPositionDelta.x;
            handWeldTranslation.m[1][3] = handPositionDelta.y;
            handWeldTranslation.m[2][3] = handPositionDelta.z;
            if (!HooksNativeViewmodelArmIkApplyDeltaToBranch(
                boneParents,
                numBones,
                chain.forearm,
                handWeldTranslation,
                bones) ||
                !HooksViewmodelAutoGripNormalizeRigidMatrix(
                    bones[chain.hand],
                    currentHandRigid))
            {
                return fail("hand-weld-translate");
            }

            const Vector weldedHandPosition =
                vr_vm_stabilize::GetOrigin(currentHandRigid);
            desiredHandRigid.m[0][3] = weldedHandPosition.x;
            desiredHandRigid.m[1][3] = weldedHandPosition.y;
            desiredHandRigid.m[2][3] = weldedHandPosition.z;
        }
        else
        {
            // Third-person keeps both authored segment lengths and uses the
            // analytic reachable endpoint.
            desiredHandRigid.m[0][3] = solvedHandTarget.x;
            desiredHandRigid.m[1][3] = solvedHandTarget.y;
            desiredHandRigid.m[2][3] = solvedHandTarget.z;
        }

        vr_vm_stabilize::Mat3x4 inverseCurrentHand{};
        vr_vm_stabilize::Mat3x4 handDelta{};
        vr_vm_stabilize::InvertTR(currentHandRigid, inverseCurrentHand);
        vr_vm_stabilize::Mul(desiredHandRigid, inverseCurrentHand, handDelta);
        if (!HooksNativeViewmodelArmIkApplyDeltaToBranch(
            boneParents,
            numBones,
            chain.hand,
            handDelta,
            bones))
        {
            return fail("palm-weld");
        }

        outBendDirection = solution.bendDirection;
        return HooksNativeViewmodelHandsOnlyVectorFinite(outBendDirection)
            ? true
            : fail("bend-output");
    }

    struct HooksNativeViewmodelFirstPersonArmRestCache
    {
        VR* owner = nullptr;
        const uint8_t* studioHdr = nullptr;
        std::string modelName;
        uint32_t boneLayoutSignature = 0;
        int numBones = 0;
        int side = 0;
        int upperArm = -1;
        int forearm = -1;
        int hand = -1;
        bool valid = false;
        vr_vm_stabilize::Mat3x4 canonicalUpperWorld{};
        std::vector<uint8_t> solveMask;
        std::vector<uint8_t> localValid;
        std::vector<vr_vm_stabilize::Mat3x4> restLocalBones;

        void Reset()
        {
            owner = nullptr;
            studioHdr = nullptr;
            modelName.clear();
            boneLayoutSignature = 0;
            numBones = 0;
            side = 0;
            upperArm = -1;
            forearm = -1;
            hand = -1;
            valid = false;
            canonicalUpperWorld = vr_vm_stabilize::Mat3x4{};
            solveMask.clear();
            localValid.clear();
            restLocalBones.clear();
        }
    };

    inline HooksNativeViewmodelFirstPersonArmRestCache&
        HooksNativeViewmodelFirstPersonArmRestCacheInstance(int side)
    {
        static HooksNativeViewmodelFirstPersonArmRestCache leftCache;
        static HooksNativeViewmodelFirstPersonArmRestCache rightCache;
        return side < 0 ? leftCache : rightCache;
    }

    inline bool HooksNativeViewmodelArmIkResetFirstPersonBranchToRestPose(
        VR* vr,
        void* drawState,
        int boneIndex,
        int stride,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelArmIkChain& chain,
        const std::vector<uint8_t>& solveMask,
        const Vector& shoulderTarget,
        const Vector& bodyForward,
        const vr_vm_stabilize::Mat3x4& anchorRotationDelta,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!vr || !drawState || !bones || chain.side == 0 ||
            chain.upperArm < 0 || chain.upperArm >= numBones ||
            chain.forearm < 0 || chain.forearm >= numBones ||
            chain.hand < 0 || chain.hand >= numBones ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones ||
            static_cast<int>(solveMask.size()) < numBones ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(shoulderTarget) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(bodyForward) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(anchorRotationDelta))
        {
            return false;
        }

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(
            drawState,
            studioHdr) ||
            !studioHdr)
        {
            return false;
        }

        const uint32_t boneLayoutSignature =
            HooksNativeViewmodelHandsOnlyBuildBoneLayoutSignature(
                boneNames,
                boneParents,
                numBones);

        static std::mutex s_restCacheMutex;
        std::lock_guard<std::mutex> lock(s_restCacheMutex);
        HooksNativeViewmodelFirstPersonArmRestCache& cache =
            HooksNativeViewmodelFirstPersonArmRestCacheInstance(chain.side);

        const bool cacheMatches =
            cache.valid &&
            cache.owner == vr &&
            cache.studioHdr == studioHdr &&
            cache.modelName == lowerModel &&
            cache.boneLayoutSignature == boneLayoutSignature &&
            cache.numBones == numBones &&
            cache.side == chain.side &&
            cache.upperArm == chain.upperArm &&
            cache.forearm == chain.forearm &&
            cache.hand == chain.hand &&
            cache.solveMask == solveMask &&
            static_cast<int>(cache.localValid.size()) == numBones &&
            static_cast<int>(cache.restLocalBones.size()) == numBones;

        if (!cacheMatches)
        {
            cache.Reset();
            cache.owner = vr;
            cache.studioHdr = studioHdr;
            cache.modelName = lowerModel;
            cache.boneLayoutSignature = boneLayoutSignature;
            cache.numBones = numBones;
            cache.side = chain.side;
            cache.upperArm = chain.upperArm;
            cache.forearm = chain.forearm;
            cache.hand = chain.hand;
            cache.solveMask = solveMask;
            cache.localValid.assign(static_cast<size_t>(numBones), 0u);
            cache.restLocalBones.resize(static_cast<size_t>(numBones));

            // Build the upper-arm bind orientation from the complete rest-pose
            // ancestor chain. No current viewmodel/world-model matrix is sampled,
            // so HMD-driven body turns and Source animation cannot leave axial
            // rotation in the first-person upper arm.
            std::vector<int> ancestorPath;
            int current = chain.upperArm;
            for (int guard = 0;
                guard < numBones && current >= 0 && current < numBones;
                ++guard)
            {
                ancestorPath.push_back(current);
                const int parent = boneParents[static_cast<size_t>(current)];
                if (parent < 0 || parent >= numBones || parent == current)
                    break;
                current = parent;
            }
            if (ancestorPath.empty())
            {
                cache.Reset();
                return false;
            }

            vr_vm_stabilize::Mat3x4 canonical = vr_vm_stabilize::Identity();
            for (auto it = ancestorPath.rbegin(); it != ancestorPath.rend(); ++it)
            {
                vr_vm_stabilize::Mat3x4 local{};
                if (!HooksNativeViewmodelHandsOnlyReadBoneRestLocalTransform(
                    drawState,
                    boneIndex,
                    stride,
                    *it,
                    local) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(local))
                {
                    cache.Reset();
                    return false;
                }

                vr_vm_stabilize::Mat3x4 next{};
                vr_vm_stabilize::Mul(canonical, local, next);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(next))
                {
                    cache.Reset();
                    return false;
                }
                canonical = next;
            }
            cache.canonicalUpperWorld = canonical;

            int requiredLocals = 0;
            int capturedLocals = 0;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!solveMask[static_cast<size_t>(bone)] ||
                    bone == chain.upperArm)
                {
                    continue;
                }
                ++requiredLocals;

                vr_vm_stabilize::Mat3x4 local{};
                if (!HooksNativeViewmodelHandsOnlyReadBoneRestLocalTransform(
                    drawState,
                    boneIndex,
                    stride,
                    bone,
                    local) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(local))
                {
                    cache.Reset();
                    return false;
                }
                cache.restLocalBones[static_cast<size_t>(bone)] = local;
                cache.localValid[static_cast<size_t>(bone)] = 1u;
                ++capturedLocals;
            }

            cache.valid =
                requiredLocals > 0 &&
                capturedLocals == requiredLocals &&
                HooksNativeViewmodelHandsOnlyMatrixFinite(
                    cache.canonicalUpperWorld);
            if (!cache.valid)
            {
                cache.Reset();
                return false;
            }

            if (vr->m_VrHandsDebugLog)
            {
                Game::logMsg(
                    "[VR][ViewmodelArmIK] cached animation-free %s arm rest branch model=%s bones=%d",
                    chain.side < 0 ? "left" : "right",
                    lowerModel.c_str(),
                    capturedLocals + 1);
            }
        }

        // The body owns the immutable shoulder position, but the analytic arm
        // must start from the cropped viewmodel's own rigid bind orientation.
        // World-model upper-arm matrices may contain animation, mirroring or
        // authored scale that this solver was never designed to inherit.
        Vector horizontalBodyForward(
            bodyForward.x,
            bodyForward.y,
            0.0f);
        if (!HooksNativeViewmodelArmIkNormalize(
                horizontalBodyForward,
                horizontalBodyForward))
        {
            return false;
        }

        const float bodyYawRadians =
            std::atan2(horizontalBodyForward.y, horizontalBodyForward.x);
        vr_vm_stabilize::Mat3x4 bodyYawDelta{};
        if (!std::isfinite(bodyYawRadians) ||
            !HooksNativeViewmodelArmIkBuildAxisRotationDelta(
                Vector(0.0f, 0.0f, 0.0f),
                Vector(0.0f, 0.0f, 1.0f),
                bodyYawRadians,
                bodyYawDelta))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 armRootDelta{};
        vr_vm_stabilize::Mul(
            anchorRotationDelta,
            bodyYawDelta,
            armRootDelta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(armRootDelta))
            return false;

        vr_vm_stabilize::Mat3x4 upperWorld{};
        vr_vm_stabilize::Mul(
            armRootDelta,
            cache.canonicalUpperWorld,
            upperWorld);
        upperWorld.m[0][3] = shoulderTarget.x;
        upperWorld.m[1][3] = shoulderTarget.y;
        upperWorld.m[2][3] = shoulderTarget.z;
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(upperWorld))
            return false;
        bones[chain.upperArm] = upperWorld;

        std::vector<uint8_t> resolved(static_cast<size_t>(numBones), 0u);
        resolved[static_cast<size_t>(chain.upperArm)] = 1u;
        int expected = 1;
        int rebuilt = 1;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (solveMask[static_cast<size_t>(bone)] && bone != chain.upperArm)
                ++expected;
        }

        for (int pass = 0; pass < numBones && rebuilt < expected; ++pass)
        {
            bool progressed = false;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (bone == chain.upperArm ||
                    !solveMask[static_cast<size_t>(bone)] ||
                    resolved[static_cast<size_t>(bone)])
                {
                    continue;
                }

                const int parent = boneParents[static_cast<size_t>(bone)];
                if (parent < 0 || parent >= numBones || parent == bone ||
                    !solveMask[static_cast<size_t>(parent)] ||
                    !resolved[static_cast<size_t>(parent)] ||
                    !cache.localValid[static_cast<size_t>(bone)])
                {
                    continue;
                }

                vr_vm_stabilize::Mat3x4 world{};
                vr_vm_stabilize::Mul(
                    bones[parent],
                    cache.restLocalBones[static_cast<size_t>(bone)],
                    world);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(world))
                    return false;

                bones[bone] = world;
                resolved[static_cast<size_t>(bone)] = 1u;
                ++rebuilt;
                progressed = true;
            }

            if (!progressed)
                break;
        }

        return rebuilt == expected &&
            resolved[static_cast<size_t>(chain.forearm)] != 0u &&
            resolved[static_cast<size_t>(chain.hand)] != 0u;
    }

    inline bool HooksNativeViewmodelArmIkRestoreAnimatedFingerPose(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& sideInfo,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        vr_vm_stabilize::Mat3x4* currentBones)
    {
        if (!sourceBones || !currentBones ||
            (sideInfo.side != -1 && sideInfo.side != 1) ||
            sideInfo.hand < 0 || sideInfo.hand >= numBones ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        // Cropped hands do not rebuild a guessed list of finger names: the
        // complete hand subtree stays in the pose produced by that pipeline.
        // Do the same after moving the IK palm. Copy every local transform below
        // the hand, including unnamed palm/finger helpers, so replacement models
        // cannot leave missed bones in the open bind pose.
        std::vector<uint8_t> handSubtreeMask(
            static_cast<size_t>(numBones),
            0u);
        int expected = 0;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (bone == sideInfo.hand ||
                !HooksNativeViewmodelHandsOnlyIsAncestor(
                    boneParents,
                    bone,
                    sideInfo.hand,
                    numBones))
            {
                continue;
            }

            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(
                boneNames[static_cast<size_t>(bone)]);
            const int namedSide =
                HooksNativeViewmodelHandsOnlyBoneSide(lowerName);
            if (namedSide != 0 && namedSide != sideInfo.side)
                continue;

            handSubtreeMask[static_cast<size_t>(bone)] = 1u;
            ++expected;
        }

        if (expected == 0)
            return true;

        std::vector<uint8_t> localValid(static_cast<size_t>(numBones), 0u);
        std::vector<vr_vm_stabilize::Mat3x4> sourceLocal(
            static_cast<size_t>(numBones));
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!handSubtreeMask[static_cast<size_t>(bone)])
                continue;

            const int parent = boneParents[static_cast<size_t>(bone)];
            if (parent < 0 || parent >= numBones || parent == bone)
                return false;

            vr_vm_stabilize::Mat3x4 parentWorld{};
            vr_vm_stabilize::Mat3x4 boneWorld{};
            vr_vm_stabilize::Mat3x4 inverseParent{};
            vr_vm_stabilize::Mat3x4 local{};
            if (!vr_vm_stabilize::SafeRead(
                sourceBones + parent,
                parentWorld) ||
                !vr_vm_stabilize::SafeRead(
                    sourceBones + bone,
                    boneWorld) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(parentWorld) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(boneWorld) ||
                !vr_vm_stabilize::InvertAffine(
                    parentWorld,
                    inverseParent))
            {
                return false;
            }

            vr_vm_stabilize::Mul(inverseParent, boneWorld, local);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(local))
                return false;
            sourceLocal[static_cast<size_t>(bone)] = local;
            localValid[static_cast<size_t>(bone)] = 1u;
        }

        std::vector<uint8_t> resolved(static_cast<size_t>(numBones), 0u);
        int applied = 0;
        for (int pass = 0; pass < numBones && applied < expected; ++pass)
        {
            bool progressed = false;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!handSubtreeMask[static_cast<size_t>(bone)] ||
                    !localValid[static_cast<size_t>(bone)] ||
                    resolved[static_cast<size_t>(bone)])
                {
                    continue;
                }

                const int parent = boneParents[static_cast<size_t>(bone)];
                if (handSubtreeMask[static_cast<size_t>(parent)] &&
                    !resolved[static_cast<size_t>(parent)])
                {
                    continue;
                }
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                    currentBones[parent]))
                {
                    return false;
                }

                vr_vm_stabilize::Mat3x4 world{};
                vr_vm_stabilize::Mul(
                    currentBones[parent],
                    sourceLocal[static_cast<size_t>(bone)],
                    world);
                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(world))
                    return false;

                currentBones[bone] = world;
                resolved[static_cast<size_t>(bone)] = 1u;
                ++applied;
                progressed = true;
            }

            if (!progressed)
                break;
        }

        return applied == expected;
    }

    inline bool HooksNativeViewmodelArmIkApplyCroppedEmptyFingerPose(
        VR* vr,
        void* drawState,
        int boneIndex,
        int stride,
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int numBones,
        const HooksNativeViewmodelHandsOnlySideInfo& sideInfo,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        vr_vm_stabilize::Mat3x4* currentBones)
    {
        if (!vr || !drawState || !sourceBones || !currentBones ||
            (sideInfo.side != -1 && sideInfo.side != 1) ||
            sideInfo.hand < 0 || sideInfo.hand >= numBones ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 sourceHand{};
        vr_vm_stabilize::Mat3x4 solvedHand{};
        if (!vr_vm_stabilize::SafeRead(
            sourceBones + sideInfo.hand,
            sourceHand) ||
            !vr_vm_stabilize::SafeRead(
                currentBones + sideInfo.hand,
                solvedHand) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(sourceHand) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(solvedHand))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 inverseSourceHand{};
        vr_vm_stabilize::Mat3x4 targetDelta{};
        vr_vm_stabilize::InvertTR(sourceHand, inverseSourceHand);
        vr_vm_stabilize::Mul(solvedHand, inverseSourceHand, targetDelta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(targetDelta))
            return false;

        // Re-run the cropped-hand empty-state pipeline on a temporary copy.
        // Its saved/captured frozen pose is the authoritative empty-hand base.
        // Copy only the complete subtree below the solved IK palm, so the cropped
        // forearm data cannot overwrite the full-arm solution.
        static thread_local std::vector<vr_vm_stabilize::Mat3x4> croppedPose;
        croppedPose.resize(static_cast<size_t>(numBones));
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(source))
            {
                return false;
            }

            vr_vm_stabilize::Mat3x4 transformed{};
            vr_vm_stabilize::Mul(targetDelta, source, transformed);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(transformed))
                return false;
            croppedPose[static_cast<size_t>(bone)] = transformed;
        }

        const bool frozenPoseApplied =
            HooksNativeViewmodelHandsOnlyApplyFrozenSideHandPose(
                vr,
                drawState,
                boneIndex,
                stride,
                lowerModel,
                boneNames,
                boneParents,
                numBones,
                sideInfo,
                sourceBones,
                solvedHand,
                targetDelta,
                croppedPose.data(),
                nullptr);

        if (!frozenPoseApplied)
        {
            // During the map-load freeze delay, the arm branch is already in
            // stable bind locals. Apply curls directly to that frozen fallback;
            // never restore this frame's Source finger animation for an empty hand.
            HooksNativeViewmodelHandsOnlyApplyOpenVRFingerPose(
                vr,
                drawState,
                boneIndex,
                stride,
                boneNames,
                boneParents,
                numBones,
                sideInfo,
                sourceBones,
                currentBones);
            return true;
        }

        HooksNativeViewmodelHandsOnlyApplyOpenVRFingerPose(
            vr,
            drawState,
            boneIndex,
            stride,
            boneNames,
            boneParents,
            numBones,
            sideInfo,
            sourceBones,
            croppedPose.data());

        return HooksNativeViewmodelArmIkRestoreAnimatedFingerPose(
            boneNames,
            boneParents,
            numBones,
            sideInfo,
            croppedPose.data(),
            currentBones);
    }

    struct HooksNativeViewmodelArmIkContinuity
    {
        VR* owner = nullptr;
        const uint8_t* studioHdr = nullptr;
        int numBones = 0;
        int leftUpper = -1;
        int leftForearm = -1;
        int leftHand = -1;
        int rightUpper = -1;
        int rightForearm = -1;
        int rightHand = -1;
        uint32_t renderFrameSeq = 0;
        int lastEyeIndex = -1;
        std::chrono::steady_clock::time_point lastCall{};
        std::chrono::steady_clock::time_point lastSolved{};
        bool latestValid[2]{};
        bool frameValid[2]{};
        Vector latestTurnLocal[2]{};
        Vector frameTurnLocal[2]{};
        bool latestTwistValid[2]{};
        bool frameTwistValid[2]{};
        float latestTwistRadians[2]{};
        float frameTwistRadians[2]{};
        bool latestTargetValid[2]{};
        bool frameTargetValid[2]{};
        bool latestTargetUsesNativeAnimation[2]{};
        bool frameTargetUsesNativeAnimation[2]{};
        Vector latestTargetTurnLocal[2]{};
        Vector frameTargetTurnLocal[2]{};
        Vector anchorRotationOffsetDeg[2]{};
    };

    struct HooksNativeViewmodelArmIkFrozenPlacement
    {
        VR* owner = nullptr;
        uint32_t generation = 0;
        bool valid = false;
        Vector shoulderCenterOffsetMeters{};

        void Reset()
        {
            owner = nullptr;
            generation = 0;
            valid = false;
            shoulderCenterOffsetMeters = Vector{};
        }
    };

    inline int HooksNativeViewmodelArmIkSideSlot(int side)
    {
        return side < 0 ? 0 : 1;
    }

    inline void HooksNativeViewmodelArmIkLogOnce(
        const std::string& key,
        const char* format,
        const std::string& modelName,
        const HooksNativeViewmodelArmIkChain* left,
        const HooksNativeViewmodelArmIkChain* right,
        const std::vector<std::string>* boneNames)
    {
        static std::mutex s_logMutex;
        static std::unordered_map<std::string, bool> s_logged;
        {
            std::lock_guard<std::mutex> lock(s_logMutex);
            if (s_logged[key])
                return;
            s_logged[key] = true;
        }

        auto boneName = [&](int bone) -> const char*
            {
                if (!boneNames || bone < 0 || bone >= static_cast<int>(boneNames->size()))
                    return "<none>";
                return (*boneNames)[static_cast<size_t>(bone)].c_str();
            };
        if (left || right)
        {
            Game::logMsg(
                format,
                modelName.c_str(),
                left ? left->upperArm : -1,
                left ? boneName(left->upperArm) : "<none>",
                left ? left->forearm : -1,
                left ? boneName(left->forearm) : "<none>",
                left ? left->hand : -1,
                left ? boneName(left->hand) : "<none>",
                right ? right->upperArm : -1,
                right ? boneName(right->upperArm) : "<none>",
                right ? right->forearm : -1,
                right ? boneName(right->forearm) : "<none>",
                right ? right->hand : -1,
                right ? boneName(right->hand) : "<none>");
        }
        else
        {
            Game::logMsg(format, modelName.c_str());
        }
    }

    inline bool HooksNativeViewmodelBuildFullArmIkBones(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        void* pCustomBoneToWorld,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!HooksNativeViewmodelFullArmIkActive(vr) ||
            !drawState || !pCustomBoneToWorld)
        {
            return false;
        }

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsArmsOrHands(lowerModel))
            return false;
        if (!HooksNativeViewmodelFullArmControllerPairReady(vr))
            return false;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset) ||
            numBones <= 0 || numBones > 512 ||
            static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            HooksNativeViewmodelArmIkLogOnce(
                "layout|" + lowerModel,
                "[VR][ViewmodelArmIK] failed to read viewmodel arm skeleton model=%s",
                lowerModel,
                nullptr,
                nullptr,
                nullptr);
            return false;
        }

        const auto* sourceBones =
            reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        HooksNativeViewmodelHandsOnlySideInfo leftInfo{};
        HooksNativeViewmodelHandsOnlySideInfo rightInfo{};
        const bool haveLeftInfo = HooksNativeViewmodelHandsOnlyBuildSideInfo(
            vr,
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            sourceBones,
            lowerModel,
            -1,
            leftInfo);
        const bool haveRightInfo = HooksNativeViewmodelHandsOnlyBuildSideInfo(
            vr,
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            sourceBones,
            lowerModel,
            1,
            rightInfo);

        HooksNativeViewmodelArmIkChain leftChain{};
        HooksNativeViewmodelArmIkChain rightChain{};
        bool haveLeftChain = haveLeftInfo &&
            HooksNativeViewmodelArmIkFindChain(
                boneNames,
                boneParents,
                numBones,
                leftInfo,
                leftChain);
        bool haveRightChain = haveRightInfo &&
            HooksNativeViewmodelArmIkFindChain(
                boneNames,
                boneParents,
                numBones,
                rightInfo,
                rightChain);

        bool rejectedSharedBranch = false;
        if (haveLeftChain && haveRightInfo && rightInfo.hand >= 0 &&
            HooksNativeViewmodelHandsOnlyIsAncestor(
                boneParents,
                rightInfo.hand,
                leftChain.upperArm,
                numBones))
        {
            haveLeftChain = false;
            rejectedSharedBranch = true;
        }
        if (haveRightChain && haveLeftInfo && leftInfo.hand >= 0 &&
            HooksNativeViewmodelHandsOnlyIsAncestor(
                boneParents,
                leftInfo.hand,
                rightChain.upperArm,
                numBones))
        {
            haveRightChain = false;
            rejectedSharedBranch = true;
        }
        // A replacement rig may label a common parent as one side's upper arm.
        // Solving that wider branch after the other side would rotate both arms,
        // producing the asymmetric right-controller-drives-left-arm failure.
        // Reject an arm root that owns the opposite arm root before any solve.
        if (haveLeftChain && haveRightChain)
        {
            const bool leftOwnsRightRoot =
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    boneParents,
                    rightChain.upperArm,
                    leftChain.upperArm,
                    numBones);
            const bool rightOwnsLeftRoot =
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    boneParents,
                    leftChain.upperArm,
                    rightChain.upperArm,
                    numBones);
            if (leftOwnsRightRoot && rightOwnsLeftRoot)
            {
                haveLeftChain = false;
                haveRightChain = false;
                rejectedSharedBranch = true;
            }
            else if (leftOwnsRightRoot)
            {
                haveLeftChain = false;
                rejectedSharedBranch = true;
            }
            else if (rightOwnsLeftRoot)
            {
                haveRightChain = false;
                rejectedSharedBranch = true;
            }
        }
        if (rejectedSharedBranch)
        {
            HooksNativeViewmodelArmIkLogOnce(
                "shared-root|" + lowerModel,
                "[VR][ViewmodelArmIK] rejected arm chain that also owns the opposite arm model=%s",
                lowerModel,
                nullptr,
                nullptr,
                nullptr);
        }
        if (!haveLeftChain || !haveRightChain)
        {
            HooksNativeViewmodelArmIkLogOnce(
                "atomic-chain|" + lowerModel,
                "[VR][ViewmodelArmIK] atomic pair rejected because one shoulder-elbow-hand chain is missing model=%s",
                lowerModel,
                nullptr,
                nullptr,
                nullptr);
            return false;
        }

        std::vector<uint8_t> leftSolveMask(static_cast<size_t>(numBones), 0u);
        std::vector<uint8_t> rightSolveMask(static_cast<size_t>(numBones), 0u);
        auto buildSolveMask = [&](
            const HooksNativeViewmodelArmIkChain& chain,
            std::vector<uint8_t>& mask) -> bool
            {
                if (chain.side == 0 || chain.upperArm < 0 ||
                    chain.forearm < 0 || chain.hand < 0)
                {
                    return false;
                }

                for (int bone = 0; bone < numBones; ++bone)
                {
                    if (!HooksNativeViewmodelHandsOnlyIsAncestor(
                        boneParents,
                        bone,
                        chain.upperArm,
                        numBones))
                    {
                        continue;
                    }

                    // Explicitly opposite-side descendants never belong to this
                    // solve even when a malformed replacement rig parents them
                    // below the wrong upper-arm node.
                    const int namedSide = HooksNativeViewmodelHandsOnlyBoneSide(
                        vr_vm_stabilize::ToLowerAscii(
                            boneNames[static_cast<size_t>(bone)]));
                    if (namedSide != 0 && namedSide != chain.side)
                        continue;
                    mask[static_cast<size_t>(bone)] = 1u;
                }

                return mask[static_cast<size_t>(chain.upperArm)] != 0u &&
                    mask[static_cast<size_t>(chain.forearm)] != 0u &&
                    mask[static_cast<size_t>(chain.hand)] != 0u;
            };

        if (haveLeftChain && !buildSolveMask(leftChain, leftSolveMask))
            haveLeftChain = false;
        if (haveRightChain && !buildSolveMask(rightChain, rightSolveMask))
            haveRightChain = false;

        if (haveLeftChain && haveRightChain)
        {
            bool masksOverlap = false;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (leftSolveMask[static_cast<size_t>(bone)] != 0u &&
                    rightSolveMask[static_cast<size_t>(bone)] != 0u)
                {
                    masksOverlap = true;
                    break;
                }
            }
            if (masksOverlap)
            {
                HooksNativeViewmodelArmIkLogOnce(
                    "overlap|" + lowerModel,
                    "[VR][ViewmodelArmIK] rejected overlapping left/right arm branches model=%s",
                    lowerModel,
                    nullptr,
                    nullptr,
                    nullptr);
                return false;
            }
        }
        if (!haveLeftChain || !haveRightChain)
            return false;

        vr_vm_stabilize::Mat3x4 leftTarget{};
        vr_vm_stabilize::Mat3x4 rightTarget{};
        const bool haveLeftTarget = haveLeftChain &&
            HooksNativeViewmodelArmIkBuildHandTarget(
                vr,
                leftInfo,
                sourceBones,
                numBones,
                leftTarget);
        const bool haveRightTarget = haveRightChain &&
            HooksNativeViewmodelArmIkBuildHandTarget(
                vr,
                rightInfo,
                sourceBones,
                numBones,
                rightTarget);
        if (!haveLeftTarget || !haveRightTarget)
        {
            HooksNativeViewmodelArmIkLogOnce(
                "atomic-target|" + lowerModel,
                "[VR][ViewmodelArmIK] atomic pair rejected because one hand target is missing model=%s",
                lowerModel,
                nullptr,
                nullptr,
                nullptr);
            return false;
        }

        Vector viewOrigin{};
        Vector bodyForward{};
        Vector bodyRight{};
        Vector bodyUp{};
        if (!HooksNativeViewmodelArmIkResolveBodyFrame(
            vr,
            viewOrigin,
            bodyForward,
            bodyRight,
            bodyUp))
        {
            return false;
        }

        Vector turnForward{};
        Vector turnRight{};
        Vector turnUp{};
        if (!HooksNativeViewmodelArmIkResolveTurnFrame(
            vr,
            turnForward,
            turnRight,
            turnUp))
        {
            return false;
        }

        // The no-body fallback stays on the artificial turn frame. When the
        // first-person survivor body is active, replace that frame with the
        // deadzone-resolved torso yaw. Raw physical HMD yaw is never consumed
        // directly by the arm IK.
        Vector armFrameForward = turnForward;
        Vector armFrameRight = turnRight;
        Vector armFrameUp = turnUp;
        const HooksFirstPersonBodyEyeSceneState* firstPersonBodyState = nullptr;
        const bool haveFirstPersonBodyFrame =
            HooksFirstPersonBodyCurrentSceneWantsViewmodelSkeleton(
                vr,
                firstPersonBodyState);
        if (haveFirstPersonBodyFrame && firstPersonBodyState)
        {
            const float visualBodyYaw = HooksTrackedBodyResolveVisualYaw(
                vr,
                firstPersonBodyState->view.angles.y);
            if (std::isfinite(visualBodyYaw))
            {
                Vector torsoForward{};
                Vector torsoRight{};
                Vector torsoUp{};
                QAngle::AngleVectors(
                    QAngle(0.0f, visualBodyYaw, 0.0f),
                    &torsoForward,
                    &torsoRight,
                    &torsoUp);
                if (HooksNativeViewmodelHandsOnlyVectorFinite(torsoForward) &&
                    HooksNativeViewmodelHandsOnlyVectorFinite(torsoRight) &&
                    HooksNativeViewmodelHandsOnlyVectorFinite(torsoUp))
                {
                    bodyForward = torsoForward;
                    bodyRight = torsoRight;
                    bodyUp = torsoUp;
                    armFrameForward = torsoForward;
                    armFrameRight = torsoRight;
                    armFrameUp = torsoUp;
                }
            }
        }

        const float sourceUnitsPerMeter =
            std::isfinite(vr->m_VRScale) && vr->m_VRScale > 1.0f
            ? vr->m_VRScale
            : 39.3701f;
        const Vector leftTargetOrigin = vr_vm_stabilize::GetOrigin(leftTarget);
        const Vector rightTargetOrigin = vr_vm_stabilize::GetOrigin(rightTarget);
        const float maximumTrackedTargetDistance = 2.0f * sourceUnitsPerMeter;
        const float leftTargetFromView = (leftTargetOrigin - viewOrigin).Length();
        const float rightTargetFromView = (rightTargetOrigin - viewOrigin).Length();
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(leftTargetOrigin) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(rightTargetOrigin) ||
            !std::isfinite(leftTargetFromView) ||
            !std::isfinite(rightTargetFromView) ||
            leftTargetFromView > maximumTrackedTargetDistance ||
            rightTargetFromView > maximumTrackedTargetDistance)
        {
            static std::mutex s_invalidTargetLogMutex;
            static std::unordered_set<std::string> s_invalidTargetLogged;
            bool shouldLog = false;
            {
                std::lock_guard<std::mutex> lock(s_invalidTargetLogMutex);
                shouldLog = s_invalidTargetLogged.insert(lowerModel).second;
            }
            if (shouldLog)
            {
                Game::logMsg(
                    "[VR][ViewmodelArmIK] rejected out-of-range hand target model=%s fromView=(%.2f %.2f) max=%.2f left=(%.2f %.2f %.2f) right=(%.2f %.2f %.2f)",
                    lowerModel.c_str(),
                    leftTargetFromView,
                    rightTargetFromView,
                    maximumTrackedTargetDistance,
                    leftTargetOrigin.x,
                    leftTargetOrigin.y,
                    leftTargetOrigin.z,
                    rightTargetOrigin.x,
                    rightTargetOrigin.y,
                    rightTargetOrigin.z);
            }
            return false;
        }
        Vector shoulderCenter =
            viewOrigin - bodyForward * (0.08f * sourceUnitsPerMeter) -
            bodyUp * (0.24f * sourceUnitsPerMeter);
        Vector shoulderLateralAxis = bodyRight;
        const float shoulderHalfWidth = 0.18f * sourceUnitsPerMeter;

        // The arm viewmodel retains the survivor's complete skeleton even though
        // most of its mesh was deleted. Match that skeleton to the final anchored
        // first-person body by bone name. This supplies the exact spine/clavicle
        // ancestry and upper-arm root matrices; no shoulder position is inferred
        // from HMD offsets, fixed widths or user tuning while the body is active.
        std::vector<std::string> bodyBoneNames;
        std::vector<vr_vm_stabilize::Mat3x4> bodyBones;
        std::vector<int> bodyBoneForViewmodel(
            static_cast<size_t>(numBones),
            -1);
        std::vector<uint8_t> attachedAncestorMask(
            static_cast<size_t>(numBones),
            0u);
        std::vector<vr_vm_stabilize::Mat3x4> attachedAncestorBones(
            static_cast<size_t>(numBones));
        Vector attachedUpperArmOrigin[2]{};
        bool attachedUpperArmValid[2]{};
        bool haveAttachedBodySkeleton = false;
        if (haveFirstPersonBodyFrame)
        {
            if (!HooksFirstPersonBodyReadViewmodelSkeleton(
                    vr,
                    firstPersonBodyState,
                    bodyBoneNames,
                    bodyBones))
            {
                return false;
            }

            std::unordered_map<std::string, int> bodyBoneByName;
            for (int bone = 0;
                bone < static_cast<int>(bodyBoneNames.size());
                ++bone)
            {
                const std::string key = vr_vm_stabilize::ToLowerAscii(
                    bodyBoneNames[static_cast<size_t>(bone)]);
                if (!key.empty() && bodyBoneByName.find(key) == bodyBoneByName.end())
                    bodyBoneByName.emplace(key, bone);
            }
            for (int bone = 0; bone < numBones; ++bone)
            {
                const std::string key = vr_vm_stabilize::ToLowerAscii(
                    boneNames[static_cast<size_t>(bone)]);
                const auto found = bodyBoneByName.find(key);
                if (found != bodyBoneByName.end())
                    bodyBoneForViewmodel[static_cast<size_t>(bone)] = found->second;
            }

            auto attachSide = [&] (
                int slot,
                bool chainValid,
                const HooksNativeViewmodelArmIkChain& chain) -> bool
                {
                    if (!chainValid)
                        return true;
                    if (slot < 0 || slot > 1 || chain.upperArm < 0 ||
                        chain.upperArm >= numBones)
                    {
                        return false;
                    }

                    const int bodyUpperArm = bodyBoneForViewmodel[
                        static_cast<size_t>(chain.upperArm)];
                    if (bodyUpperArm < 0 ||
                        bodyUpperArm >= static_cast<int>(bodyBones.size()) ||
                        !HooksNativeViewmodelHandsOnlyMatrixFinite(
                            bodyBones[static_cast<size_t>(bodyUpperArm)]))
                    {
                        return false;
                    }
                    // The body supplies the hard shoulder joint position. Its
                    // complete upper-arm orientation deliberately does not enter
                    // the viewmodel solver; the latter rebuilds a rigid bind-pose
                    // branch at this exact point below.
                    attachedUpperArmOrigin[slot] = vr_vm_stabilize::GetOrigin(
                        bodyBones[static_cast<size_t>(bodyUpperArm)]);
                    if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                            attachedUpperArmOrigin[slot]))
                    {
                        return false;
                    }
                    attachedUpperArmValid[slot] = true;

                    // Sleeve vertices near the seam may still carry clavicle or
                    // chest weights. Copy every matching ancestor as well, so
                    // those weights use the same body matrices as the torso mesh.
                    int current = boneParents[
                        static_cast<size_t>(chain.upperArm)];
                    for (int guard = 0;
                        guard < numBones && current >= 0 && current < numBones;
                        ++guard)
                    {
                        const int bodyBone = bodyBoneForViewmodel[
                            static_cast<size_t>(current)];
                        if (bodyBone >= 0 &&
                            bodyBone < static_cast<int>(bodyBones.size()) &&
                            HooksNativeViewmodelHandsOnlyMatrixFinite(
                                bodyBones[static_cast<size_t>(bodyBone)]))
                        {
                            attachedAncestorMask[
                                static_cast<size_t>(current)] = 1u;
                            attachedAncestorBones[
                                static_cast<size_t>(current)] =
                                bodyBones[static_cast<size_t>(bodyBone)];
                        }

                        const int parent = boneParents[
                            static_cast<size_t>(current)];
                        if (parent < 0 || parent >= numBones || parent == current)
                            break;
                        current = parent;
                    }
                    return true;
                };

            if (!attachSide(0, haveLeftChain, leftChain) ||
                !attachSide(1, haveRightChain, rightChain))
            {
                HooksNativeViewmodelArmIkLogOnce(
                    "body-skeleton-mismatch|" + lowerModel,
                    "[VR][ViewmodelArmIK] body/viewmodel skeleton does not share matching upper-arm roots model=%s",
                    lowerModel,
                    nullptr,
                    nullptr,
                    nullptr);
                return false;
            }
            haveAttachedBodySkeleton =
                (!haveLeftChain || attachedUpperArmValid[0]) &&
                (!haveRightChain || attachedUpperArmValid[1]);
        }

        if (haveAttachedBodySkeleton)
        {
            const Vector attachedCenter =
                (attachedUpperArmOrigin[0] + attachedUpperArmOrigin[1]) * 0.5f;
            const float attachedSeparation =
                (attachedUpperArmOrigin[1] - attachedUpperArmOrigin[0]).Length();
            const float attachedCenterFromView = (attachedCenter - viewOrigin).Length();
            const float attachedLeftReach =
                (leftTargetOrigin - attachedUpperArmOrigin[0]).Length();
            const float attachedRightReach =
                (rightTargetOrigin - attachedUpperArmOrigin[1]).Length();
            const float minimumShoulderSeparation = 0.08f * sourceUnitsPerMeter;
            const float maximumShoulderSeparation = 0.80f * sourceUnitsPerMeter;
            const float maximumShoulderCenterDistance = 1.25f * sourceUnitsPerMeter;
            const float maximumAttachedArmReach = 2.0f * sourceUnitsPerMeter;
            const bool attachedGeometryPlausible =
                std::isfinite(attachedSeparation) &&
                std::isfinite(attachedCenterFromView) &&
                std::isfinite(attachedLeftReach) &&
                std::isfinite(attachedRightReach) &&
                attachedSeparation >= minimumShoulderSeparation &&
                attachedSeparation <= maximumShoulderSeparation &&
                attachedCenterFromView <= maximumShoulderCenterDistance &&
                attachedLeftReach <= maximumAttachedArmReach &&
                attachedRightReach <= maximumAttachedArmReach;
            if (!attachedGeometryPlausible)
            {
                static std::mutex s_invalidAttachedShoulderLogMutex;
                static std::unordered_set<std::string> s_invalidAttachedShoulderLogged;
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(
                        s_invalidAttachedShoulderLogMutex);
                    shouldLog = s_invalidAttachedShoulderLogged.insert(
                        lowerModel).second;
                }
                if (shouldLog)
                {
                    Game::logMsg(
                        "[VR][ViewmodelArmIK] rejected incompatible body shoulder attachment; using torso fallback model=%s separation=%.2f centerFromView=%.2f reach=(%.2f %.2f) left=(%.2f %.2f %.2f) right=(%.2f %.2f %.2f)",
                        lowerModel.c_str(),
                        attachedSeparation,
                        attachedCenterFromView,
                        attachedLeftReach,
                        attachedRightReach,
                        attachedUpperArmOrigin[0].x,
                        attachedUpperArmOrigin[0].y,
                        attachedUpperArmOrigin[0].z,
                        attachedUpperArmOrigin[1].x,
                        attachedUpperArmOrigin[1].y,
                        attachedUpperArmOrigin[1].z);
                }

                haveAttachedBodySkeleton = false;
                attachedUpperArmValid[0] = false;
                attachedUpperArmValid[1] = false;
                std::fill(
                    attachedAncestorMask.begin(),
                    attachedAncestorMask.end(),
                    0u);
            }
        }

        // NativeViewmodelArm shares the cropped-hand map delay. Once that delay
        // is ready, preserve the selected shoulder centre in the moving torso
        // frame rather than keying it by StudioHdr/model. World movement and body
        // yaw still follow tracking, while switching character or arm models can
        // no longer resample a different bind-pose origin. Map/character changes
        // advance the shared generation and deliberately allow one new capture.
        const uint32_t placementGeneration =
            vr->m_NativeViewmodelLeftHandFreezeGeneration.load(
                std::memory_order_acquire);
        static std::mutex s_frozenPlacementMutex;
        static HooksNativeViewmodelArmIkFrozenPlacement s_frozenPlacement;
        if (!haveAttachedBodySkeleton)
        {
            std::lock_guard<std::mutex> lock(s_frozenPlacementMutex);
            if (s_frozenPlacement.owner != vr ||
                s_frozenPlacement.generation != placementGeneration)
            {
                s_frozenPlacement.Reset();
                s_frozenPlacement.owner = vr;
                s_frozenPlacement.generation = placementGeneration;
            }

            if (!s_frozenPlacement.valid &&
                vr->m_NativeViewmodelLeftHandFreezeReady.load(
                    std::memory_order_acquire) != 0u)
            {
                const Vector centerFromView = shoulderCenter - viewOrigin;
                const float inverseUnitsPerMeter = 1.0f / sourceUnitsPerMeter;
                const Vector capturedOffsetMeters(
                    DotProduct(centerFromView, armFrameForward) * inverseUnitsPerMeter,
                    DotProduct(centerFromView, armFrameRight) * inverseUnitsPerMeter,
                    DotProduct(centerFromView, armFrameUp) * inverseUnitsPerMeter);
                if (HooksNativeViewmodelHandsOnlyVectorFinite(
                    capturedOffsetMeters))
                {
                    s_frozenPlacement.shoulderCenterOffsetMeters =
                        capturedOffsetMeters;
                    s_frozenPlacement.valid = true;
                    if (vr->m_VrHandsDebugLog)
                    {
                        Game::logMsg(
                            "[VR][ViewmodelArmIK] froze shared shoulder centre offset=(%.4f %.4f %.4f)m generation=%u model=%s",
                            capturedOffsetMeters.x,
                            capturedOffsetMeters.y,
                            capturedOffsetMeters.z,
                            placementGeneration,
                            lowerModel.c_str());
                    }
                }
            }

            if (s_frozenPlacement.valid)
            {
                const Vector offsetMeters =
                    s_frozenPlacement.shoulderCenterOffsetMeters;
                shoulderCenter =
                    viewOrigin +
                    armFrameForward *
                        (offsetMeters.x * sourceUnitsPerMeter) +
                    armFrameRight *
                        (offsetMeters.y * sourceUnitsPerMeter) +
                    armFrameUp *
                        (offsetMeters.z * sourceUnitsPerMeter);
            }
        }

        Vector leftShoulder = attachedUpperArmValid[0]
            ? attachedUpperArmOrigin[0]
            : shoulderCenter - shoulderLateralAxis * shoulderHalfWidth;
        Vector rightShoulder = attachedUpperArmValid[1]
            ? attachedUpperArmOrigin[1]
            : shoulderCenter + shoulderLateralAxis * shoulderHalfWidth;
        bool leftShoulderValid = !haveLeftChain ||
            HooksNativeViewmodelHandsOnlyVectorFinite(leftShoulder);
        bool rightShoulderValid = !haveRightChain ||
            HooksNativeViewmodelHandsOnlyVectorFinite(rightShoulder);

        // User tuning is applied after the body-aligned shoulders have been
        // selected. Position offsets are independent per side and use the
        // resolved torso frame: X=forward, Y=right, Z=up. They remain available
        // with an attached body so replacement meshes can be aligned manually.
        const Vector configuredAnchorOffsetMeters[2] = {
            vr->m_NativeViewmodelLeftArmAnchorOffsetMeters,
            vr->m_NativeViewmodelRightArmAnchorOffsetMeters,
        };
        const Vector leftAnchorOffsetWorld =
            armFrameForward *
                (configuredAnchorOffsetMeters[0].x * sourceUnitsPerMeter) +
            armFrameRight *
                (configuredAnchorOffsetMeters[0].y * sourceUnitsPerMeter) +
            armFrameUp *
                (configuredAnchorOffsetMeters[0].z * sourceUnitsPerMeter);
        const Vector rightAnchorOffsetWorld =
            armFrameForward *
                (configuredAnchorOffsetMeters[1].x * sourceUnitsPerMeter) +
            armFrameRight *
                (configuredAnchorOffsetMeters[1].y * sourceUnitsPerMeter) +
            armFrameUp *
                (configuredAnchorOffsetMeters[1].z * sourceUnitsPerMeter);
        if (leftShoulderValid)
            leftShoulder += leftAnchorOffsetWorld;
        if (rightShoulderValid)
            rightShoulder += rightAnchorOffsetWorld;

        // Spacing tuning uses the same resolved torso frame as the shoulder
        // roots. It never re-reads animated survivor shoulder orientation.
        const Vector shoulderRightAxis = armFrameRight;
        const float spacingHalfDelta =
            std::clamp(
                vr->m_NativeViewmodelArmShoulderSpacingOffsetMeters,
                -0.5f,
                0.5f) *
            sourceUnitsPerMeter * 0.5f;
        if (leftShoulderValid)
            leftShoulder -= shoulderRightAxis * spacingHalfDelta;
        if (rightShoulderValid)
            rightShoulder += shoulderRightAxis * spacingHalfDelta;

        // Each arm gets its own shoulder-root orientation for the animation-free
        // rest branch. The anatomical elbow pole deliberately stays on the
        // unrotated torso frame below: manual pitch/yaw/roll may align sleeve
        // orientation, but cannot rotate the elbow into the opposite hemisphere.
        // The hard body shoulder origin and controller/weapon palm target remain
        // unchanged.
        const Vector configuredAnchorRotationOffsetDeg[2] = {
            vr->m_NativeViewmodelLeftArmAnchorRotationOffsetDeg,
            vr->m_NativeViewmodelRightArmAnchorRotationOffsetDeg,
        };
        Vector armSolveForward[2]{};
        Vector armSolveRight[2]{};
        Vector armSolveUp[2]{};
        const Vector armAnatomicalForward[2] = {
            armFrameForward, armFrameForward };
        const Vector armAnatomicalRight[2] = {
            armFrameRight, armFrameRight };
        const Vector armAnatomicalUp[2] = {
            armFrameUp, armFrameUp };
        vr_vm_stabilize::Mat3x4 armAnchorRotationDelta[2]{};
        for (int slot = 0; slot < 2; ++slot)
        {
            if (!HooksNativeViewmodelArmIkBuildAnchorRotation(
                armFrameForward,
                armFrameRight,
                armFrameUp,
                configuredAnchorRotationOffsetDeg[slot],
                armSolveForward[slot],
                armSolveRight[slot],
                armSolveUp[slot],
                armAnchorRotationDelta[slot]))
            {
                return false;
            }
        }

        // Shoulder roots and palm targets are hard endpoints, but reach extension
        // is strictly local to each arm. Sharing the farther hand's scale with the
        // nearer side changes that side's segment lengths while its target stays
        // fixed, forcing the unaffected elbow to fold or flip merely because the
        // opposite controller moved.
        float armUpperLength[2] = { 0.0f, 0.0f };
        float armLowerLength[2] = { 0.0f, 0.0f };
        float armTargetDistance[2] = { 0.0f, 0.0f };
        float armRequiredStretchScale[2] = { 1.0f, 1.0f };
        bool armGeometryValid[2] = { false, false };
        auto accumulateRequiredStretch = [&] (
            int slot,
            bool chainValid,
            bool targetValid,
            const HooksNativeViewmodelArmIkChain& chain,
            const vr_vm_stabilize::Mat3x4& target,
            const Vector& shoulder)
            {
                if (slot < 0 || slot > 1 || !chainValid || !targetValid ||
                    chain.upperArm < 0 || chain.upperArm >= numBones ||
                    chain.forearm < 0 || chain.forearm >= numBones ||
                    chain.hand < 0 || chain.hand >= numBones)
                {
                    return;
                }

                vr_vm_stabilize::Mat3x4 sourceUpper{};
                vr_vm_stabilize::Mat3x4 sourceForearm{};
                vr_vm_stabilize::Mat3x4 sourceHand{};
                if (!vr_vm_stabilize::SafeRead(
                        sourceBones + chain.upperArm,
                        sourceUpper) ||
                    !vr_vm_stabilize::SafeRead(
                        sourceBones + chain.forearm,
                        sourceForearm) ||
                    !vr_vm_stabilize::SafeRead(
                        sourceBones + chain.hand,
                        sourceHand) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(sourceUpper) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(sourceForearm) ||
                    !HooksNativeViewmodelHandsOnlyMatrixFinite(sourceHand))
                {
                    return;
                }

                const float upperLength =
                    (vr_vm_stabilize::GetOrigin(sourceForearm) -
                        vr_vm_stabilize::GetOrigin(sourceUpper)).Length();
                const float lowerLength =
                    (vr_vm_stabilize::GetOrigin(sourceHand) -
                        vr_vm_stabilize::GetOrigin(sourceForearm)).Length();
                const float targetDistance =
                    (vr_vm_stabilize::GetOrigin(target) - shoulder).Length();
                if (!std::isfinite(upperLength) ||
                    !std::isfinite(lowerLength) ||
                    !std::isfinite(targetDistance) ||
                    upperLength < 0.25f || lowerLength < 0.25f ||
                    upperLength > 256.0f || lowerLength > 256.0f ||
                    targetDistance < 0.01f)
                {
                    return;
                }

                float requiredUpperLength = upperLength;
                for (int iteration = 0; iteration < 3; ++iteration)
                {
                    const float reachEpsilon = std::max(
                        0.01f,
                        (requiredUpperLength + lowerLength) * 0.0005f);
                    const float maximumReach =
                        requiredUpperLength + lowerLength - reachEpsilon;
                    if (targetDistance <= maximumReach)
                        break;
                    requiredUpperLength +=
                        targetDistance - maximumReach + 0.001f;
                }

                const float requiredScale = requiredUpperLength / upperLength;
                constexpr float kMaximumUpperArmStretchScale = 3.0f;
                if (!std::isfinite(requiredScale) || requiredScale < 1.0f ||
                    requiredScale > kMaximumUpperArmStretchScale)
                    return;

                armUpperLength[slot] = upperLength;
                armLowerLength[slot] = lowerLength;
                armTargetDistance[slot] = targetDistance;
                armRequiredStretchScale[slot] = requiredScale;
                armGeometryValid[slot] = true;
            };
        accumulateRequiredStretch(
            0,
            haveLeftChain && leftShoulderValid,
            haveLeftTarget,
            leftChain,
            leftTarget,
            leftShoulder);
        accumulateRequiredStretch(
            1,
            haveRightChain && rightShoulderValid,
            haveRightTarget,
            rightChain,
            rightTarget,
            rightShoulder);
        if (!armGeometryValid[0] || !armGeometryValid[1])
        {
            HooksNativeViewmodelArmIkLogOnce(
                "implausible-geometry|" + lowerModel,
                "[VR][ViewmodelArmIK] rejected implausible full-arm geometry model=%s",
                lowerModel,
                nullptr,
                nullptr,
                nullptr);
            return false;
        }

        const uint8_t* studioHdr = nullptr;
        vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr);
        static std::mutex s_continuityMutex;
        static HooksNativeViewmodelArmIkContinuity s_continuity;

        bool previousValid[2]{};
        Vector previousWorld[2]{};
        bool previousTwistValid[2]{};
        float previousTwistRadians[2]{};
        const bool targetValid[2] = { haveLeftTarget, haveRightTarget };
        const bool targetUsesNativeAnimation[2] = {
            haveLeftTarget && vr->IsVrHandsTwoHandedGripPoseActive(),
            haveRightTarget &&
                !vr->m_ManualInventoryEmptyHandsActive.load(
                    std::memory_order_acquire),
        };
        const Vector targetShoulder[2] = { leftShoulder, rightShoulder };
        const vr_vm_stabilize::Mat3x4* targetMatrix[2] = {
            &leftTarget,
            &rightTarget,
        };
        Vector targetTurnLocal[2]{};
        bool targetTurnLocalValid[2]{};
        for (int slot = 0; slot < 2; ++slot)
        {
            if (!targetValid[slot])
                continue;
            const Vector targetRelative =
                vr_vm_stabilize::GetOrigin(*targetMatrix[slot]) -
                targetShoulder[slot];
            if (!HooksNativeViewmodelHandsOnlyVectorFinite(targetRelative))
                continue;
            targetTurnLocal[slot] =
                HooksNativeViewmodelArmIkWorldDirectionToBodyLocal(
                    targetRelative,
                    armAnatomicalForward[slot],
                    armAnatomicalRight[slot],
                    armAnatomicalUp[slot]);
            targetTurnLocalValid[slot] =
                HooksNativeViewmodelHandsOnlyVectorFinite(
                    targetTurnLocal[slot]);
        }
        const auto now = std::chrono::steady_clock::now();
        const uint32_t renderFrameSeq =
            vr->m_RenderFrameSeq.load(std::memory_order_acquire) & ~1u;
        const int eyeIndex = vr->m_VrHandsActiveEyeIndex;
        {
            std::lock_guard<std::mutex> lock(s_continuityMutex);
            const bool keyChanged =
                s_continuity.owner != vr ||
                s_continuity.studioHdr != studioHdr ||
                s_continuity.numBones != numBones ||
                s_continuity.leftUpper != (haveLeftChain ? leftChain.upperArm : -1) ||
                s_continuity.leftForearm != (haveLeftChain ? leftChain.forearm : -1) ||
                s_continuity.leftHand != (haveLeftChain ? leftChain.hand : -1) ||
                s_continuity.rightUpper != (haveRightChain ? rightChain.upperArm : -1) ||
                s_continuity.rightForearm != (haveRightChain ? rightChain.forearm : -1) ||
                s_continuity.rightHand != (haveRightChain ? rightChain.hand : -1) ||
                (s_continuity.anchorRotationOffsetDeg[0] -
                    configuredAnchorRotationOffsetDeg[0]).LengthSqr() > 0.0001f ||
                (s_continuity.anchorRotationOffsetDeg[1] -
                    configuredAnchorRotationOffsetDeg[1]).LengthSqr() > 0.0001f ||
                (s_continuity.lastSolved.time_since_epoch().count() != 0 &&
                    now - s_continuity.lastSolved > std::chrono::milliseconds(750));
            if (keyChanged)
            {
                s_continuity = HooksNativeViewmodelArmIkContinuity{};
                s_continuity.owner = vr;
                s_continuity.studioHdr = studioHdr;
                s_continuity.numBones = numBones;
                s_continuity.leftUpper = haveLeftChain ? leftChain.upperArm : -1;
                s_continuity.leftForearm = haveLeftChain ? leftChain.forearm : -1;
                s_continuity.leftHand = haveLeftChain ? leftChain.hand : -1;
                s_continuity.rightUpper = haveRightChain ? rightChain.upperArm : -1;
                s_continuity.rightForearm = haveRightChain ? rightChain.forearm : -1;
                s_continuity.rightHand = haveRightChain ? rightChain.hand : -1;
                s_continuity.anchorRotationOffsetDeg[0] =
                    configuredAnchorRotationOffsetDeg[0];
                s_continuity.anchorRotationOffsetDeg[1] =
                    configuredAnchorRotationOffsetDeg[1];
            }

            const bool timedEyeZeroFrame =
                eyeIndex == 0 &&
                s_continuity.lastCall.time_since_epoch().count() != 0 &&
                (s_continuity.lastEyeIndex != 0 ||
                    now - s_continuity.lastCall > std::chrono::milliseconds(2));
            if (keyChanged ||
                s_continuity.renderFrameSeq != renderFrameSeq ||
                timedEyeZeroFrame)
            {
                s_continuity.renderFrameSeq = renderFrameSeq;
                for (int slot = 0; slot < 2; ++slot)
                {
                    s_continuity.frameValid[slot] = s_continuity.latestValid[slot];
                    s_continuity.frameTurnLocal[slot] =
                        s_continuity.latestTurnLocal[slot];
                    s_continuity.frameTwistValid[slot] =
                        s_continuity.latestTwistValid[slot];
                    s_continuity.frameTwistRadians[slot] =
                        s_continuity.latestTwistRadians[slot];
                    s_continuity.frameTargetValid[slot] =
                        s_continuity.latestTargetValid[slot];
                    s_continuity.frameTargetUsesNativeAnimation[slot] =
                        s_continuity.latestTargetUsesNativeAnimation[slot];
                    s_continuity.frameTargetTurnLocal[slot] =
                        s_continuity.latestTargetTurnLocal[slot];
                }
            }
            s_continuity.lastEyeIndex = eyeIndex;
            s_continuity.lastCall = now;

            for (int slot = 0; slot < 2; ++slot)
            {
                previousValid[slot] = s_continuity.frameValid[slot];
                if (previousValid[slot])
                {
                    // Continuity uses the same frame as the shoulder roots. With
                    // the first-person body active this is the anchored torso frame;
                    // otherwise it remains the artificial turn frame. Head-only yaw
                    // therefore cannot wind elbow direction or upper-arm twist.
                    previousWorld[slot] =
                        HooksNativeViewmodelArmIkBodyLocalDirectionToWorld(
                            s_continuity.frameTurnLocal[slot],
                            armAnatomicalForward[slot],
                            armAnatomicalRight[slot],
                            armAnatomicalUp[slot]);
                    previousValid[slot] =
                        HooksNativeViewmodelArmIkNormalize(
                            previousWorld[slot],
                            previousWorld[slot]);
                }
                previousTwistValid[slot] =
                    s_continuity.frameTwistValid[slot] &&
                    std::isfinite(s_continuity.frameTwistRadians[slot]);
                if (previousTwistValid[slot])
                {
                    previousTwistRadians[slot] =
                        s_continuity.frameTwistRadians[slot];
                }

                // A stock weapon/melee animation may abruptly move the hand far
                // from the preceding frame. Do not feed elbow/twist continuity
                // across that discontinuity; the current frame is solved from
                // the animation-free rest branch and becomes the new seed. The
                // comparison is shoulder-relative turn space, so locomotion and
                // HMD translation do not create false resets.
                const float targetResetDistance =
                    0.15f * sourceUnitsPerMeter;
                const bool targetModeChanged =
                    s_continuity.frameTargetValid[slot] &&
                    targetTurnLocalValid[slot] &&
                    s_continuity.frameTargetUsesNativeAnimation[slot] !=
                    targetUsesNativeAnimation[slot];
                const bool targetJumped =
                    s_continuity.frameTargetValid[slot] &&
                    targetTurnLocalValid[slot] &&
                    (targetTurnLocal[slot] -
                        s_continuity.frameTargetTurnLocal[slot]).Length() >
                    targetResetDistance;
                if (targetModeChanged || targetJumped)
                {
                    previousValid[slot] = false;
                    previousTwistValid[slot] = false;
                }
            }
        }

        uint32_t seqEven = renderFrameSeq;
        if (seqEven == 0)
            seqEven = 2;
        vr_vm_stabilize::Mat3x4* solvedBones =
            vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!solvedBones)
            return false;
        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, solvedBones[bone]) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(solvedBones[bone]))
            {
                return false;
            }
        }
        if (haveAttachedBodySkeleton)
        {
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (attachedAncestorMask[static_cast<size_t>(bone)] != 0u)
                    solvedBones[bone] =
                        attachedAncestorBones[static_cast<size_t>(bone)];
            }
        }

        bool solvedSide[2]{};
        Vector solvedBendWorld[2]{};
        bool solvedTwistValid[2]{};
        float solvedTwistRadians[2]{};
        auto logSideSolveFailure = [&] (
            const HooksNativeViewmodelArmIkChain& chain,
            const char* stage)
            {
                static std::mutex s_failureLogMutex;
                static std::unordered_set<std::string> s_failureLogged;
                const char* const sideName = chain.side < 0 ? "left" : "right";
                const std::string key =
                    lowerModel + "|" + sideName + "|" +
                    (stage ? stage : "unknown");
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(s_failureLogMutex);
                    shouldLog = s_failureLogged.insert(key).second;
                }
                if (shouldLog)
                {
                    Game::logMsg(
                        "[VR][ViewmodelArmIK] %s arm solve failed stage=%s model=%s chain=(%d,%d,%d) bodySkeleton=%s",
                        sideName,
                        stage ? stage : "unknown",
                        lowerModel.c_str(),
                        chain.upperArm,
                        chain.forearm,
                        chain.hand,
                        haveAttachedBodySkeleton ? "attached" : "unavailable");
                }
            };
        auto solveSide = [&](
            bool chainValid,
            bool targetValid,
            const HooksNativeViewmodelArmIkChain& chain,
            const HooksNativeViewmodelHandsOnlySideInfo& sideInfo,
            const std::vector<uint8_t>& solveMask,
            const vr_vm_stabilize::Mat3x4& target,
            const Vector& shoulder) -> bool
            {
                if (!chainValid || !targetValid)
                    return false;

                const int slot = HooksNativeViewmodelArmIkSideSlot(chain.side);
                vr_vm_stabilize::Mat3x4* candidate =
                    vr_vm_stabilize::AllocStableBones(numBones, seqEven);
                if (!candidate)
                    return false;
                // Solve each side from the immutable engine pose. Starting the
                // right solve from the already-solved left arm made any accidental
                // hierarchy overlap one-way: rotating the right controller could
                // carry the left arm, while the reverse never occurred.
                for (int bone = 0; bone < numBones; ++bone)
                {
                    if (!vr_vm_stabilize::SafeRead(
                        sourceBones + bone,
                        candidate[bone]) ||
                        !HooksNativeViewmodelHandsOnlyMatrixFinite(candidate[bone]))
                    {
                        return false;
                    }
                }

                // Descendants use the retained viewmodel rest locals. The root
                // orientation stays in that rigid bind frame while shoulder is
                // the exact matching body joint position.
                if (!HooksNativeViewmodelArmIkResetFirstPersonBranchToRestPose(
                    vr,
                    drawState,
                    boneIndex,
                    stride,
                    lowerModel,
                    boneNames,
                    boneParents,
                    numBones,
                    chain,
                    solveMask,
                    shoulder,
                    armFrameForward,
                    armAnchorRotationDelta[slot],
                    candidate))
                {
                    logSideSolveFailure(chain, "reset-rest-branch");
                    return false;
                }

                std::vector<vr_vm_stabilize::Mat3x4> restCandidate;
                try
                {
                    restCandidate.assign(candidate, candidate + numBones);
                }
                catch (...)
                {
                    logSideSolveFailure(chain, "snapshot-rest-branch");
                    return false;
                }

                const Vector* previous = previousValid[slot]
                    ? &previousWorld[slot]
                    : nullptr;
                const float* previousTwist = previousTwistValid[slot]
                    ? &previousTwistRadians[slot]
                    : nullptr;
                Vector bend{};
                float twistRadians = 0.0f;
                const char* failureStage = nullptr;
                const float perSideUpperArmStretchScale =
                    armGeometryValid[slot]
                    ? armRequiredStretchScale[slot]
                    : 1.0f;
                bool armSolved = HooksNativeViewmodelArmIkApplyArm(
                    boneParents,
                    numBones,
                    chain,
                    shoulder,
                    target,
                    armAnatomicalForward[slot],
                    armAnatomicalRight[slot],
                    armAnatomicalUp[slot],
                    previous,
                    true,
                    candidate,
                    bend,
                    previousTwist,
                    &twistRadians,
                    false,
                    perSideUpperArmStretchScale,
                    true,
                    &vr->m_NativeViewmodelArmElbowPoleBias,
                    &failureStage);
                if (!armSolved)
                {
                    // Retry from the identical rigid rest branch without stale
                    // bend/twist continuity. Passing 1.0 lets ApplyArm derive this
                    // side's missing reach internally; neither opposite-arm state
                    // nor either hard endpoint participates.
                    std::copy(
                        restCandidate.begin(),
                        restCandidate.end(),
                        candidate);
                    bend = Vector{};
                    twistRadians = 0.0f;
                    const char* retryFailureStage = nullptr;
                    armSolved = HooksNativeViewmodelArmIkApplyArm(
                        boneParents,
                        numBones,
                        chain,
                        shoulder,
                        target,
                        armAnatomicalForward[slot],
                        armAnatomicalRight[slot],
                        armAnatomicalUp[slot],
                        nullptr,
                        true,
                        candidate,
                        bend,
                        nullptr,
                        &twistRadians,
                        false,
                        1.0f,
                        true,
                        &vr->m_NativeViewmodelArmElbowPoleBias,
                        &retryFailureStage);
                    if (!armSolved)
                    {
                        logSideSolveFailure(
                            chain,
                            retryFailureStage
                                ? retryFailureStage
                                : (failureStage ? failureStage : "analytic-unknown"));
                        return false;
                    }
                    if (vr->m_VrHandsDebugLog)
                    {
                        Game::logMsg(
                            "[VR][ViewmodelArmIK] %s arm recovered with independent stretch initialStage=%s sideScale=%.3f model=%s",
                            chain.side < 0 ? "left" : "right",
                            failureStage ? failureStage : "unknown",
                            perSideUpperArmStretchScale,
                            lowerModel.c_str());
                    }
                }
                if (!armSolved)
                {
                    logSideSolveFailure(chain, "analytic-unknown");
                    return false;
                }

                const bool emptyHandsPlaceholderActive =
                    vr->m_ManualInventoryEmptyHandsActive.load(
                        std::memory_order_acquire);
                const bool leftTwoHandedGrip =
                    chain.side == -1 &&
                    vr->IsVrHandsTwoHandedGripPoseActive();
                const bool useAnimatedFingerPose =
                    leftTwoHandedGrip ||
                    (chain.side == 1 && !emptyHandsPlaceholderActive);

                // Match cropped hands instead of treating every state alike:
                //   * weapon hand and two-handed support hand use this frame's
                //     native viewmodel finger animation;
                //   * free left hand and empty right hand use the cropped-hand
                //     frozen pose, then apply OpenVR curls on that fixed base.
                if (useAnimatedFingerPose)
                {
                    if (!HooksNativeViewmodelArmIkRestoreAnimatedFingerPose(
                        boneNames,
                        boneParents,
                        numBones,
                        sideInfo,
                        sourceBones,
                        candidate))
                    {
                        logSideSolveFailure(chain, "animated-fingers");
                        return false;
                    }
                }
                else if (!HooksNativeViewmodelArmIkApplyCroppedEmptyFingerPose(
                    vr,
                    drawState,
                    boneIndex,
                    stride,
                    lowerModel,
                    boneNames,
                    boneParents,
                    numBones,
                    sideInfo,
                    sourceBones,
                    candidate))
                {
                    logSideSolveFailure(chain, "cropped-empty-fingers");
                    return false;
                }

                // A compatible rest branch may be stretched, but never by more
                // than the guarded 3x arm scale above. Reject a replacement rig
                // whose helper/rest matrices amplify that solve into a skinning
                // spike; the caller then keeps the already-safe cropped-hand draw.
                for (int bone = 0; bone < numBones; ++bone)
                {
                    if (bone >= static_cast<int>(solveMask.size()) ||
                        solveMask[static_cast<size_t>(bone)] == 0u)
                    {
                        continue;
                    }

                    vr_vm_stabilize::Mat3x4 source{};
                    if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source) ||
                        !HooksNativeViewmodelHandsOnlyMatrixFinite(source) ||
                        !HooksNativeViewmodelHandsOnlyMatrixFinite(candidate[bone]))
                    {
                        logSideSolveFailure(chain, "output-matrix");
                        return false;
                    }

                    double sourceBasisLength = 0.0;
                    double outputBasisLength = 0.0;
                    for (int col = 0; col < 3; ++col)
                    {
                        double sourceLengthSqr = 0.0;
                        double outputLengthSqr = 0.0;
                        for (int row = 0; row < 3; ++row)
                        {
                            const double sourceValue = source.m[row][col];
                            const double outputValue = candidate[bone].m[row][col];
                            sourceLengthSqr += sourceValue * sourceValue;
                            outputLengthSqr += outputValue * outputValue;
                        }
                        sourceBasisLength = (std::max)(
                            sourceBasisLength,
                            std::sqrt(sourceLengthSqr));
                        outputBasisLength = (std::max)(
                            outputBasisLength,
                            std::sqrt(outputLengthSqr));
                    }
                    const double allowedBasisLength =
                        (std::max)(0.02, sourceBasisLength * 4.0);
                    if (!std::isfinite(sourceBasisLength) ||
                        !std::isfinite(outputBasisLength) ||
                        outputBasisLength > 16.0 ||
                        outputBasisLength > allowedBasisLength)
                    {
                        logSideSolveFailure(chain, "output-deformation");
                        return false;
                    }
                }

                for (int bone = 0; bone < numBones; ++bone)
                {
                    if (bone < static_cast<int>(solveMask.size()) &&
                        solveMask[static_cast<size_t>(bone)] != 0u)
                    {
                        solvedBones[bone] = candidate[bone];
                    }
                }
                solvedSide[slot] = true;
                solvedBendWorld[slot] = bend;
                solvedTwistValid[slot] = std::isfinite(twistRadians);
                solvedTwistRadians[slot] = twistRadians;
                return true;
            };

        solveSide(
            haveLeftChain && leftShoulderValid,
            haveLeftTarget,
            leftChain,
            leftInfo,
            leftSolveMask,
            leftTarget,
            leftShoulder);
        solveSide(
            haveRightChain && rightShoulderValid,
            haveRightTarget,
            rightChain,
            rightInfo,
            rightSolveMask,
            rightTarget,
            rightShoulder);

        if (!solvedSide[0] || !solvedSide[1])
        {
            HooksNativeViewmodelArmIkLogOnce(
                "atomic-solve|" + lowerModel,
                "[VR][ViewmodelArmIK] atomic pair discarded because one arm solve failed model=%s",
                lowerModel,
                nullptr,
                nullptr,
                nullptr);
            std::lock_guard<std::mutex> lock(s_continuityMutex);
            if (s_continuity.owner == vr &&
                s_continuity.studioHdr == studioHdr &&
                s_continuity.numBones == numBones)
            {
                for (int slot = 0; slot < 2; ++slot)
                {
                    s_continuity.latestValid[slot] = false;
                    s_continuity.latestTwistValid[slot] = false;
                    s_continuity.latestTargetValid[slot] = false;
                }
            }
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(s_continuityMutex);
            if (s_continuity.owner == vr &&
                s_continuity.studioHdr == studioHdr &&
                s_continuity.numBones == numBones)
            {
                for (int slot = 0; slot < 2; ++slot)
                {
                    if (!solvedSide[slot])
                    {
                        // Do not preserve one side's arm state through a draw in
                        // which that side could not be rebuilt. Otherwise the
                        // other arm keeps lastSolved fresh and a stale weapon
                        // animation seed can be reused when this side returns.
                        s_continuity.latestValid[slot] = false;
                        s_continuity.latestTwistValid[slot] = false;
                        s_continuity.latestTargetValid[slot] = false;
                        continue;
                    }
                    Vector world = solvedBendWorld[slot];
                    if (HooksNativeViewmodelArmIkNormalize(world, world))
                    {
                        Vector turnLocal =
                            HooksNativeViewmodelArmIkWorldDirectionToBodyLocal(
                                world,
                                armAnatomicalForward[slot],
                                armAnatomicalRight[slot],
                                armAnatomicalUp[slot]);
                        if (HooksNativeViewmodelArmIkNormalize(
                            turnLocal,
                            turnLocal))
                        {
                            s_continuity.latestTurnLocal[slot] = turnLocal;
                            s_continuity.latestValid[slot] = true;
                        }
                    }
                    if (solvedTwistValid[slot])
                    {
                        s_continuity.latestTwistRadians[slot] =
                            solvedTwistRadians[slot];
                        s_continuity.latestTwistValid[slot] = true;
                    }
                    if (targetTurnLocalValid[slot])
                    {
                        s_continuity.latestTargetTurnLocal[slot] =
                            targetTurnLocal[slot];
                        s_continuity.latestTargetUsesNativeAnimation[slot] =
                            targetUsesNativeAnimation[slot];
                        s_continuity.latestTargetValid[slot] = true;
                    }
                    else
                    {
                        s_continuity.latestTargetValid[slot] = false;
                    }
                }
                s_continuity.lastSolved = now;
            }
        }

        if (solvedSide[0])
        {
            HooksNativeViewmodelHandsOnlyPublishLeftWristAnchor(
                vr,
                boneNames,
                numBones,
                solvedBones);
        }
        if (solvedSide[0] != solvedSide[1])
        {
            HooksNativeViewmodelArmIkLogOnce(
                solvedSide[0]
                ? "partial-left|" + lowerModel
                : "partial-right|" + lowerModel,
                solvedSide[0]
                ? "[VR][ViewmodelArmIK] only the left viewmodel arm was solved model=%s"
                : "[VR][ViewmodelArmIK] only the right viewmodel arm was solved model=%s",
                lowerModel,
                nullptr,
                nullptr,
                nullptr);
        }
        if (solvedSide[0] && solvedSide[1] &&
            armGeometryValid[0] && armGeometryValid[1])
        {
            static std::mutex s_geometryLogMutex;
            static std::unordered_set<std::string> s_geometryLogged;
            bool shouldLogGeometry = false;
            {
                std::lock_guard<std::mutex> lock(s_geometryLogMutex);
                shouldLogGeometry = s_geometryLogged.insert(lowerModel).second;
            }
            if (shouldLogGeometry)
            {
                const float shoulderForwardDelta = DotProduct(
                    leftShoulder - rightShoulder,
                    armFrameForward);
                Game::logMsg(
                    "[VR][ViewmodelArmIK] independent stretch model=%s shoulderForwardDelta=%.3f upper=(%.2f %.2f) lower=(%.2f %.2f) targetDistance=(%.2f %.2f) sideScale=(%.3f %.3f)",
                    lowerModel.c_str(),
                    shoulderForwardDelta,
                    armUpperLength[0],
                    armUpperLength[1],
                    armLowerLength[0],
                    armLowerLength[1],
                    armTargetDistance[0],
                    armTargetDistance[1],
                    armRequiredStretchScale[0],
                    armRequiredStretchScale[1]);
            }
        }
        HooksNativeViewmodelArmIkLogOnce(
            "success|" + lowerModel,
            haveAttachedBodySkeleton
                ? "[VR][ViewmodelArmIK] active bodySkeleton=attached manualArmOffsets=active model=%s left=(%d:%s,%d:%s,%d:%s) right=(%d:%s,%d:%s,%d:%s)"
                : "[VR][ViewmodelArmIK] active bodySkeleton=unavailable cameraFallback=active model=%s left=(%d:%s,%d:%s,%d:%s) right=(%d:%s,%d:%s,%d:%s)",
            lowerModel,
            solvedSide[0] ? &leftChain : nullptr,
            solvedSide[1] ? &rightChain : nullptr,
            &boneNames);
        outBones = solvedBones;
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlyBuildSplitClipSets(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        void* pCustomBoneToWorld,
        std::vector<HooksNativeViewmodelHandsOnlyClipSet>& outClipSets)
    {
        outClipSets.clear();
        if (vr &&
            !vr->IsVrHandsTwoHandedGripPoseActive() &&
            !HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLock(vr))
        {
            HooksNativeViewmodelHandsOnlyResetFixedFreezePlaneLocks(vr);
        }
        const bool hidePendingFreeze =
            HooksNativeViewmodelHandsOnlyShouldHidePendingFreeze(vr);
        if (!HooksNativeViewmodelHandsOnlyActive(vr) ||
            HooksNativeViewmodelHandsOnlyHideArmsRequested(vr) ||
            (!HooksNativeViewmodelEffectiveArmCroppingEnabled(vr) &&
                !hidePendingFreeze) ||
            !drawState || !pCustomBoneToWorld)
            return false;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsArmsOrHands(lowerModel))
            return false;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return false;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones)
            return false;

        const auto* bones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        HooksNativeViewmodelHandsOnlySideInfo rightInfo{};
        HooksNativeViewmodelHandsOnlySideInfo leftInfo{};
        const bool hasRight = HooksNativeViewmodelHandsOnlyBuildSideInfo(
            vr, drawState, boneNames, boneParents, numBones, boneIndex, stride, bones, lowerModel, 1, rightInfo);
        const bool hasLeft = HooksNativeViewmodelHandsOnlyBuildSideInfo(
            vr, drawState, boneNames, boneParents, numBones, boneIndex, stride, bones, lowerModel, -1, leftInfo);
        if (hasRight && hasLeft)
        {
            rightInfo.oppositeSideValid = true;
            rightInfo.oppositeHandPos = leftInfo.handPos;
            rightInfo.oppositeAnchorPos = leftInfo.anchorPos;
            rightInfo.oppositeForearmPos = leftInfo.forearmPos;

            leftInfo.oppositeSideValid = true;
            leftInfo.oppositeHandPos = rightInfo.handPos;
            leftInfo.oppositeAnchorPos = rightInfo.anchorPos;
            leftInfo.oppositeForearmPos = rightInfo.forearmPos;
        }
        if (HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLock(vr))
        {
            float ignoredPlane[4]{};
            if (hasLeft && HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, leftInfo.side))
            {
                HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                    vr,
                    leftInfo,
                    lowerModel,
                    bones,
                    numBones,
                    nullptr,
                    nullptr,
                    ignoredPlane);
            }
            if (hasRight && HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, rightInfo.side))
            {
                HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                    vr,
                    rightInfo,
                    lowerModel,
                    bones,
                    numBones,
                    nullptr,
                    nullptr,
                    ignoredPlane);
            }
        }

        auto appendSet = [&](const HooksNativeViewmodelHandsOnlySideInfo& keepSide) -> bool
            {
                HooksNativeViewmodelHandsOnlyClipSet set{};
                set.side = keepSide.side;
                float wristPlaneWorld[4] = {
                    keepSide.wristPlaneWorld[0],
                    keepSide.wristPlaneWorld[1],
                    keepSide.wristPlaneWorld[2],
                    keepSide.wristPlaneWorld[3],
                };
                if (!HooksNativeViewmodelHandsOnlyBuildIsolatedSideBones(
                    vr,
                    drawState,
                    boneIndex,
                    stride,
                    lowerModel,
                    boneNames,
                    boneParents,
                    numBones,
                    bones,
                    keepSide,
                    set.isolatedBones,
                    wristPlaneWorld))
                {
                    return false;
                }
                if (!HooksNativeViewmodelHandsOnlyAppendWorldClipPlane(vr, wristPlaneWorld, set))
                    return false;
                const HooksNativeViewmodelHandsOnlySideInfo regionSide =
                    HooksNativeViewmodelHandsOnlyBuildFinalRegionSideInfo(
                        keepSide,
                        set.isolatedBones,
                        numBones,
                        wristPlaneWorld);
                HooksNativeViewmodelHandsOnlyAppendRegionClipPlanes(vr, regionSide, set);

                if (set.planeCount <= 0)
                    return false;
                outClipSets.push_back(set);
                return true;
            };

        auto appendHiddenSet = [&](const HooksNativeViewmodelHandsOnlySideInfo& hideSide) -> bool
            {
                HooksNativeViewmodelHandsOnlyClipSet set{};
                set.side = hideSide.side;
                float wristPlaneWorld[4] = {
                    hideSide.wristPlaneWorld[0],
                    hideSide.wristPlaneWorld[1],
                    hideSide.wristPlaneWorld[2],
                    hideSide.wristPlaneWorld[3],
                };
                if (HooksNativeViewmodelHandsOnlyShouldUseFixedFreezePlaneLockForSide(vr, hideSide.side))
                {
                    if (!HooksNativeViewmodelHandsOnlyTryResolveFixedFreezePlaneLock(
                        vr,
                        hideSide,
                        lowerModel,
                        bones,
                        numBones,
                        nullptr,
                        nullptr,
                        wristPlaneWorld))
                    {
                        return false;
                    }
                }
                if (!HooksNativeViewmodelHandsOnlyNormalizePlane(wristPlaneWorld))
                    return false;

                Vector normal(
                    wristPlaneWorld[0],
                    wristPlaneWorld[1],
                    wristPlaneWorld[2]);
                const float normalLen = normal.Length();
                if (!std::isfinite(normalLen) || normalLen < 0.001f)
                    return false;
                normal *= (1.0f / normalLen);
                const Vector planePoint = normal * (-wristPlaneWorld[3] / normalLen);
                const Vector hiddenOrigin = planePoint - (normal * 96.0f);
                const vr_vm_stabilize::Mat3x4 hiddenBone =
                    HooksNativeViewmodelHandsOnlyCollapsedBoneAt(hiddenOrigin);

                uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
                seqEven &= ~1u;
                if (seqEven == 0)
                    seqEven = 2;

                set.isolatedBones = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
                if (!set.isolatedBones)
                    return false;
                for (int bone = 0; bone < numBones; ++bone)
                    set.isolatedBones[bone] = hiddenBone;

                if (!HooksNativeViewmodelHandsOnlyAppendWorldClipPlane(vr, wristPlaneWorld, set))
                    return false;
                outClipSets.push_back(set);
                return true;
            };

        if (hidePendingFreeze)
        {
            if (hasLeft)
                appendHiddenSet(leftInfo);
            else if (hasRight)
                appendHiddenSet(rightInfo);
        }
        else if (hasRight && hasLeft)
        {
            appendSet(rightInfo);
            appendSet(leftInfo);
        }
        else if (hasRight)
        {
            appendSet(rightInfo);
        }
        else if (hasLeft)
        {
            appendSet(leftInfo);
        }

        return !outClipSets.empty();
    }

    inline IDirect3DDevice9* HooksNativeViewmodelHandsOnlyGetD3DDevice(VR* vr)
    {
        if (!vr)
            return nullptr;

        IDirect3DDevice9* device = nullptr;
        if (vr->m_D9LeftEyeSurface && SUCCEEDED(vr->m_D9LeftEyeSurface->GetDevice(&device)) && device)
            return device;
        if (vr->m_D9RightEyeSurface && SUCCEEDED(vr->m_D9RightEyeSurface->GetDevice(&device)) && device)
            return device;
        if (vr->m_D9HUDSurface && SUCCEEDED(vr->m_D9HUDSurface->GetDevice(&device)) && device)
            return device;
        return nullptr;
    }

    class ScopedNativeViewmodelHandsOnlyClipPlane
    {
    public:
        explicit ScopedNativeViewmodelHandsOnlyClipPlane(
            VR* vr,
            const HooksNativeViewmodelHandsOnlyClipSet& clipSet)
        {
            Activate(vr, clipSet);
        }

        ScopedNativeViewmodelHandsOnlyClipPlane(
            VR* vr,
            void* drawState,
            const std::string& modelName,
            void* pCustomBoneToWorld)
        {
            if (!HooksNativeViewmodelHandsOnlyActive(vr) ||
                HooksNativeViewmodelHandsOnlyHideArmsRequested(vr) ||
                !HooksNativeViewmodelEffectiveArmCroppingEnabled(vr) ||
                !pCustomBoneToWorld)
                return;

            float plane[4]{};
            if (!HooksNativeViewmodelHandsOnlyBuildClipPlane(vr, drawState, modelName, pCustomBoneToWorld, plane))
                return;

            HooksNativeViewmodelHandsOnlyClipSet clipSet{};
            clipSet.planeCount = 1;
            memcpy(clipSet.planes[0], plane, sizeof(plane));
            Activate(vr, clipSet);
        }

        ~ScopedNativeViewmodelHandsOnlyClipPlane()
        {
            if (m_Device)
            {
                if (m_Active)
                {
                    m_Device->SetRenderState(D3DRS_CLIPPLANEENABLE, m_OldClipEnable);
                    if (m_HasOldClipping)
                        m_Device->SetRenderState(D3DRS_CLIPPING, m_OldClipping);
                    for (int i = 0; i < kHooksNativeViewmodelHandsOnlyMaxClipPlanes; ++i)
                    {
                        if (m_HasOldPlane[i])
                            m_Device->SetClipPlane(static_cast<DWORD>(i), m_OldPlane[i]);
                    }
                }
                m_Device->Release();
            }
        }

        bool Active() const { return m_Active; }

    private:
        void Activate(VR* vr, const HooksNativeViewmodelHandsOnlyClipSet& clipSet)
        {
            if (!vr || clipSet.planeCount <= 0 ||
                clipSet.planeCount > kHooksNativeViewmodelHandsOnlyMaxClipPlanes)
                return;

            m_Device = HooksNativeViewmodelHandsOnlyGetD3DDevice(vr);
            if (!m_Device)
                return;

            if (FAILED(m_Device->GetRenderState(D3DRS_CLIPPLANEENABLE, &m_OldClipEnable)))
                return;
            m_HasOldClipping = SUCCEEDED(m_Device->GetRenderState(D3DRS_CLIPPING, &m_OldClipping));
            for (int i = 0; i < kHooksNativeViewmodelHandsOnlyMaxClipPlanes; ++i)
            {
                m_HasOldPlane[i] = SUCCEEDED(m_Device->GetClipPlane(static_cast<DWORD>(i), m_OldPlane[i]));
                if ((m_OldClipEnable & (1u << i)) && !m_HasOldPlane[i])
                    return;
            }

            DWORD newClipEnable = m_OldClipEnable;
            for (int i = 0; i < clipSet.planeCount; ++i)
            {
                if (FAILED(m_Device->SetClipPlane(static_cast<DWORD>(i), clipSet.planes[i])))
                    return;
                newClipEnable |= (1u << i);
            }
            if (FAILED(m_Device->SetRenderState(D3DRS_CLIPPLANEENABLE, newClipEnable)))
            {
                if (m_HasOldClipping)
                    m_Device->SetRenderState(D3DRS_CLIPPING, m_OldClipping);
                for (int i = 0; i < kHooksNativeViewmodelHandsOnlyMaxClipPlanes; ++i)
                {
                    if (m_HasOldPlane[i])
                        m_Device->SetClipPlane(static_cast<DWORD>(i), m_OldPlane[i]);
                }
                return;
            }

            m_Active = true;
        }

        IDirect3DDevice9* m_Device = nullptr;
        DWORD m_OldClipEnable = 0;
        DWORD m_OldClipping = TRUE;
        float m_OldPlane[kHooksNativeViewmodelHandsOnlyMaxClipPlanes][4]{};
        bool m_HasOldClipping = false;
        bool m_HasOldPlane[kHooksNativeViewmodelHandsOnlyMaxClipPlanes]{};
        bool m_Active = false;
    };

    inline bool HooksNativeViewmodelHandsOnlySourceQueueReadableProtection(DWORD protect)
    {
        if (protect & PAGE_GUARD)
            return false;

        switch (protect & 0xff)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    inline bool HooksNativeViewmodelHandsOnlySourceQueueReadableMemory(const void* ptr, size_t bytes)
    {
        if (!ptr || bytes == 0)
            return false;

        const uint8_t* p = reinterpret_cast<const uint8_t*>(ptr);
        size_t remaining = bytes;
        while (remaining > 0)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi)))
                return false;
            if (mbi.State != MEM_COMMIT ||
                !HooksNativeViewmodelHandsOnlySourceQueueReadableProtection(mbi.Protect))
            {
                return false;
            }

            const uintptr_t current = reinterpret_cast<uintptr_t>(p);
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= current)
                return false;

            const size_t chunk = std::min<size_t>(remaining, static_cast<size_t>(regionEnd - current));
            p += chunk;
            remaining -= chunk;
        }
        return true;
    }

    inline bool HooksNativeViewmodelHandsOnlySourceCallQueueUsable(ICallQueue* callQueue)
    {
        if (!HooksNativeViewmodelHandsOnlySourceQueueReadableMemory(callQueue, sizeof(void*)))
            return false;

        void* const vtable = *reinterpret_cast<void**>(callQueue);
        return HooksNativeViewmodelHandsOnlySourceQueueReadableMemory(vtable, sizeof(void*));
    }

    inline ICallQueue* HooksNativeViewmodelHandsOnlyGetSourceRenderCallQueue(IMatRenderContext* renderContext)
    {
        if (!HooksNativeViewmodelHandsOnlySourceQueueReadableMemory(renderContext, sizeof(void*)))
            return nullptr;

        void** const contextVtable = *reinterpret_cast<void***>(renderContext);
        constexpr size_t kGetCallQueueAbsoluteSlot = 142;
        if (!HooksNativeViewmodelHandsOnlySourceQueueReadableMemory(
            contextVtable + kGetCallQueueAbsoluteSlot,
            sizeof(void*)))
        {
            return nullptr;
        }

        void* const getCallQueuePtr = contextVtable[kGetCallQueueAbsoluteSlot];
        if (!HooksNativeViewmodelHandsOnlySourceQueueReadableMemory(getCallQueuePtr, 1))
            return nullptr;

        using GetCallQueueFn = ICallQueue * (__thiscall*)(IMatRenderContext*);
        ICallQueue* callQueue = reinterpret_cast<GetCallQueueFn>(getCallQueuePtr)(renderContext);
        return HooksNativeViewmodelHandsOnlySourceCallQueueUsable(callQueue) ? callQueue : nullptr;
    }

    struct HooksQueuedNativeViewmodelHandsOnlyClipState
    {
        std::atomic<long> refs{ 0 };
        VR* vr = nullptr;
        int planeCount = 0;
        float planes[kHooksNativeViewmodelHandsOnlyMaxClipPlanes][4]{};
        IDirect3DDevice9* device = nullptr;
        DWORD oldClipEnable = 0;
        DWORD oldClipping = TRUE;
        float oldPlanes[kHooksNativeViewmodelHandsOnlyMaxClipPlanes][4]{};
        bool hasOldClipping = false;
        bool hasOldPlane[kHooksNativeViewmodelHandsOnlyMaxClipPlanes]{};
        bool active = false;

        HooksQueuedNativeViewmodelHandsOnlyClipState(
            VR* inVr,
            const HooksNativeViewmodelHandsOnlyClipSet& clipSet)
            : vr(inVr),
            planeCount(std::clamp(
                clipSet.planeCount,
                0,
                kHooksNativeViewmodelHandsOnlyMaxClipPlanes))
        {
            for (int i = 0; i < planeCount; ++i)
                memcpy(planes[i], clipSet.planes[i], sizeof(planes[i]));
        }

        void AddRef()
        {
            refs.fetch_add(1, std::memory_order_acq_rel);
        }

        void Release()
        {
            const long remaining = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
        }

        void Begin()
        {
            if (active || !vr || planeCount <= 0)
                return;

            device = HooksNativeViewmodelHandsOnlyGetD3DDevice(vr);
            if (!device)
                return;

            if (FAILED(device->GetRenderState(D3DRS_CLIPPLANEENABLE, &oldClipEnable)))
            {
                device->Release();
                device = nullptr;
                return;
            }
            hasOldClipping = SUCCEEDED(device->GetRenderState(D3DRS_CLIPPING, &oldClipping));
            for (int i = 0; i < kHooksNativeViewmodelHandsOnlyMaxClipPlanes; ++i)
            {
                hasOldPlane[i] = SUCCEEDED(device->GetClipPlane(static_cast<DWORD>(i), oldPlanes[i]));
                if ((oldClipEnable & (1u << i)) && !hasOldPlane[i])
                {
                    device->Release();
                    device = nullptr;
                    return;
                }
            }

            DWORD newClipEnable = oldClipEnable;
            for (int i = 0; i < planeCount; ++i)
            {
                if (FAILED(device->SetClipPlane(static_cast<DWORD>(i), planes[i])))
                {
                    End();
                    return;
                }
                newClipEnable |= (1u << i);
            }
            if (FAILED(device->SetRenderState(D3DRS_CLIPPLANEENABLE, newClipEnable)))
            {
                End();
                return;
            }

            active = true;
        }

        void End()
        {
            if (!device)
                return;

            if (active)
            {
                device->SetRenderState(D3DRS_CLIPPLANEENABLE, oldClipEnable);
                if (hasOldClipping)
                    device->SetRenderState(D3DRS_CLIPPING, oldClipping);
                for (int i = 0; i < kHooksNativeViewmodelHandsOnlyMaxClipPlanes; ++i)
                {
                    if (hasOldPlane[i])
                        device->SetClipPlane(static_cast<DWORD>(i), oldPlanes[i]);
                }
                active = false;
            }

            device->Release();
            device = nullptr;
        }

        ~HooksQueuedNativeViewmodelHandsOnlyClipState()
        {
            End();
        }
    };

    class HooksQueuedNativeViewmodelHandsOnlyClipBeginFunctor final : public CFunctor
    {
    public:
        explicit HooksQueuedNativeViewmodelHandsOnlyClipBeginFunctor(HooksQueuedNativeViewmodelHandsOnlyClipState* state)
            : m_State(state)
        {
            if (m_State)
                m_State->AddRef();
        }

        ~HooksQueuedNativeViewmodelHandsOnlyClipBeginFunctor() override
        {
            if (m_State)
                m_State->Release();
        }

        int AddRef() override
        {
            return static_cast<int>(m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            if (m_State)
                m_State->Begin();
        }

    private:
        std::atomic<long> m_RefCount{ 0 };
        HooksQueuedNativeViewmodelHandsOnlyClipState* m_State = nullptr;
    };

    class HooksQueuedNativeViewmodelHandsOnlyClipEndFunctor final : public CFunctor
    {
    public:
        explicit HooksQueuedNativeViewmodelHandsOnlyClipEndFunctor(HooksQueuedNativeViewmodelHandsOnlyClipState* state)
            : m_State(state)
        {
            if (m_State)
                m_State->AddRef();
        }

        ~HooksQueuedNativeViewmodelHandsOnlyClipEndFunctor() override
        {
            if (m_State)
                m_State->Release();
        }

        int AddRef() override
        {
            return static_cast<int>(m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            if (m_State)
                m_State->End();
        }

    private:
        std::atomic<long> m_RefCount{ 0 };
        HooksQueuedNativeViewmodelHandsOnlyClipState* m_State = nullptr;
    };

    struct HooksQueuedNativeViewmodelDepthClampState
    {
        std::atomic<long> refs{ 1 };
        VR* vr = nullptr;
        IDirect3DDevice9* device = nullptr;
        DWORD oldClipping = TRUE;
        bool active = false;

        explicit HooksQueuedNativeViewmodelDepthClampState(VR* inVr)
            : vr(inVr)
        {
        }

        void AddRef()
        {
            refs.fetch_add(1, std::memory_order_acq_rel);
        }

        void Release()
        {
            const long remaining = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
        }

        void Begin()
        {
            if (active || !vr)
                return;

            device = HooksNativeViewmodelHandsOnlyGetD3DDevice(vr);
            if (!device)
                return;

            if (FAILED(device->GetRenderState(D3DRS_CLIPPING, &oldClipping)) ||
                FAILED(device->SetRenderState(D3DRS_CLIPPING, FALSE)))
            {
                device->Release();
                device = nullptr;
                return;
            }
            active = true;
        }

        void End()
        {
            if (!device)
                return;

            if (active)
            {
                device->SetRenderState(D3DRS_CLIPPING, oldClipping);
                active = false;
            }
            device->Release();
            device = nullptr;
        }

        ~HooksQueuedNativeViewmodelDepthClampState()
        {
            End();
        }
    };

    class HooksQueuedNativeViewmodelDepthClampBeginFunctor final : public CFunctor
    {
    public:
        explicit HooksQueuedNativeViewmodelDepthClampBeginFunctor(
            HooksQueuedNativeViewmodelDepthClampState* state)
            : m_State(state)
        {
            if (m_State)
                m_State->AddRef();
        }

        ~HooksQueuedNativeViewmodelDepthClampBeginFunctor() override
        {
            if (m_State)
                m_State->Release();
        }

        int AddRef() override
        {
            return static_cast<int>(m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            if (m_State)
                m_State->Begin();
        }

    private:
        std::atomic<long> m_RefCount{ 0 };
        HooksQueuedNativeViewmodelDepthClampState* m_State = nullptr;
    };

    class HooksQueuedNativeViewmodelDepthClampEndFunctor final : public CFunctor
    {
    public:
        explicit HooksQueuedNativeViewmodelDepthClampEndFunctor(
            HooksQueuedNativeViewmodelDepthClampState* state)
            : m_State(state)
        {
            if (m_State)
                m_State->AddRef();
        }

        ~HooksQueuedNativeViewmodelDepthClampEndFunctor() override
        {
            if (m_State)
                m_State->Release();
        }

        int AddRef() override
        {
            return static_cast<int>(m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            if (m_State)
                m_State->End();
        }

    private:
        std::atomic<long> m_RefCount{ 0 };
        HooksQueuedNativeViewmodelDepthClampState* m_State = nullptr;
    };

    class ScopedNativeViewmodelDepthClamp
    {
    public:
        ScopedNativeViewmodelDepthClamp(
            bool enabled,
            VR* vr,
            int queueMode,
            ICallQueue* callQueue)
        {
            if (!enabled || !vr)
                return;

            if (queueMode != 0)
            {
                if (!callQueue)
                {
                    static std::atomic<bool> s_loggedMissingQueue{ false };
                    if (vr->m_VRNearClipDebugLog &&
                        !s_loggedMissingQueue.exchange(true, std::memory_order_acq_rel))
                    {
                        Game::logMsg(
                            "[VR][NearClip] viewmodel depth-clamp skipped: Source render call queue unavailable");
                    }
                    return;
                }

                m_CallQueue = callQueue;
                m_QueuedState = new HooksQueuedNativeViewmodelDepthClampState(vr);
                m_CallQueue->QueueFunctor(
                    new HooksQueuedNativeViewmodelDepthClampBeginFunctor(m_QueuedState));
                return;
            }

            m_Device = HooksNativeViewmodelHandsOnlyGetD3DDevice(vr);
            if (!m_Device)
                return;

            if (FAILED(m_Device->GetRenderState(D3DRS_CLIPPING, &m_OldClipping)) ||
                FAILED(m_Device->SetRenderState(D3DRS_CLIPPING, FALSE)))
            {
                m_Device->Release();
                m_Device = nullptr;
                return;
            }
            m_Active = true;
        }

        ~ScopedNativeViewmodelDepthClamp()
        {
            if (m_QueuedState)
            {
                m_CallQueue->QueueFunctor(
                    new HooksQueuedNativeViewmodelDepthClampEndFunctor(m_QueuedState));
                m_QueuedState->Release();
                m_QueuedState = nullptr;
                m_CallQueue = nullptr;
            }

            if (m_Device)
            {
                if (m_Active)
                    m_Device->SetRenderState(D3DRS_CLIPPING, m_OldClipping);
                m_Device->Release();
                m_Device = nullptr;
            }
        }

    private:
        ICallQueue* m_CallQueue = nullptr;
        HooksQueuedNativeViewmodelDepthClampState* m_QueuedState = nullptr;
        IDirect3DDevice9* m_Device = nullptr;
        DWORD m_OldClipping = TRUE;
        bool m_Active = false;
    };

    inline bool HooksFirstPersonBodyGetModelNameSafe(
        IModelInfo* modelInfo,
        const model_t* model,
        const char*& modelName)
    {
        modelName = nullptr;
        if (!modelInfo || !model)
            return false;

#if defined(_MSC_VER)
        __try
        {
            modelName = modelInfo->GetModelName(
                const_cast<model_t*>(model));
            return modelName && *modelName;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            modelName = nullptr;
            return false;
        }
#else
        modelName = modelInfo->GetModelName(const_cast<model_t*>(model));
        return modelName && *modelName;
#endif
    }

    inline IMaterial* HooksWorldModelClothingFindMaterialSafe(
        IMaterialSystem* materialSystem,
        const char* materialName)
    {
        if (!materialSystem || !materialName || !*materialName)
            return nullptr;

#if defined(_MSC_VER)
        __try
        {
            return materialSystem->FindMaterial(
                materialName,
                "Model textures",
                false,
                nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return materialSystem->FindMaterial(
            materialName,
            "Model textures",
            false,
            nullptr);
#endif
    }

    inline bool HooksWorldModelClothingIsUsableMaterialSafe(
        IMaterial* material)
    {
        if (!material)
            return false;

#if defined(_MSC_VER)
        __try
        {
            return !material->IsErrorMaterial();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        return !material->IsErrorMaterial();
#endif
    }

    inline bool HooksFirstPersonBodySetMaterialNoDrawSafe(
        IMaterial* material,
        bool noDraw)
    {
        if (!material)
            return false;

#if defined(_MSC_VER)
        __try
        {
            material->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, noDraw);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        material->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, noDraw);
        return true;
#endif
    }

    inline bool HooksFirstPersonBodyAddMaterialRefSafe(IMaterial* material)
    {
        if (!material)
            return false;

#if defined(_MSC_VER)
        __try
        {
            material->IncrementReferenceCount();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        material->IncrementReferenceCount();
        return true;
#endif
    }

    inline void HooksFirstPersonBodyReleaseMaterialRefSafe(IMaterial* material)
    {
        if (!material)
            return;

#if defined(_MSC_VER)
        __try
        {
            material->DecrementReferenceCount();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
#else
        material->DecrementReferenceCount();
#endif
    }

    inline std::string HooksFirstPersonBodyNormalizeMaterialBaseName(
        const std::string& rawName)
    {
        std::string result = rawName;
        std::transform(
            result.begin(),
            result.end(),
            result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(result.begin(), result.end(), '\\', '/');

        const size_t slash = result.find_last_of('/');
        if (slash != std::string::npos)
            result.erase(0, slash + 1);

        constexpr char kVmtExtension[] = ".vmt";
        constexpr size_t kVmtExtensionLength = sizeof(kVmtExtension) - 1;
        if (result.size() > kVmtExtensionLength &&
            result.compare(
                result.size() - kVmtExtensionLength,
                kVmtExtensionLength,
                kVmtExtension) == 0)
        {
            result.resize(result.size() - kVmtExtensionLength);
        }
        return result;
    }

    inline bool HooksWorldModelClothingGetCharacterName(
        IModelInfo* modelInfo,
        const model_t* model,
        std::string& characterName)
    {
        characterName.clear();
        const char* rawModelName = nullptr;
        if (!HooksFirstPersonBodyGetModelNameSafe(
            modelInfo,
            model,
            rawModelName))
        {
            return false;
        }

        std::string modelName = rawModelName;
        std::transform(
            modelName.begin(),
            modelName.end(),
            modelName.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(modelName.begin(), modelName.end(), '\\', '/');

        struct CharacterModel
        {
            const char* modelName;
            const char* characterName;
        };
        static constexpr CharacterModel kCharacterModels[] = {
            { "models/survivors/survivor_biker.mdl", "francis" },
            { "models/survivors/survivor_coach.mdl", "coach" },
            { "models/survivors/survivor_gambler.mdl", "nick" },
            { "models/survivors/survivor_manager.mdl", "louis" },
            { "models/survivors/survivor_mechanic.mdl", "ellis" },
            { "models/survivors/survivor_namvet.mdl", "bill" },
            { "models/survivors/survivor_producer.mdl", "rochelle" },
            { "models/survivors/survivor_teenangst.mdl", "zoey" },
        };
        for (const CharacterModel& characterModel : kCharacterModels)
        {
            if (modelName == characterModel.modelName)
            {
                characterName = characterModel.characterName;
                return true;
            }
        }
        return false;
    }

    inline std::string HooksWorldModelClothingNormalizeMaterialPath(
        const std::string& rawName)
    {
        std::string result = rawName;
        std::transform(
            result.begin(),
            result.end(),
            result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(result.begin(), result.end(), '\\', '/');

        constexpr char kMaterialsPrefix[] = "materials/";
        constexpr size_t kMaterialsPrefixLength =
            sizeof(kMaterialsPrefix) - 1;
        if (result.compare(
            0,
            kMaterialsPrefixLength,
            kMaterialsPrefix) == 0)
        {
            result.erase(0, kMaterialsPrefixLength);
        }

        constexpr char kVmtExtension[] = ".vmt";
        constexpr size_t kVmtExtensionLength = sizeof(kVmtExtension) - 1;
        if (result.size() > kVmtExtensionLength &&
            result.compare(
                result.size() - kVmtExtensionLength,
                kVmtExtensionLength,
                kVmtExtension) == 0)
        {
            result.resize(result.size() - kVmtExtensionLength);
        }
        return result;
    }

    inline void HooksMaybePublishAndLogPlayerModelMaterials(
        VR* vr,
        Game* game,
        void* drawState,
        const ModelRenderInfo_t& info,
        bool shadowDepthDraw)
    {
        struct PublishedModelKey
        {
            uint32_t session = 0;
            int entityIndex = 0;
            const void* renderable = nullptr;
            std::string modelName;
        };
        static std::mutex s_LogStateMutex;
        static uint32_t s_PublishedSession = 0;
        static std::vector<PublishedModelKey> s_PublishedModels;

        if (!game || !game->m_EngineClient)
            return;

        const bool inGame = game->m_EngineClient->IsInGame();
        const int localPlayerIndex =
            inGame ? game->m_EngineClient->GetLocalPlayer() : -1;
        if (!inGame || localPlayerIndex <= 0)
        {
            std::lock_guard<std::mutex> lock(s_LogStateMutex);
            s_PublishedSession = 0;
            s_PublishedModels.clear();
            return;
        }

        if (shadowDepthDraw ||
            !game->m_ModelInfo ||
            !info.pModel ||
            info.entity_index <= 0)
        {
            return;
        }

        std::string modelName = "<unknown>";
        const char* resolvedModelName = nullptr;
        if (HooksFirstPersonBodyGetModelNameSafe(
            game->m_ModelInfo,
            info.pModel,
            resolvedModelName))
        {
            modelName = resolvedModelName;
        }

        const uint32_t logSession =
            vr
            ? vr->m_PlayerModelMaterialsLogSession.load(
                std::memory_order_acquire)
            : 0u;
        auto matchesCurrentDraw = [&](const PublishedModelKey& key) {
            return key.session == logSession &&
                key.entityIndex == info.entity_index &&
                key.renderable == info.pRenderable &&
                key.modelName == modelName;
            };
        {
            std::lock_guard<std::mutex> lock(s_LogStateMutex);
            if (s_PublishedSession != logSession)
            {
                s_PublishedSession = logSession;
                s_PublishedModels.clear();
            }
            if (std::find_if(
                s_PublishedModels.begin(),
                s_PublishedModels.end(),
                matchesCurrentDraw) != s_PublishedModels.end())
                return;
        }

        std::string characterName;
        if (!HooksWorldModelClothingGetCharacterName(
            game->m_ModelInfo,
            info.pModel,
            characterName))
        {
            return;
        }

        std::vector<std::string> materialNames;
        if (!vr_vm_stabilize::TryGetStudioMaterialNamesFromDrawState(
            drawState,
            materialNames))
        {
            return;
        }

        if (materialNames.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(s_LogStateMutex);
            if (s_PublishedSession != logSession)
            {
                s_PublishedSession = logSession;
                s_PublishedModels.clear();
            }
            if (std::find_if(
                s_PublishedModels.begin(),
                s_PublishedModels.end(),
                matchesCurrentDraw) != s_PublishedModels.end())
                return;
            s_PublishedModels.push_back({
                logSession,
                info.entity_index,
                info.pRenderable,
                modelName
                });
        }

        if (vr)
        {
            vr->PublishPlayerModelMaterialsSnapshot(
                logSession,
                info.entity_index,
                characterName,
                modelName,
                materialNames,
                info.entity_index == localPlayerIndex);
        }

        // Cache every actually drawn survivor for the scan button, while keeping
        // the detailed console/file log focused on the local player.
        if (info.entity_index != localPlayerIndex)
            return;

        Game::logMsg(
            "[VR][PlayerModelMaterials] character=%s model=%s count=%u",
            characterName.c_str(),
            modelName.c_str(),
            static_cast<unsigned int>(materialNames.size()));
        for (size_t materialIndex = 0;
            materialIndex < materialNames.size();
            ++materialIndex)
        {
            Game::logMsg(
                "[VR][PlayerModelMaterials] character=%s material[%u/%u]=%s",
                characterName.c_str(),
                static_cast<unsigned int>(materialIndex + 1),
                static_cast<unsigned int>(materialNames.size()),
                materialNames[materialIndex].c_str());
        }
    }

    inline bool HooksFirstPersonBodyCollectHiddenMaterials(
        VR* vr,
        Game* game,
        void* drawState,
        const model_t* model,
        std::vector<IMaterial*>& hiddenMaterials)
    {
        hiddenMaterials.clear();
        if (!vr ||
            !game ||
            !game->m_ModelInfo ||
            !drawState ||
            !model ||
            !vr->m_ClothingMaterials.load(std::memory_order_acquire))
        {
            return false;
        }

        std::string characterName;
        if (!HooksWorldModelClothingGetCharacterName(
            game->m_ModelInfo,
            model,
            characterName))
        {
            return false;
        }

        if (!game->m_MaterialSystem)
        {
            return false;
        }

        std::vector<std::string> configuredMaterialNames;
        {
            std::lock_guard<std::mutex> lock(
                vr->m_HiddenMaterialNamesMutex);
            for (const VR::HiddenMaterialNameRule& rule :
                vr->m_HiddenMaterialNames)
            {
                if (rule.characterName != characterName ||
                    rule.materialName.empty())
                {
                    continue;
                }

                if (std::find(
                    configuredMaterialNames.begin(),
                    configuredMaterialNames.end(),
                    rule.materialName) == configuredMaterialNames.end())
                {
                    configuredMaterialNames.push_back(rule.materialName);
                }
            }
        }
        if (configuredMaterialNames.empty())
            return false;

        // Read the current model's own texture table and search directories.
        // This avoids the unsafe model-info material-enumeration ABI while also
        // ensuring a rule cannot affect a different character that happens to
        // reuse the same material base name.
        std::vector<std::string> modelMaterialNames;
        std::vector<std::string> modelMaterialDirectories;
        if (!vr_vm_stabilize::TryGetStudioMaterialNamesFromDrawState(
            drawState,
            modelMaterialNames,
            &modelMaterialDirectories))
        {
            return false;
        }

        auto addResolvedMaterial =
            [&](const std::string& rawMaterialPath) -> bool {
            const std::string materialPath =
                HooksWorldModelClothingNormalizeMaterialPath(
                    rawMaterialPath);
            if (materialPath.empty())
                return false;

            IMaterial* material =
                HooksWorldModelClothingFindMaterialSafe(
                    game->m_MaterialSystem,
                    materialPath.c_str());
            if (!HooksWorldModelClothingIsUsableMaterialSafe(material))
                return false;

            if (std::find(
                hiddenMaterials.begin(),
                hiddenMaterials.end(),
                material) == hiddenMaterials.end())
            {
                hiddenMaterials.push_back(material);
            }
            return true;
            };

        for (const std::string& modelMaterialName : modelMaterialNames)
        {
            const std::string materialBaseName =
                HooksFirstPersonBodyNormalizeMaterialBaseName(
                    modelMaterialName);
            if (materialBaseName.empty() ||
                std::find(
                    configuredMaterialNames.begin(),
                    configuredMaterialNames.end(),
                    materialBaseName) == configuredMaterialNames.end())
            {
                continue;
            }

            const std::string normalizedModelMaterialPath =
                HooksWorldModelClothingNormalizeMaterialPath(
                    modelMaterialName);
            const bool modelMaterialContainsPath =
                normalizedModelMaterialPath.find('/') != std::string::npos;
            if (modelMaterialContainsPath &&
                addResolvedMaterial(normalizedModelMaterialPath))
            {
                continue;
            }

            bool resolved = false;
            for (const std::string& materialDirectory :
                modelMaterialDirectories)
            {
                if (addResolvedMaterial(
                    materialDirectory + modelMaterialName))
                {
                    resolved = true;
                    break;
                }
            }

            if (!resolved)
                addResolvedMaterial(normalizedModelMaterialPath);
        }

        return !hiddenMaterials.empty();
    }

    class HooksFirstPersonBodyMaterialHideRegistry
    {
    public:
        void Acquire(
            const std::vector<IMaterial*>& materials,
            std::vector<IMaterial*>& acquiredMaterials)
        {
            acquiredMaterials.clear();
            if (materials.empty())
                return;

            std::lock_guard<std::mutex> lock(m_Mutex);
            for (IMaterial* material : materials)
            {
                if (!material ||
                    std::find(
                        acquiredMaterials.begin(),
                        acquiredMaterials.end(),
                        material) != acquiredMaterials.end())
                {
                    continue;
                }

                auto existing = m_Entries.find(material);
                if (existing != m_Entries.end())
                {
                    ++existing->second.pendingDraws;
                    HooksFirstPersonBodySetMaterialNoDrawSafe(material, true);
                    acquiredMaterials.push_back(material);
                    continue;
                }

                if (!HooksFirstPersonBodyAddMaterialRefSafe(material))
                {
                    continue;
                }

                if (!HooksFirstPersonBodySetMaterialNoDrawSafe(material, true))
                {
                    HooksFirstPersonBodyReleaseMaterialRefSafe(material);
                    continue;
                }

                Entry entry{};
                // A configured visible model material is restored after this
                // scoped draw. Avoid the unstable GetMaterialVarFlag ABI call.
                entry.originalNoDraw = false;
                entry.pendingDraws = 1;
                m_Entries.emplace(material, entry);
                acquiredMaterials.push_back(material);
            }
        }

        void Release(const std::vector<IMaterial*>& materials)
        {
            if (materials.empty())
                return;

            std::lock_guard<std::mutex> lock(m_Mutex);
            for (IMaterial* material : materials)
            {
                const auto existing = m_Entries.find(material);
                if (existing == m_Entries.end())
                    continue;

                if (existing->second.pendingDraws > 1)
                {
                    --existing->second.pendingDraws;
                    continue;
                }

                const bool restored = HooksFirstPersonBodySetMaterialNoDrawSafe(
                    material,
                    existing->second.originalNoDraw);
                (void)restored;
                HooksFirstPersonBodyReleaseMaterialRefSafe(material);
                m_Entries.erase(existing);
            }
        }

    private:
        struct Entry
        {
            bool originalNoDraw = false;
            size_t pendingDraws = 0;
        };

        std::mutex m_Mutex;
        std::unordered_map<IMaterial*, Entry> m_Entries;
    };

    inline HooksFirstPersonBodyMaterialHideRegistry&
        HooksFirstPersonBodyGetMaterialHideRegistry()
    {
        static HooksFirstPersonBodyMaterialHideRegistry s_Registry;
        return s_Registry;
    }

    class HooksFirstPersonBodyMaterialHideLease
    {
    public:
        explicit HooksFirstPersonBodyMaterialHideLease(
            const std::vector<IMaterial*>& materials)
        {
            HooksFirstPersonBodyGetMaterialHideRegistry().Acquire(
                materials,
                m_AcquiredMaterials);
        }

        ~HooksFirstPersonBodyMaterialHideLease()
        {
            HooksFirstPersonBodyGetMaterialHideRegistry().Release(
                m_AcquiredMaterials);
        }

        bool Active() const
        {
            return !m_AcquiredMaterials.empty();
        }

        std::vector<IMaterial*> Detach()
        {
            return std::move(m_AcquiredMaterials);
        }

        HooksFirstPersonBodyMaterialHideLease(
            const HooksFirstPersonBodyMaterialHideLease&) = delete;
        HooksFirstPersonBodyMaterialHideLease& operator=(
            const HooksFirstPersonBodyMaterialHideLease&) = delete;

    private:
        std::vector<IMaterial*> m_AcquiredMaterials;
    };

    class HooksFirstPersonBodyQueuedMaterialReleaseFunctor final : public CFunctor
    {
    public:
        explicit HooksFirstPersonBodyQueuedMaterialReleaseFunctor(
            std::vector<IMaterial*>&& materials)
            : m_Materials(std::move(materials))
        {
        }

        ~HooksFirstPersonBodyQueuedMaterialReleaseFunctor() override
        {
            Restore();
        }

        int AddRef() override
        {
            return static_cast<int>(
                m_RefCount.fetch_add(1, std::memory_order_acq_rel) + 1);
        }

        int Release() override
        {
            const long remaining =
                m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
                delete this;
            return static_cast<int>(remaining);
        }

        void operator()() override
        {
            Restore();
        }

    private:
        void Restore()
        {
            if (m_Restored)
                return;

            HooksFirstPersonBodyGetMaterialHideRegistry().Release(m_Materials);
            m_Materials.clear();
            m_Restored = true;
        }

        std::atomic<long> m_RefCount{ 0 };
        std::vector<IMaterial*> m_Materials;
        bool m_Restored = false;
    };

    class HooksWorldModelClothingQueuedReleaseGuard
    {
    public:
        HooksWorldModelClothingQueuedReleaseGuard(
            HooksFirstPersonBodyMaterialHideLease& lease,
            int queueMode,
            ICallQueue* callQueue)
            : m_Lease(lease),
            m_QueueMode(queueMode),
            m_CallQueue(callQueue)
        {
        }

        ~HooksWorldModelClothingQueuedReleaseGuard()
        {
            if (!m_Lease.Active() || m_QueueMode == 0)
                return;

            if (m_CallQueue)
            {
                std::vector<IMaterial*> queuedMaterials = m_Lease.Detach();
                auto* releaseFunctor =
                    new (std::nothrow)
                    HooksFirstPersonBodyQueuedMaterialReleaseFunctor(
                        std::move(queuedMaterials));
                if (releaseFunctor)
                {
                    m_CallQueue->QueueFunctor(releaseFunctor);
                }
                else
                {
                    HooksFirstPersonBodyGetMaterialHideRegistry().Release(
                        queuedMaterials);
                }
                return;
            }

            // No Source call queue was exposed. The lease destructor below
            // restores the material immediately as a safe submission-time
            // fallback; this diagnostic is intentionally silent.
        }

        HooksWorldModelClothingQueuedReleaseGuard(
            const HooksWorldModelClothingQueuedReleaseGuard&) = delete;
        HooksWorldModelClothingQueuedReleaseGuard& operator=(
            const HooksWorldModelClothingQueuedReleaseGuard&) = delete;

    private:
        HooksFirstPersonBodyMaterialHideLease& m_Lease;
        int m_QueueMode = 0;
        ICallQueue* m_CallQueue = nullptr;
    };

    struct HooksFirstPersonBodyBoneBuffer
    {
        std::vector<vr_vm_stabilize::Mat3x4> bones;
    };

    // L4D2's StudioRender copies a non-RenderData pCustomBoneToWorld into Source
    // RenderData before DrawModelExecute returns in both q0 and q2. The plugin
    // scratch therefore only needs call lifetime. Keep a bounded pool to remain
    // safe under theoretical nested model draws without tying reclamation to a
    // render-frame counter or a separately acquired material call queue.
    class HooksFirstPersonBodyBoneBufferPool
    {
    public:
        static constexpr size_t kMaxBuffers = 32;

        HooksFirstPersonBodyBoneBuffer* Acquire()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_FreeCount > 0)
            {
                HooksFirstPersonBodyBoneBuffer* const result =
                    m_Free[--m_FreeCount];
                m_Free[m_FreeCount] = nullptr;
                return result;
            }

            if (m_AllCount >= kMaxBuffers)
                return nullptr;

            std::unique_ptr<HooksFirstPersonBodyBoneBuffer> buffer(
                new (std::nothrow) HooksFirstPersonBodyBoneBuffer());
            if (!buffer)
                return nullptr;

            HooksFirstPersonBodyBoneBuffer* const result = buffer.get();
            m_All[m_AllCount++] = std::move(buffer);
            return result;
        }

        void Release(HooksFirstPersonBodyBoneBuffer* buffer)
        {
            if (!buffer)
                return;

            std::lock_guard<std::mutex> lock(m_Mutex);
            buffer->bones.clear();
            if (m_FreeCount < kMaxBuffers)
                m_Free[m_FreeCount++] = buffer;
        }

    private:
        std::mutex m_Mutex;
        std::unique_ptr<HooksFirstPersonBodyBoneBuffer> m_All[kMaxBuffers];
        HooksFirstPersonBodyBoneBuffer* m_Free[kMaxBuffers]{};
        size_t m_AllCount = 0;
        size_t m_FreeCount = 0;
    };

    inline HooksFirstPersonBodyBoneBufferPool& HooksFirstPersonBodyGetBoneBufferPool()
    {
        static HooksFirstPersonBodyBoneBufferPool s_Pool;
        return s_Pool;
    }

    class HooksFirstPersonBodyBoneBufferLease
    {
    public:
        HooksFirstPersonBodyBoneBufferLease()
            : m_Buffer(HooksFirstPersonBodyGetBoneBufferPool().Acquire())
        {
        }

        ~HooksFirstPersonBodyBoneBufferLease()
        {
            HooksFirstPersonBodyGetBoneBufferPool().Release(m_Buffer);
            m_Buffer = nullptr;
        }

        HooksFirstPersonBodyBoneBuffer* Get() const
        {
            return m_Buffer;
        }

        HooksFirstPersonBodyBoneBufferLease(
            const HooksFirstPersonBodyBoneBufferLease&) = delete;
        HooksFirstPersonBodyBoneBufferLease& operator=(
            const HooksFirstPersonBodyBoneBufferLease&) = delete;

    private:
        HooksFirstPersonBodyBoneBuffer* m_Buffer = nullptr;
    };

    inline bool HooksFirstPersonBodyFindBone(
        const std::vector<std::string>& boneNames,
        const std::vector<const char*>& preferredSuffixes,
        int& outBone)
    {
        outBone = -1;
        for (const char* suffix : preferredSuffixes)
        {
            if (!suffix || !*suffix)
                continue;

            for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
            {
                const std::string lowerName =
                    vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
                const size_t suffixLength = std::strlen(suffix);
                if (lowerName.size() >= suffixLength &&
                    lowerName.compare(
                        lowerName.size() - suffixLength,
                        suffixLength,
                        suffix) == 0)
                {
                    outBone = bone;
                    return true;
                }
            }
        }
        return false;
    }

    struct HooksFirstPersonBodyBoneLayout
    {
        const uint8_t* studioHdr = nullptr;
        std::uint64_t playerGeneration = 0;
        bool attempted = false;
        bool valid = false;
        bool frozenPoseCaptured = false;
        bool animationFreeArmAnchorsCaptured = false;
        bool animationFreeUpperChestValid = false;
        bool animationFreeTorsoFrameValid = false;
        bool animationFreeLeftShoulderValid = false;
        bool animationFreeRightShoulderValid = false;
        bool animationFreeAnchorUsesModelEye = false;
        bool animationFreeAnchorRejectedDistantModelEye = false;
        int numBones = 0;
        int boneIndex = 0;
        int boneStride = 0;
        int headBone = -1;
        int neckBone = -1;
        int upperChestBone = -1;
        int torsoYawRootBone = -1;
        int leftClavicleBone = -1;
        int rightClavicleBone = -1;
        int leftUpperArmBone = -1;
        int rightUpperArmBone = -1;
        int leftForearmBone = -1;
        int rightForearmBone = -1;
        Vector animationFreeHeadFromAnchor{};
        Vector animationFreeUpperChestFromAnchor{};
        Vector animationFreeLeftShoulderFromAnchor{};
        Vector animationFreeRightShoulderFromAnchor{};
        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        std::vector<uint8_t> hideHeadMask;
        std::vector<uint8_t> hideLeftArmMask;
        std::vector<uint8_t> hideRightArmMask;
        std::vector<uint8_t> freezeLeftArmMask;
        std::vector<uint8_t> freezeRightArmMask;
        std::vector<vr_vm_stabilize::Mat3x4> frozenLocalPose;
    };

    inline bool HooksFirstPersonBodyBuildGeometricTorsoFrame(
        const Vector& upperChest,
        const Vector& leftShoulder,
        const Vector& rightShoulder,
        const Vector& head,
        vr_vm_stabilize::Mat3x4& outFrame)
    {
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(upperChest) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(leftShoulder) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(rightShoulder) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(head))
        {
            return false;
        }

        // Source model space is forward/left/up. Deriving the frame from visible
        // landmarks avoids assuming that a replacement model's chest bone uses
        // any particular local matrix-axis convention.
        Vector leftAxis = leftShoulder - rightShoulder;
        Vector upHint = head - upperChest;
        if (VectorNormalize(leftAxis) == 0.0f)
            return false;
        upHint -= leftAxis * DotProduct(upHint, leftAxis);
        if (VectorNormalize(upHint) == 0.0f)
            return false;
        Vector forwardAxis = CrossProduct(leftAxis, upHint);
        if (VectorNormalize(forwardAxis) == 0.0f)
            return false;

        return HooksViewmodelAutoGripBuildRigidMatrix(
            upperChest,
            forwardAxis,
            leftAxis,
            upHint,
            outFrame);
    }

    inline bool HooksFirstPersonBodyBuildProperBodyFrame(
        const QAngle& angles,
        vr_vm_stabilize::Mat3x4& outFrame)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(angles, &forward, &right, &up);
        return HooksViewmodelAutoGripBuildRigidMatrix(
            Vector(0.0f, 0.0f, 0.0f),
            forward,
            right * -1.0f,
            up,
            outFrame);
    }

    inline bool HooksFirstPersonBodyBuildBoneLayout(
        void* drawState,
        std::uint64_t playerGeneration,
        HooksFirstPersonBodyBoneLayout& layout)
    {
        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        if (layout.studioHdr == studioHdr &&
            layout.playerGeneration == playerGeneration &&
            layout.attempted)
            return layout.valid;

        layout = HooksFirstPersonBodyBoneLayout{};
        layout.studioHdr = studioHdr;
        layout.playerGeneration = playerGeneration;
        layout.attempted = true;

        std::vector<std::string> boneNames;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            layout.boneParents,
            layout.numBones,
            boneIndex,
            stride,
            numBonesOffset) ||
            layout.numBones <= 0 ||
            layout.numBones > 128 ||
            static_cast<int>(boneNames.size()) < layout.numBones ||
            static_cast<int>(layout.boneParents.size()) < layout.numBones)
        {
            return false;
        }
        layout.boneIndex = boneIndex;
        layout.boneStride = stride;
        layout.boneNames = boneNames;

        const std::vector<const char*> headSuffixes = {
            "bip01_head1", "bip01_head", "head1", "head",
        };
        const std::vector<const char*> neckSuffixes = {
            "bip01_neck1", "bip01_neck", "neck1", "neck",
        };
        HooksFirstPersonBodyFindBone(boneNames, headSuffixes, layout.headBone);
        HooksFirstPersonBodyFindBone(boneNames, neckSuffixes, layout.neckBone);

        if (layout.headBone < 0 || layout.headBone >= layout.numBones)
            return false;

        if (layout.neckBone < 0 || layout.neckBone >= layout.numBones)
        {
            const int parent = layout.boneParents[static_cast<size_t>(layout.headBone)];
            if (parent >= 0 && parent < layout.numBones)
                layout.neckBone = parent;
            else
                layout.neckBone = layout.headBone;
        }

        HooksFirstPersonBodyFindBone(
            boneNames,
            {
                "bip01_spine4", "spine4",
                "bip01_spine3", "spine3",
                "bip01_spine2", "spine2",
            },
            layout.upperChestBone);
        if (layout.neckBone >= 0 &&
            (layout.upperChestBone < 0 ||
             !HooksNativeViewmodelHandsOnlyIsAncestor(
                layout.boneParents,
                layout.neckBone,
                layout.upperChestBone,
                layout.numBones)))
        {
            int current =
                layout.boneParents[static_cast<size_t>(layout.neckBone)];
            layout.upperChestBone = -1;
            for (int guard = 0;
                 guard < layout.numBones &&
                 current >= 0 &&
                 current < layout.numBones;
                 ++guard)
            {
                const std::string lowerName =
                    vr_vm_stabilize::ToLowerAscii(
                        boneNames[static_cast<size_t>(current)]);
                if (lowerName.find("spine") != std::string::npos ||
                    lowerName.find("chest") != std::string::npos)
                {
                    layout.upperChestBone = current;
                    break;
                }
                current = layout.boneParents[static_cast<size_t>(current)];
            }
        }

        // The first-person body keeps normal lower-body locomotion, but all
        // spine bones must share the deadzone-resolved torso yaw.  Remember the
        // lowest named spine/chest ancestor so a later residual correction can
        // remove Source aim-yaw that is baked into the animated upper body.
        layout.torsoYawRootBone = layout.upperChestBone;
        int torsoYawCandidate = layout.upperChestBone;
        for (int guard = 0;
             guard < layout.numBones &&
             torsoYawCandidate >= 0 &&
             torsoYawCandidate < layout.numBones;
             ++guard)
        {
            const int parent =
                layout.boneParents[static_cast<size_t>(torsoYawCandidate)];
            if (parent < 0 || parent >= layout.numBones ||
                parent == torsoYawCandidate)
            {
                break;
            }

            const std::string lowerParent =
                vr_vm_stabilize::ToLowerAscii(
                    boneNames[static_cast<size_t>(parent)]);
            if (lowerParent.find("spine") == std::string::npos &&
                lowerParent.find("chest") == std::string::npos &&
                lowerParent.find("torso") == std::string::npos)
            {
                break;
            }

            layout.torsoYawRootBone = parent;
            torsoYawCandidate = parent;
        }

        const std::vector<const char*> clavicleNeedles = {
            "clavicle", "collarbone", "collar",
        };
        HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            clavicleNeedles,
            -1,
            layout.leftClavicleBone);
        HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            clavicleNeedles,
            1,
            layout.rightClavicleBone);

        const std::vector<const char*> upperArmNeedles = {
            "upperarm", "upper_arm", "upper arm",
        };
        HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            upperArmNeedles,
            -1,
            layout.leftUpperArmBone);
        HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            upperArmNeedles,
            1,
            layout.rightUpperArmBone);

        const std::vector<const char*> forearmNeedles = {
            "forearm", "lowerarm", "lower_arm", "lower arm",
        };
        HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            forearmNeedles,
            -1,
            layout.leftForearmBone);
        HooksNativeViewmodelHandsOnlyFindNamedBone(
            boneNames,
            forearmNeedles,
            1,
            layout.rightForearmBone);

        const size_t boneCount = static_cast<size_t>(layout.numBones);
        layout.hideHeadMask.assign(boneCount, 0u);
        layout.hideLeftArmMask.assign(boneCount, 0u);
        layout.hideRightArmMask.assign(boneCount, 0u);
        layout.freezeLeftArmMask.assign(boneCount, 0u);
        layout.freezeRightArmMask.assign(boneCount, 0u);

        int hiddenHeadBones = 0;
        int hiddenLeftArmBones = 0;
        int hiddenRightArmBones = 0;
        int frozenLeftArmBones = 0;
        int frozenRightArmBones = 0;
        const int leftShoulderFreezeRoot =
            (layout.leftClavicleBone >= 0 &&
                layout.leftClavicleBone < layout.numBones)
            ? layout.leftClavicleBone
            : layout.leftUpperArmBone;
        const int rightShoulderFreezeRoot =
            (layout.rightClavicleBone >= 0 &&
                layout.rightClavicleBone < layout.numBones)
            ? layout.rightClavicleBone
            : layout.rightUpperArmBone;
        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            if (HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.boneParents,
                    bone,
                    layout.headBone,
                    layout.numBones))
            {
                layout.hideHeadMask[static_cast<size_t>(bone)] = 1u;
                ++hiddenHeadBones;
            }

            if (layout.leftForearmBone >= 0 &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.boneParents,
                    bone,
                    layout.leftForearmBone,
                    layout.numBones))
            {
                layout.hideLeftArmMask[static_cast<size_t>(bone)] = 1u;
                ++hiddenLeftArmBones;
            }

            if (layout.rightForearmBone >= 0 &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.boneParents,
                    bone,
                    layout.rightForearmBone,
                    layout.numBones))
            {
                layout.hideRightArmMask[static_cast<size_t>(bone)] = 1u;
                ++hiddenRightArmBones;
            }

            if (leftShoulderFreezeRoot >= 0 &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.boneParents,
                    bone,
                    leftShoulderFreezeRoot,
                    layout.numBones))
            {
                layout.freezeLeftArmMask[static_cast<size_t>(bone)] = 1u;
                ++frozenLeftArmBones;
            }

            if (rightShoulderFreezeRoot >= 0 &&
                HooksNativeViewmodelHandsOnlyIsAncestor(
                    layout.boneParents,
                    bone,
                    rightShoulderFreezeRoot,
                    layout.numBones))
            {
                layout.freezeRightArmMask[static_cast<size_t>(bone)] = 1u;
                ++frozenRightArmBones;
            }
        }

        layout.valid = hiddenHeadBones > 0;
        if (layout.valid)
        {
            Game::logMsg(
                "[VR][FirstPersonBody] bone layout ready bones=%d head=%d neck=%d upperChest=%d torsoYawRoot=%d leftClavicle=%d rightClavicle=%d leftUpperArm=%d rightUpperArm=%d leftForearm=%d rightForearm=%d hiddenHead=%d hiddenLeftForearm=%d hiddenRightForearm=%d frozenLeftShoulder=%d frozenRightShoulder=%d",
                layout.numBones,
                layout.headBone,
                layout.neckBone,
                layout.upperChestBone,
                layout.torsoYawRootBone,
                layout.leftClavicleBone,
                layout.rightClavicleBone,
                layout.leftUpperArmBone,
                layout.rightUpperArmBone,
                layout.leftForearmBone,
                layout.rightForearmBone,
                hiddenHeadBones,
                hiddenLeftArmBones,
                hiddenRightArmBones,
                frozenLeftArmBones,
                frozenRightArmBones);
        }
        return layout.valid;
    }

    inline bool HooksFirstPersonBodyCaptureAnimationFreeArmAnchors(
        void* drawState,
        HooksFirstPersonBodyBoneLayout& layout)
    {
        if (layout.animationFreeArmAnchorsCaptured)
        {
            return layout.animationFreeUpperChestValid ||
                layout.animationFreeLeftShoulderValid ||
                layout.animationFreeRightShoulderValid;
        }
        if (!drawState || layout.numBones <= 0 || layout.numBones > 128 ||
            layout.boneIndex <= 0 || layout.boneStride <= 0 ||
            layout.headBone < 0 || layout.headBone >= layout.numBones ||
            static_cast<int>(layout.boneParents.size()) < layout.numBones)
        {
            return false;
        }

        std::vector<vr_vm_stabilize::Mat3x4> restLocal;
        std::vector<vr_vm_stabilize::Mat3x4> restWorld;
        std::vector<uint8_t> resolved;
        try
        {
            restLocal.resize(static_cast<size_t>(layout.numBones));
            restWorld.resize(static_cast<size_t>(layout.numBones));
            resolved.assign(static_cast<size_t>(layout.numBones), 0u);
        }
        catch (...)
        {
            return false;
        }

        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            if (!HooksNativeViewmodelHandsOnlyReadBoneRestLocalTransform(
                    drawState,
                    layout.boneIndex,
                    layout.boneStride,
                    bone,
                    restLocal[static_cast<size_t>(bone)]) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(
                    restLocal[static_cast<size_t>(bone)]))
            {
                return false;
            }
        }

        int rebuilt = 0;
        for (int pass = 0; pass < layout.numBones && rebuilt < layout.numBones; ++pass)
        {
            bool progressed = false;
            for (int bone = 0; bone < layout.numBones; ++bone)
            {
                const size_t index = static_cast<size_t>(bone);
                if (resolved[index])
                    continue;

                const int parent = layout.boneParents[index];
                if (parent < 0 || parent >= layout.numBones || parent == bone)
                {
                    restWorld[index] = restLocal[index];
                }
                else
                {
                    if (!resolved[static_cast<size_t>(parent)])
                        continue;
                    vr_vm_stabilize::Mul(
                        restWorld[static_cast<size_t>(parent)],
                        restLocal[index],
                        restWorld[index]);
                }

                if (!HooksNativeViewmodelHandsOnlyMatrixFinite(restWorld[index]))
                    return false;
                resolved[index] = 1u;
                ++rebuilt;
                progressed = true;
            }
            if (!progressed)
                break;
        }

        if (!resolved[static_cast<size_t>(layout.headBone)])
            return false;
        const Vector restHead = vr_vm_stabilize::GetOrigin(
            restWorld[static_cast<size_t>(layout.headBone)]);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(restHead))
            return false;

        // StudioHdr::eyeposition is useful only when it belongs to the replacement
        // skeleton. Some workshop models retain the stock survivor eye position
        // while moving the entire replacement skeleton elsewhere. Treat a distant
        // eye as stale and anchor those fallback rigs from the bind-pose head.
        Vector restAnchor = restHead;
        layout.animationFreeAnchorUsesModelEye = false;
        layout.animationFreeAnchorRejectedDistantModelEye = false;
        constexpr float kMaxModelEyeToHeadDistanceUnits = 16.0f;
        int studioLength = 0;
        float eyeX = 0.0f;
        float eyeY = 0.0f;
        float eyeZ = 0.0f;
        if (layout.studioHdr &&
            vr_vm_stabilize::SafeRead(layout.studioHdr + 0x4C, studioLength) &&
            studioLength >= 0x5C &&
            vr_vm_stabilize::SafeRead(layout.studioHdr + 0x50, eyeX) &&
            vr_vm_stabilize::SafeRead(layout.studioHdr + 0x54, eyeY) &&
            vr_vm_stabilize::SafeRead(layout.studioHdr + 0x58, eyeZ))
        {
            const Vector modelEye(eyeX, eyeY, eyeZ);
            const float eyeMagnitude = modelEye.Length();
            const float eyeToHeadDistance = (restHead - modelEye).Length();
            if (HooksNativeViewmodelHandsOnlyVectorFinite(modelEye) &&
                std::isfinite(eyeMagnitude) && eyeMagnitude > 1.0f &&
                std::isfinite(eyeToHeadDistance) &&
                eyeToHeadDistance <= kMaxModelEyeToHeadDistanceUnits)
            {
                restAnchor = modelEye;
                layout.animationFreeAnchorUsesModelEye = true;
            }
            else if (HooksNativeViewmodelHandsOnlyVectorFinite(modelEye) &&
                std::isfinite(eyeMagnitude) && eyeMagnitude > 1.0f &&
                std::isfinite(eyeToHeadDistance) &&
                eyeToHeadDistance > kMaxModelEyeToHeadDistanceUnits)
            {
                layout.animationFreeAnchorRejectedDistantModelEye = true;
                Game::logMsg(
                    "[VR][FirstPersonBody] rejected stale Studio eyeposition eye=(%.2f %.2f %.2f) head=(%.2f %.2f %.2f) distance=%.2f max=%.2f",
                    modelEye.x,
                    modelEye.y,
                    modelEye.z,
                    restHead.x,
                    restHead.y,
                    restHead.z,
                    eyeToHeadDistance,
                    kMaxModelEyeToHeadDistanceUnits);
            }
        }

        layout.animationFreeHeadFromAnchor = restHead - restAnchor;
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                layout.animationFreeHeadFromAnchor))
        {
            return false;
        }

        auto captureOffset = [&](int bone, Vector& outOffset, bool& outValid)
            {
                outOffset = Vector{};
                outValid = false;
                if (bone < 0 || bone >= layout.numBones ||
                    !resolved[static_cast<size_t>(bone)])
                {
                    return;
                }
                const Vector restOrigin = vr_vm_stabilize::GetOrigin(
                    restWorld[static_cast<size_t>(bone)]);
                if (!HooksNativeViewmodelHandsOnlyVectorFinite(restOrigin))
                    return;
                outOffset = restOrigin - restAnchor;
                outValid = HooksNativeViewmodelHandsOnlyVectorFinite(outOffset);
            };

        captureOffset(
            layout.upperChestBone,
            layout.animationFreeUpperChestFromAnchor,
            layout.animationFreeUpperChestValid);
        layout.animationFreeTorsoFrameValid = false;
        if (layout.animationFreeUpperChestValid &&
            layout.leftUpperArmBone >= 0 &&
            layout.leftUpperArmBone < layout.numBones &&
            layout.rightUpperArmBone >= 0 &&
            layout.rightUpperArmBone < layout.numBones &&
            resolved[static_cast<size_t>(layout.leftUpperArmBone)] &&
            resolved[static_cast<size_t>(layout.rightUpperArmBone)])
        {
            const Vector restUpperChest = vr_vm_stabilize::GetOrigin(
                restWorld[static_cast<size_t>(layout.upperChestBone)]);
            const Vector restLeftShoulder = vr_vm_stabilize::GetOrigin(
                restWorld[static_cast<size_t>(layout.leftUpperArmBone)]);
            const Vector restRightShoulder = vr_vm_stabilize::GetOrigin(
                restWorld[static_cast<size_t>(layout.rightUpperArmBone)]);
            vr_vm_stabilize::Mat3x4 restTorsoFrame{};
            if (HooksFirstPersonBodyBuildGeometricTorsoFrame(
                    restUpperChest,
                    restLeftShoulder,
                    restRightShoulder,
                    restHead,
                    restTorsoFrame))
            {
                // The rest frame is only a landmark-validity check. Its rotation
                // is already expressed in model space and must not be multiplied
                // into the target body frame a second time.
                layout.animationFreeTorsoFrameValid = true;
            }
        }
        captureOffset(
            layout.leftUpperArmBone,
            layout.animationFreeLeftShoulderFromAnchor,
            layout.animationFreeLeftShoulderValid);
        captureOffset(
            layout.rightUpperArmBone,
            layout.animationFreeRightShoulderFromAnchor,
            layout.animationFreeRightShoulderValid);

        // This flag means the StudioHdr rest pose was sampled successfully, not
        // that every optional replacement-rig landmark exists.  Cache the result
        // even when a model only supplies the upper-chest anchor.
        layout.animationFreeArmAnchorsCaptured = true;
        Game::logMsg(
            "[VR][FirstPersonBody] positional anchor=%s modelAnchor=(%.2f %.2f %.2f) headFromAnchor=(%.2f %.2f %.2f)",
            layout.animationFreeAnchorUsesModelEye
                ? "studio-eyeposition"
                : (layout.animationFreeAnchorRejectedDistantModelEye
                    ? "head-fallback-stale-eye"
                    : "head-fallback"),
            restAnchor.x,
            restAnchor.y,
            restAnchor.z,
            layout.animationFreeHeadFromAnchor.x,
            layout.animationFreeHeadFromAnchor.y,
            layout.animationFreeHeadFromAnchor.z);
        return layout.animationFreeUpperChestValid ||
            layout.animationFreeLeftShoulderValid ||
            layout.animationFreeRightShoulderValid;
    }

    inline bool HooksFirstPersonBodyCaptureFrozenLocalPose(
        HooksFirstPersonBodyBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4* sourceBones)
    {
        if (!sourceBones || layout.numBones <= 0)
            return false;

        if (layout.frozenPoseCaptured &&
            static_cast<int>(layout.frozenLocalPose.size()) == layout.numBones)
        {
            return true;
        }

        std::vector<vr_vm_stabilize::Mat3x4> frozenLocalPose;
        try
        {
            frozenLocalPose.assign(
                static_cast<size_t>(layout.numBones),
                vr_vm_stabilize::Identity());
        }
        catch (...)
        {
            return false;
        }

        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            const size_t index = static_cast<size_t>(bone);
            const bool freezeBone =
                layout.hideHeadMask[index] != 0u ||
                layout.freezeLeftArmMask[index] != 0u ||
                layout.freezeRightArmMask[index] != 0u;
            if (!freezeBone)
                continue;

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(boneWorld))
            {
                return false;
            }

            const int parent = layout.boneParents[index];
            if (parent < 0 || parent >= layout.numBones)
            {
                frozenLocalPose[index] = boneWorld;
                continue;
            }

            vr_vm_stabilize::Mat3x4 parentWorld{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + parent, parentWorld) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(parentWorld))
            {
                return false;
            }

            vr_vm_stabilize::Mat3x4 inverseParent{};
            vr_vm_stabilize::Mat3x4 boneLocal{};
            if (!vr_vm_stabilize::InvertAffine(parentWorld, inverseParent))
                return false;
            vr_vm_stabilize::Mul(inverseParent, boneWorld, boneLocal);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(boneLocal))
                return false;
            frozenLocalPose[index] = boneLocal;
        }

        layout.frozenLocalPose = std::move(frozenLocalPose);
        layout.frozenPoseCaptured = true;
        return true;
    }

    inline bool HooksFirstPersonBodyApplyFrozenBranch(
        const HooksFirstPersonBodyBoneLayout& layout,
        const std::vector<uint8_t>& mask,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones ||
            static_cast<int>(mask.size()) < layout.numBones ||
            static_cast<int>(layout.frozenLocalPose.size()) < layout.numBones)
        {
            return false;
        }

        for (int bone = 0; bone < layout.numBones; ++bone)
        {
            const size_t index = static_cast<size_t>(bone);
            if (!mask[index])
                continue;

            const int parent = layout.boneParents[index];
            if (parent < 0 || parent >= layout.numBones)
                continue;

            vr_vm_stabilize::Mat3x4 frozenWorld{};
            vr_vm_stabilize::Mul(
                bones[parent],
                layout.frozenLocalPose[index],
                frozenWorld);
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(frozenWorld))
                return false;
            bones[bone] = frozenWorld;
        }
        return true;
    }

    inline bool HooksFirstPersonBodyMaxBasisLength(
        const vr_vm_stabilize::Mat3x4& matrix,
        double& outLength)
    {
        outLength = 0.0;
        for (int axis = 0; axis < 3; ++axis)
        {
            const double x = static_cast<double>(matrix.m[0][axis]);
            const double y = static_cast<double>(matrix.m[1][axis]);
            const double z = static_cast<double>(matrix.m[2][axis]);
            const double lengthSquared = x * x + y * y + z * z;
            if (!std::isfinite(lengthSquared) || lengthSquared < 0.0)
                return false;
            outLength = (std::max)(outLength, std::sqrt(lengthSquared));
        }
        return std::isfinite(outLength);
    }

    inline bool HooksFirstPersonBodyLockAnimatedTorsoYaw(
        const HooksFirstPersonBodyBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4& targetAnchorTransform,
        vr_vm_stabilize::Mat3x4* bones,
        float& outCorrectionDeg)
    {
        outCorrectionDeg = 0.0f;
        if (!bones || layout.numBones <= 0 || layout.numBones > 128 ||
            layout.torsoYawRootBone < 0 ||
            layout.torsoYawRootBone >= layout.numBones ||
            layout.leftUpperArmBone < 0 ||
            layout.leftUpperArmBone >= layout.numBones ||
            layout.rightUpperArmBone < 0 ||
            layout.rightUpperArmBone >= layout.numBones ||
            static_cast<int>(layout.boneParents.size()) < layout.numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(targetAnchorTransform))
        {
            // Replacement rigs without a complete shoulder/spine reference keep
            // the existing whole-model yaw correction instead of losing the body.
            return true;
        }

        const Vector currentLeft = vr_vm_stabilize::GetOrigin(
            bones[layout.leftUpperArmBone]);
        const Vector currentRight = vr_vm_stabilize::GetOrigin(
            bones[layout.rightUpperArmBone]);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(currentLeft) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(currentRight))
        {
            return false;
        }

        Vector currentShoulderLine = currentRight - currentLeft;
        // BuildFromOrgAngles stores the body's physical right axis in column 1.
        // The named left-to-right upper-arm line must share that yaw regardless
        // of custom-model bind-pose asymmetry or Source aim-yaw animation.
        Vector targetShoulderLine(
            targetAnchorTransform.m[0][1],
            targetAnchorTransform.m[1][1],
            targetAnchorTransform.m[2][1]);
        currentShoulderLine.z = 0.0f;
        targetShoulderLine.z = 0.0f;
        const float currentLength = currentShoulderLine.Length();
        const float targetLength = targetShoulderLine.Length();
        if (!std::isfinite(currentLength) || !std::isfinite(targetLength) ||
            currentLength <= 0.01f || targetLength <= 0.01f)
        {
            return true;
        }

        currentShoulderLine *= 1.0f / currentLength;
        targetShoulderLine *= 1.0f / targetLength;
        const float dot = std::clamp(
            DotProduct(currentShoulderLine, targetShoulderLine),
            -1.0f,
            1.0f);
        const float crossZ =
            currentShoulderLine.x * targetShoulderLine.y -
            currentShoulderLine.y * targetShoulderLine.x;
        const float correctionDeg = RAD2DEG(std::atan2(crossZ, dot));
        if (!std::isfinite(correctionDeg))
            return false;
        outCorrectionDeg = correctionDeg;
        if (std::fabs(correctionDeg) <= 0.001f)
            return true;

        const Vector pivot = vr_vm_stabilize::GetOrigin(
            bones[layout.torsoYawRootBone]);
        vr_vm_stabilize::Mat3x4 correctionDelta{};
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(pivot) ||
            !HooksNativeViewmodelArmIkBuildAxisRotationDelta(
                pivot,
                Vector(0.0f, 0.0f, 1.0f),
                DEG2RAD(correctionDeg),
                correctionDelta))
        {
            return false;
        }

        return HooksNativeViewmodelArmIkApplyDeltaToBranch(
            layout.boneParents,
            layout.numBones,
            layout.torsoYawRootBone,
            correctionDelta,
            bones);
    }

    inline bool HooksFirstPersonBodyLockAnimatedUpperChestFrame(
        const HooksFirstPersonBodyBoneLayout& layout,
        const vr_vm_stabilize::Mat3x4& desiredUpperChest,
        vr_vm_stabilize::Mat3x4* bones)
    {
        if (!bones || layout.numBones <= 0 || layout.numBones > 128 ||
            layout.upperChestBone < 0 ||
            layout.upperChestBone >= layout.numBones ||
            layout.headBone < 0 ||
            layout.headBone >= layout.numBones ||
            layout.leftUpperArmBone < 0 ||
            layout.leftUpperArmBone >= layout.numBones ||
            layout.rightUpperArmBone < 0 ||
            layout.rightUpperArmBone >= layout.numBones ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(desiredUpperChest))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 currentRigid{};
        if (!HooksFirstPersonBodyBuildGeometricTorsoFrame(
                vr_vm_stabilize::GetOrigin(bones[layout.upperChestBone]),
                vr_vm_stabilize::GetOrigin(bones[layout.leftUpperArmBone]),
                vr_vm_stabilize::GetOrigin(bones[layout.rightUpperArmBone]),
                vr_vm_stabilize::GetOrigin(bones[layout.headBone]),
                currentRigid))
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4 currentInverse{};
        vr_vm_stabilize::Mat3x4 correctionDelta{};
        vr_vm_stabilize::InvertTR(currentRigid, currentInverse);
        vr_vm_stabilize::Mul(
            desiredUpperChest,
            currentInverse,
            correctionDelta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(correctionDelta))
            return false;

        // Correct only the upper-chest branch around its own origin. The chest
        // stays attached at the same point while its neck/shoulder descendants
        // are made camera-safe. Pelvis and legs keep the native grounded pose;
        // child-local breathing, hit and airborne animation remains intact.
        return HooksNativeViewmodelArmIkApplyDeltaToBranch(
            layout.boneParents,
            layout.numBones,
            layout.upperChestBone,
            correctionDelta,
            bones);
    }

    inline bool HooksFirstPersonBodyBuildAnchoredBones(
        VR* vr,
        const HooksFirstPersonBodyEyeSceneState* bodyState,
        void* drawState,
        const ModelRenderInfo_t& modelInfo,
        const void* pCustomBoneToWorld,
        std::vector<vr_vm_stabilize::Mat3x4>& boneStorage,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!vr || !bodyState || !bodyState->bodyActive ||
            !drawState || !pCustomBoneToWorld ||
            !vr->m_FirstPersonBodyEnabled ||
            bodyState->playerGeneration == 0 ||
            bodyState->playerGeneration !=
            g_FirstPersonBodyPlayerGeneration.load(std::memory_order_acquire) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(bodyState->centerEyePosition) ||
            !std::isfinite(modelInfo.angles.y))
        {
            return false;
        }

        static thread_local HooksFirstPersonBodyBoneLayout s_layout{};
        if (!HooksFirstPersonBodyBuildBoneLayout(
            drawState,
            bodyState->playerGeneration,
            s_layout))
            return false;

        const auto* sourceBones =
            reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);

        try
        {
            boneStorage.resize(static_cast<size_t>(s_layout.numBones));
        }
        catch (...)
        {
            return false;
        }

        vr_vm_stabilize::Mat3x4* const anchoredBones = boneStorage.data();
        if (!anchoredBones)
            return false;

        for (int bone = 0; bone < s_layout.numBones; ++bone)
        {
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, anchoredBones[bone]) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(anchoredBones[bone]))
            {
                return false;
            }
        }

        const bool animationFreeAnchorsCaptured =
            HooksFirstPersonBodyCaptureAnimationFreeArmAnchors(
                drawState,
                s_layout);

        if (!HooksFirstPersonBodyCaptureFrozenLocalPose(s_layout, sourceBones))
            return false;

        // Freeze hidden head and complete shoulder/arm animation in parent-local
        // space. The head follows the neck and each clavicle branch follows the
        // upper chest, but shoulder raises, weapon poses, reloads, and arm swings
        // no longer move the visible shoulder/upper-arm cut.
        if (vr->m_FirstPersonBodyHideHead &&
            !HooksFirstPersonBodyApplyFrozenBranch(
                s_layout,
                s_layout.hideHeadMask,
                anchoredBones))
        {
            return false;
        }
        if (vr->m_FirstPersonBodyHideArms)
        {
            if (!HooksFirstPersonBodyApplyFrozenBranch(
                s_layout,
                s_layout.freezeLeftArmMask,
                anchoredBones) ||
                !HooksFirstPersonBodyApplyFrozenBranch(
                    s_layout,
                    s_layout.freezeRightArmMask,
                    anchoredBones))
            {
                return false;
            }
        }

        vr_vm_stabilize::Mat3x4 frozenHead{};
        if (!vr_vm_stabilize::SafeRead(
            anchoredBones + s_layout.headBone,
            frozenHead) ||
            !HooksNativeViewmodelHandsOnlyMatrixFinite(frozenHead))
        {
            return false;
        }

        const float visualBodyYaw = HooksTrackedBodyResolveVisualYaw(
            vr,
            bodyState->view.angles.y);
        if (!std::isfinite(visualBodyYaw))
            return false;
        QAngle anchorYaw(0.0f, visualBodyYaw, 0.0f);
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(anchorYaw, &forward, &right, &up);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(forward) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(right) ||
            !HooksNativeViewmodelHandsOnlyVectorFinite(up))
        {
            return false;
        }

        const float unitsPerMeter =
            (std::isfinite(vr->m_VRScale) && std::fabs(vr->m_VRScale) > 0.001f)
            ? std::fabs(vr->m_VRScale)
            : 43.2f;
        const Vector& anchorRotationOffsetDeg =
            vr->m_FirstPersonBodyAnchorRotationOffsetDeg;
        const bool useFallbackBodyCalibration =
            !s_layout.animationFreeAnchorUsesModelEye;
        const Vector& bodyOffsetMeters = useFallbackBodyCalibration
            ? vr->m_FirstPersonBodyFallbackAnchorOffsetMeters
            : vr->m_FirstPersonBodyAnchorOffsetMeters;
        const Vector& cameraOffsetMeters = useFallbackBodyCalibration
            ? vr->m_FirstPersonBodyFallbackCameraOffsetMeters
            : vr->m_FirstPersonBodyCameraOffsetMeters;

        // Reserve the clamped physical HMD planar offset for upper-body lean.
        // Subtracting it from the tracked eye center keeps the body anchor fixed
        // while the user moves inside the lean envelope. Beyond the envelope,
        // only the excess displacement translates the visible first-person body.
        const Vector hmdPlanarWorld(
            vr->m_SetupOriginToHMD.x,
            vr->m_SetupOriginToHMD.y,
            0.0f);
        const Vector rawBodyLeanLocalMeters(
            DotProduct(hmdPlanarWorld, forward) / unitsPerMeter,
            DotProduct(hmdPlanarWorld, right) / unitsPerMeter,
            0.0f);
        const Vector bodyLeanLocalMeters =
            HooksTrackedBodyClampPlanarLeanMeters(
                vr,
                rawBodyLeanLocalMeters);
        const Vector retainedLeanWorld =
            forward * (bodyLeanLocalMeters.x * unitsPerMeter) +
            right * (bodyLeanLocalMeters.y * unitsPerMeter);

        const Vector desiredModelAnchorPosition =
            bodyState->centerEyePosition - retainedLeanWorld -
            forward * (cameraOffsetMeters.x * unitsPerMeter) -
            right * (cameraOffsetMeters.y * unitsPerMeter) -
            up * (cameraOffsetMeters.z * unitsPerMeter) +
            forward * (bodyOffsetMeters.x * unitsPerMeter) +
            right * (bodyOffsetMeters.y * unitsPerMeter) +
            up * (bodyOffsetMeters.z * unitsPerMeter);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                desiredModelAnchorPosition))
            return false;

        // Viewmodel arm shoulders use the survivor model's StudioHdr rest pose,
        // transformed by the same positional body anchor and the deadzone-resolved
        // torso yaw. No current survivor animation matrix is sampled here. Raw
        // physical HMD yaw is never used directly; HMD translation can still drive
        // the shared upper-body lean.
        Vector animationFreeLeftArmShoulder{};
        Vector animationFreeRightArmShoulder{};
        bool animationFreeLeftArmShoulderValid = false;
        bool animationFreeRightArmShoulderValid = false;
        if (animationFreeAnchorsCaptured)
        {
            // Keep the arm roots on the same torso frame as the visible
            // first-person body. This yaw is already filtered by
            // WorldModelVRPoseBodyYawDeadzoneDeg, so physical HMD yaw inside
            // the comfort cone never reaches the arm IK. Once the torso itself
            // follows excess head yaw, the arm roots follow that torso motion.
            const float armYaw = visualBodyYaw;
            if (std::isfinite(armYaw))
            {
                Vector armForward{};
                Vector armRight{};
                Vector armUp{};
                QAngle::AngleVectors(
                    QAngle(0.0f, armYaw, 0.0f),
                    &armForward,
                    &armRight,
                    &armUp);

                if (HooksNativeViewmodelHandsOnlyVectorFinite(armForward) &&
                    HooksNativeViewmodelHandsOnlyVectorFinite(armRight) &&
                    HooksNativeViewmodelHandsOnlyVectorFinite(armUp))
                {
                    const Vector rawArmLeanLocalMeters(
                        DotProduct(hmdPlanarWorld, armForward) / unitsPerMeter,
                        DotProduct(hmdPlanarWorld, armRight) / unitsPerMeter,
                        0.0f);
                    const Vector armLeanLocalMeters =
                        HooksTrackedBodyClampPlanarLeanMeters(
                            vr,
                            rawArmLeanLocalMeters);
                    const Vector retainedArmLeanWorld =
                        armForward * (armLeanLocalMeters.x * unitsPerMeter) +
                        armRight * (armLeanLocalMeters.y * unitsPerMeter);

                    const Vector armDesiredModelAnchorPosition =
                        bodyState->centerEyePosition - retainedArmLeanWorld -
                        armForward *
                            (cameraOffsetMeters.x * unitsPerMeter) -
                        armRight *
                            (cameraOffsetMeters.y * unitsPerMeter) -
                        armUp *
                            (cameraOffsetMeters.z * unitsPerMeter) +
                        armForward *
                            (bodyOffsetMeters.x * unitsPerMeter) +
                        armRight *
                            (bodyOffsetMeters.y * unitsPerMeter) +
                        armUp *
                            (bodyOffsetMeters.z * unitsPerMeter);

                    const QAngle armAnchorAngles(
                        anchorRotationOffsetDeg.x,
                        HooksTrackedBodyWrapYaw(
                            armYaw + anchorRotationOffsetDeg.y),
                        anchorRotationOffsetDeg.z);
                    vr_vm_stabilize::Mat3x4 armAnchorTransform{};
                    vr_vm_stabilize::BuildFromOrgAngles(
                        armDesiredModelAnchorPosition,
                        armAnchorAngles,
                        armAnchorTransform);
                    if (HooksNativeViewmodelHandsOnlyVectorFinite(
                            armDesiredModelAnchorPosition) &&
                        HooksNativeViewmodelHandsOnlyMatrixFinite(
                            armAnchorTransform))
                    {
                        if (s_layout.animationFreeLeftShoulderValid)
                        {
                            animationFreeLeftArmShoulder = HooksTransformPoint(
                                armAnchorTransform,
                                s_layout.animationFreeLeftShoulderFromAnchor);
                            animationFreeLeftArmShoulderValid =
                                HooksNativeViewmodelHandsOnlyVectorFinite(
                                    animationFreeLeftArmShoulder);
                        }
                        if (s_layout.animationFreeRightShoulderValid)
                        {
                            animationFreeRightArmShoulder = HooksTransformPoint(
                                armAnchorTransform,
                                s_layout.animationFreeRightShoulderFromAnchor);
                            animationFreeRightArmShoulderValid =
                                HooksNativeViewmodelHandsOnlyVectorFinite(
                                    animationFreeRightArmShoulder);
                        }

                        Vector animationFreeUpperChestWorld{};
                        const bool animationFreeUpperChestWorldValid =
                            s_layout.animationFreeUpperChestValid &&
                            HooksNativeViewmodelHandsOnlyVectorFinite(
                                animationFreeUpperChestWorld = HooksTransformPoint(
                                    armAnchorTransform,
                                    s_layout.animationFreeUpperChestFromAnchor));

                        if (animationFreeUpperChestWorldValid &&
                            animationFreeLeftArmShoulderValid &&
                            animationFreeRightArmShoulderValid)
                        {
                            // Replacement survivor rigs are not required to keep
                            // the left/right upper-arm bind origins mirrored. Using
                            // their raw midpoint can therefore shift the complete
                            // viewmodel arm pair sideways even though the final
                            // shoulder spacing is symmetric. Keep only the raw
                            // shoulder midpoint's forward/up placement and force its
                            // lateral coordinate onto the upper-chest centreline.
                            // This makes the arm pair stay centred on the visible
                            // torso without importing any body animation or HMD yaw.
                            Vector shoulderCenter =
                                (animationFreeLeftArmShoulder +
                                    animationFreeRightArmShoulder) * 0.5f;
                            const Vector chestToShoulderCenter =
                                shoulderCenter - animationFreeUpperChestWorld;
                            shoulderCenter -= armRight * DotProduct(
                                chestToShoulderCenter,
                                armRight);

                            if (HooksNativeViewmodelHandsOnlyVectorFinite(
                                    shoulderCenter))
                            {
                                const float symmetricHalfWidth =
                                    0.18f * unitsPerMeter;
                                animationFreeLeftArmShoulder =
                                    shoulderCenter -
                                    armRight * symmetricHalfWidth;
                                animationFreeRightArmShoulder =
                                    shoulderCenter +
                                    armRight * symmetricHalfWidth;
                                animationFreeLeftArmShoulderValid =
                                    HooksNativeViewmodelHandsOnlyVectorFinite(
                                        animationFreeLeftArmShoulder);
                                animationFreeRightArmShoulderValid =
                                    HooksNativeViewmodelHandsOnlyVectorFinite(
                                        animationFreeRightArmShoulder);
                            }
                        }

                        if (animationFreeUpperChestWorldValid &&
                            (animationFreeLeftArmShoulderValid ||
                                animationFreeRightArmShoulderValid))
                        {
                            const Vector armLeanPivot =
                                animationFreeUpperChestWorld;
                            vr_vm_stabilize::Mat3x4 armLeanDelta{};
                            if (HooksTrackedBodyBuildLeanDelta(
                                    vr,
                                    armLeanPivot,
                                    armForward,
                                    armRight,
                                    armUp,
                                    armLeanLocalMeters,
                                    1.0f,
                                    armLeanDelta))
                            {
                                if (animationFreeLeftArmShoulderValid)
                                {
                                    animationFreeLeftArmShoulder =
                                        HooksTransformPoint(
                                            armLeanDelta,
                                            animationFreeLeftArmShoulder);
                                    animationFreeLeftArmShoulderValid =
                                        HooksNativeViewmodelHandsOnlyVectorFinite(
                                            animationFreeLeftArmShoulder);
                                }
                                if (animationFreeRightArmShoulderValid)
                                {
                                    animationFreeRightArmShoulder =
                                        HooksTransformPoint(
                                            armLeanDelta,
                                            animationFreeRightArmShoulder);
                                    animationFreeRightArmShoulderValid =
                                        HooksNativeViewmodelHandsOnlyVectorFinite(
                                            animationFreeRightArmShoulder);
                                }
                            }
                        }
                    }
                }
            }
        }

        const QAngle targetAnchorAngles(
            anchorRotationOffsetDeg.x,
            anchorYaw.y + anchorRotationOffsetDeg.y,
            anchorRotationOffsetDeg.z);
        vr_vm_stabilize::Mat3x4 targetModelAnchorTransform{};
        vr_vm_stabilize::BuildFromOrgAngles(
            desiredModelAnchorPosition,
            targetAnchorAngles,
            targetModelAnchorTransform);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(
                targetModelAnchorTransform))
            return false;

        const Vector desiredHeadPosition = HooksTransformPoint(
            targetModelAnchorTransform,
            s_layout.animationFreeHeadFromAnchor);
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(desiredHeadPosition))
            return false;

        Vector desiredUpperChestPosition{};
        bool desiredUpperChestPositionValid = false;
        if (animationFreeAnchorsCaptured &&
            s_layout.animationFreeUpperChestValid)
        {
            desiredUpperChestPosition = HooksTransformPoint(
                targetModelAnchorTransform,
                s_layout.animationFreeUpperChestFromAnchor);
            desiredUpperChestPositionValid =
                HooksNativeViewmodelHandsOnlyVectorFinite(
                    desiredUpperChestPosition);
        }

        vr_vm_stabilize::Mat3x4 desiredUpperChestTransform{};
        bool desiredUpperChestTransformValid = false;
        if (desiredUpperChestPositionValid &&
            s_layout.animationFreeTorsoFrameValid)
        {
            if (HooksFirstPersonBodyBuildProperBodyFrame(
                    targetAnchorAngles,
                    desiredUpperChestTransform))
            {
                desiredUpperChestTransform.m[0][3] =
                    desiredUpperChestPosition.x;
                desiredUpperChestTransform.m[1][3] =
                    desiredUpperChestPosition.y;
                desiredUpperChestTransform.m[2][3] =
                    desiredUpperChestPosition.z;
                desiredUpperChestTransformValid =
                    HooksNativeViewmodelHandsOnlyMatrixFinite(
                        desiredUpperChestTransform);
            }
        }

        // Native animation remains completely intact. We apply only rigid
        // post-animation corrections to the complete render skeleton. Anchoring
        // the visible upper chest (rather than the hidden animated head) prevents
        // locomotion poses from carrying the torso through the HMD while preserving
        // every bone-to-bone animated transform.
        int rigidAnchorBone = s_layout.headBone;
        Vector currentRigidAnchor = vr_vm_stabilize::GetOrigin(frozenHead);
        Vector desiredRigidAnchor = desiredHeadPosition;
        bool usingStableTorsoAnchor = false;
        if (desiredUpperChestPositionValid &&
            s_layout.upperChestBone >= 0 &&
            s_layout.upperChestBone < s_layout.numBones)
        {
            const Vector currentUpperChest = vr_vm_stabilize::GetOrigin(
                anchoredBones[s_layout.upperChestBone]);
            if (HooksNativeViewmodelHandsOnlyVectorFinite(currentUpperChest) &&
                HooksNativeViewmodelHandsOnlyVectorFinite(
                    desiredUpperChestPosition))
            {
                rigidAnchorBone = s_layout.upperChestBone;
                currentRigidAnchor = currentUpperChest;
                desiredRigidAnchor = desiredUpperChestPosition;
                usingStableTorsoAnchor = true;
            }
        }

        const Vector translation = desiredRigidAnchor - currentRigidAnchor;
        if (!HooksNativeViewmodelHandsOnlyVectorFinite(translation))
            return false;

        vr_vm_stabilize::Mat3x4 translationDelta = vr_vm_stabilize::Identity();
        translationDelta.m[0][3] = translation.x;
        translationDelta.m[1][3] = translation.y;
        translationDelta.m[2][3] = translation.z;
        vr_vm_stabilize::ApplyDelta(
            translationDelta,
            anchoredBones,
            s_layout.numBones);

        // The native first-person body draw bypasses hooks_world_pose.inl, so its
        // source bones still face the player's delayed render yaw. Rotate the whole
        // body to the shared tracked-body yaw around the same rigid torso anchor.
        // Physical HMD
        // yaw stays independent inside the configured comfort cone; thumbstick/mouse
        // turning still rotates the torso immediately. The configured pitch/yaw/roll
        // remains an additional body-local adjustment.
        const QAngle sourceAnchorAngles(
            0.0f,
            modelInfo.angles.y,
            0.0f);
        vr_vm_stabilize::Mat3x4 sourceAnchorTransform{};
        vr_vm_stabilize::Mat3x4 targetAnchorTransform{};
        vr_vm_stabilize::Mat3x4 sourceAnchorInverse{};
        vr_vm_stabilize::Mat3x4 anchorRotationDelta{};
        vr_vm_stabilize::BuildFromOrgAngles(
            desiredRigidAnchor,
            sourceAnchorAngles,
            sourceAnchorTransform);
        vr_vm_stabilize::BuildFromOrgAngles(
            desiredRigidAnchor,
            targetAnchorAngles,
            targetAnchorTransform);
        vr_vm_stabilize::InvertTR(sourceAnchorTransform, sourceAnchorInverse);
        vr_vm_stabilize::Mul(
            targetAnchorTransform,
            sourceAnchorInverse,
            anchorRotationDelta);
        if (!HooksNativeViewmodelHandsOnlyMatrixFinite(anchorRotationDelta))
            return false;
        vr_vm_stabilize::ApplyDelta(
            anchorRotationDelta,
            anchoredBones,
            s_layout.numBones);

        // A complete rest-pose upper-chest frame lets the stable path remove
        // residual animated yaw, pitch and roll with one whole-skeleton rigid
        // correction. Rigs lacking that reference retain the older yaw-only
        // shoulder solve and positional anchor as a compatibility fallback.
        float animatedTorsoYawCorrectionDeg = 0.0f;
        const bool usingStableTorsoFrame =
            usingStableTorsoAnchor && desiredUpperChestTransformValid;
        if (usingStableTorsoFrame)
        {
            if (!HooksFirstPersonBodyLockAnimatedUpperChestFrame(
                    s_layout,
                    desiredUpperChestTransform,
                    anchoredBones))
            {
                return false;
            }
        }
        else if (!HooksFirstPersonBodyLockAnimatedTorsoYaw(
                     s_layout,
                     targetAnchorTransform,
                     anchoredBones,
                     animatedTorsoYawCorrectionDeg))
        {
            return false;
        }

        if (!usingStableTorsoFrame)
        {
            // The fallback residual-yaw correction pivots only the spine branch,
            // so restore its positional anchor afterwards.
            const Vector orientedRigidAnchor = vr_vm_stabilize::GetOrigin(
                anchoredBones[rigidAnchorBone]);
            const Vector orientationAnchorCorrection =
                desiredRigidAnchor - orientedRigidAnchor;
            if (!HooksNativeViewmodelHandsOnlyVectorFinite(
                    orientationAnchorCorrection))
            {
                return false;
            }
            vr_vm_stabilize::Mat3x4 orientationAnchorDelta =
                vr_vm_stabilize::Identity();
            orientationAnchorDelta.m[0][3] = orientationAnchorCorrection.x;
            orientationAnchorDelta.m[1][3] = orientationAnchorCorrection.y;
            orientationAnchorDelta.m[2][3] = orientationAnchorCorrection.z;
            vr_vm_stabilize::ApplyDelta(
                orientationAnchorDelta,
                anchoredBones,
                s_layout.numBones);
        }

        {
            static std::mutex s_torsoYawLogMutex;
            static std::unordered_set<std::uintptr_t> s_torsoYawLogged;
            const std::uintptr_t logKey =
                reinterpret_cast<std::uintptr_t>(s_layout.studioHdr) ^
                static_cast<std::uintptr_t>(bodyState->playerGeneration);
            bool shouldLog = false;
            {
                std::lock_guard<std::mutex> lock(s_torsoYawLogMutex);
                shouldLog = s_torsoYawLogged.insert(logKey).second;
            }
            if (shouldLog)
            {
                float turnYaw = vr->m_RenderRotationOffset.load(
                    std::memory_order_acquire);
                if (!std::isfinite(turnYaw))
                    turnYaw = vr->m_RotationOffset;
                Game::logMsg(
                    "[VR][FirstPersonBody] torso frame lock deadzone=%.1f headYaw=%.1f turnYaw=%.1f visualYaw=%.1f modelYaw=%.1f animatedResidualYaw=%.1f root=%d rigidAnchor=%s bone=%d",
                    vr->m_WorldModelVRPoseBodyYawDeadzoneDeg,
                    bodyState->view.angles.y,
                    turnYaw,
                    visualBodyYaw,
                    modelInfo.angles.y,
                    animatedTorsoYawCorrectionDeg,
                    s_layout.torsoYawRootBone,
                    usingStableTorsoAnchor ? "upper-chest" : "head-fallback",
                    rigidAnchorBone);
            }
        }

        int bodyLeanRoot = s_layout.upperChestBone;
        if (bodyLeanRoot < 0 || bodyLeanRoot >= s_layout.numBones)
            bodyLeanRoot = s_layout.neckBone;
        if (bodyLeanRoot >= 0 && bodyLeanRoot < s_layout.numBones)
        {
            const Vector leanPivot =
                vr_vm_stabilize::GetOrigin(anchoredBones[bodyLeanRoot]);
            vr_vm_stabilize::Mat3x4 leanDelta{};
            if (HooksTrackedBodyBuildLeanDelta(
                    vr,
                    leanPivot,
                    forward,
                    right,
                    up,
                    bodyLeanLocalMeters,
                    1.0f,
                    leanDelta))
            {
                HooksNativeViewmodelArmIkApplyDeltaToBranch(
                    s_layout.boneParents,
                    s_layout.numBones,
                    bodyLeanRoot,
                    leanDelta,
                    anchoredBones);
            }
        }

        // Capture the complete body skeleton after its final translation, torso
        // rotation and upper-chest lean, but before head/arm render trimming. The
        // viewmodel was authored by deleting body mesh while retaining this same
        // skeleton, so matching these matrices by bone name gives it the real
        // spine -> clavicle -> upper-arm attachment instead of a hand-placed point.
        std::vector<vr_vm_stabilize::Mat3x4> viewmodelSkeletonBones;
        if (static_cast<int>(s_layout.boneNames.size()) != s_layout.numBones)
            return false;
        try
        {
            viewmodelSkeletonBones.assign(
                anchoredBones,
                anchoredBones + s_layout.numBones);
        }
        catch (...)
        {
            return false;
        }
        for (const vr_vm_stabilize::Mat3x4& bone : viewmodelSkeletonBones)
        {
            if (!HooksNativeViewmodelHandsOnlyMatrixFinite(bone))
                return false;
        }

        auto collapseOriginForRoot = [&](int rootBone) -> Vector
            {
                if (rootBone < 0 || rootBone >= s_layout.numBones)
                    return desiredHeadPosition;

                int collapseBone = rootBone;
                const int parent = s_layout.boneParents[static_cast<size_t>(rootBone)];
                if (parent >= 0 && parent < s_layout.numBones)
                    collapseBone = parent;

                const Vector origin = vr_vm_stabilize::GetOrigin(anchoredBones[collapseBone]);
                return HooksNativeViewmodelHandsOnlyVectorFinite(origin)
                    ? origin
                    : desiredHeadPosition;
            };

        const Vector headCollapseOrigin =
            collapseOriginForRoot(s_layout.headBone);
        const vr_vm_stabilize::Mat3x4 collapsedHead =
            HooksNativeViewmodelHandsOnlyCollapsedBoneAt(headCollapseOrigin);

        for (int bone = 0; bone < s_layout.numBones; ++bone)
        {
            const size_t index = static_cast<size_t>(bone);
            if (vr->m_FirstPersonBodyHideHead && s_layout.hideHeadMask[index])
                anchoredBones[bone] = collapsedHead;
        }

        auto trimArmBelowShoulder = [&](
            int upperArmBone,
            int forearmBone,
            const std::vector<uint8_t>& hideForearmMask)
            {
                if (!vr->m_FirstPersonBodyHideArms ||
                    upperArmBone < 0 || upperArmBone >= s_layout.numBones ||
                    forearmBone < 0 || forearmBone >= s_layout.numBones ||
                    static_cast<int>(hideForearmMask.size()) < s_layout.numBones)
                {
                    return;
                }

                const Vector shoulderOrigin =
                    vr_vm_stabilize::GetOrigin(anchoredBones[upperArmBone]);
                const Vector elbowOrigin =
                    vr_vm_stabilize::GetOrigin(anchoredBones[forearmBone]);
                Vector shoulderToElbow = elbowOrigin - shoulderOrigin;
                const float armLength = std::sqrt(
                    shoulderToElbow.x * shoulderToElbow.x +
                    shoulderToElbow.y * shoulderToElbow.y +
                    shoulderToElbow.z * shoulderToElbow.z);
                if (!std::isfinite(armLength) || armLength <= 0.001f)
                    return;

                shoulderToElbow *= (1.0f / armLength);
                const float configuredVisibleMeters = std::clamp(
                    vr->m_FirstPersonBodyVisibleUpperArmLengthMeters,
                    0.0f,
                    0.40f);
                const float visibleLength = (std::min)(
                    armLength,
                    configuredVisibleMeters * unitsPerMeter);
                const float lengthScale = visibleLength / armLength;

                // ValveBiped normally uses local X along the limb, but replacement
                // models are not guaranteed to do so. Scale the matrix basis column
                // most closely aligned with the shoulder-to-elbow direction.
                int limbAxis = 0;
                float bestAlignment = -1.0f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    Vector basis(
                        anchoredBones[upperArmBone].m[0][axis],
                        anchoredBones[upperArmBone].m[1][axis],
                        anchoredBones[upperArmBone].m[2][axis]);
                    const float basisLength = std::sqrt(
                        basis.x * basis.x + basis.y * basis.y + basis.z * basis.z);
                    if (!std::isfinite(basisLength) || basisLength <= 0.001f)
                        continue;

                    basis *= (1.0f / basisLength);
                    const float alignment = std::fabs(
                        basis.x * shoulderToElbow.x +
                        basis.y * shoulderToElbow.y +
                        basis.z * shoulderToElbow.z);
                    if (alignment > bestAlignment)
                    {
                        bestAlignment = alignment;
                        limbAxis = axis;
                    }
                }

                for (int row = 0; row < 3; ++row)
                    anchoredBones[upperArmBone].m[row][limbAxis] *= lengthScale;

                const Vector cutOrigin =
                    shoulderOrigin + shoulderToElbow * visibleLength;
                const vr_vm_stabilize::Mat3x4 collapsedAtCut =
                    HooksNativeViewmodelHandsOnlyCollapsedBoneAt(cutOrigin);
                for (int bone = 0; bone < s_layout.numBones; ++bone)
                {
                    if (hideForearmMask[static_cast<size_t>(bone)])
                        anchoredBones[bone] = collapsedAtCut;
                }
            };

        trimArmBelowShoulder(
            s_layout.leftUpperArmBone,
            s_layout.leftForearmBone,
            s_layout.hideLeftArmMask);
        trimArmBelowShoulder(
            s_layout.rightUpperArmBone,
            s_layout.rightForearmBone,
            s_layout.hideRightArmMask);

        // Fail closed if a replacement model supplies a transform that our
        // frozen-branch reconstruction amplifies. Rigid anchoring preserves
        // basis lengths and arm trimming can only reduce them, so a large
        // increase here is never intentional.
        for (int bone = 0; bone < s_layout.numBones; ++bone)
        {
            const size_t index = static_cast<size_t>(bone);
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source) ||
                !HooksNativeViewmodelHandsOnlyMatrixFinite(source))
            {
                return false;
            }

            double sourceBasisLength = 0.0;
            double outputBasisLength = 0.0;
            if (!HooksFirstPersonBodyMaxBasisLength(source, sourceBasisLength) ||
                !HooksFirstPersonBodyMaxBasisLength(
                    anchoredBones[bone],
                    outputBasisLength))
            {
                return false;
            }

            const bool intentionallyCollapsed =
                (vr->m_FirstPersonBodyHideHead &&
                    s_layout.hideHeadMask[index] != 0u) ||
                (vr->m_FirstPersonBodyHideArms &&
                    (s_layout.hideLeftArmMask[index] != 0u ||
                        s_layout.hideRightArmMask[index] != 0u));
            if (intentionallyCollapsed)
            {
                if (outputBasisLength > 1.0e-4)
                    return false;
                continue;
            }

            const double allowedBasisLength =
                (std::max)(0.02, sourceBasisLength * 4.0);
            if (outputBasisLength > 16.0 ||
                outputBasisLength > allowedBasisLength)
            {
                return false;
            }
        }

        HooksFirstPersonBodyPublishViewmodelSkeleton(
            vr,
            bodyState,
            useFallbackBodyCalibration,
            s_layout.boneNames,
            viewmodelSkeletonBones);

        outBones = anchoredBones;
        return true;
    }

    inline void ApplyMagazineInteractionViewmodelOverride(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& info,
        void*& pCustomBoneToWorld)
    {
        MagazineInteractionFrozenViewmodelPoseCache& frozenCache = GetMagazineInteractionFrozenViewmodelPoseCache();

        const bool magazineInteractionActive = vr && vr->IsMagazineInteractionViewmodelOverrideActive();
        if (!vr || !magazineInteractionActive)
        {
            if (!vr || frozenCache.owner == vr)
                frozenCache.Reset();
            return;
        }
        if (!drawState || !pCustomBoneToWorld)
            return;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsViewmodel(lowerModel))
            return;

        const std::string lockedModel =
            vr_vm_stabilize::ToLowerAscii(vr->m_MagazineInteractionMagazineModelName);
        const bool nativeReloadSuppressed =
            vr->m_MagazineInteractionNativeReloadSuppressUntil.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() <= vr->m_MagazineInteractionNativeReloadSuppressUntil;
        const bool isArmsOrHandsModel = HooksModelNameIsArmsOrHands(lowerModel);
        if (!nativeReloadSuppressed &&
            (lockedModel.empty() || lockedModel != lowerModel) &&
            !isArmsOrHandsModel)
        {
            return;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones)
            return;

        vr_vm_stabilize::Mat3x4 modelAnchor{};
        if (!TryGetMagazineInteractionModelAnchor(info, modelAnchor))
            return;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        int clipBone = -1;
        if (!lockedModel.empty() && lockedModel == lowerModel)
        {
            const int lockedBone = vr->m_MagazineInteractionMagazineBoneIndex;
            if (lockedBone >= 0 && lockedBone < numBones)
                clipBone = lockedBone;
        }

        int magazineInteractionBoltBone = -1;
        if (vr->m_MagazineInteractionBoltRestValid)
        {
            const std::string boltModel = vr_vm_stabilize::ToLowerAscii(
                !vr->m_MagazineInteractionBoltRestBox.modelName.empty()
                ? vr->m_MagazineInteractionBoltRestBox.modelName
                : vr->m_MagazineInteractionMagazineModelName);
            if (!boltModel.empty() && boltModel == lowerModel)
            {
                const int lockedBolt = vr->m_MagazineInteractionBoltRestBox.boneIndex;
                if (lockedBolt >= 0 && lockedBolt < numBones)
                    magazineInteractionBoltBone = lockedBolt;
            }
        }

        const bool visuallyPauseViewmodel = vr->ShouldFreezeMagazineInteractionViewmodel();
        const bool hideNativeClip = vr->ShouldHideMagazineInteractionNativeClip() && clipBone >= 0;
        const bool moveBolt = vr->ShouldMoveMagazineInteractionBolt() && magazineInteractionBoltBone >= 0;
        if (!visuallyPauseViewmodel && frozenCache.owner == vr)
            frozenCache.models.erase(lowerModel);
        if (!visuallyPauseViewmodel && !hideNativeClip && !moveBolt)
        {
            return;
        }

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_relaxed) & ~1u;
        if (seqEven == 0)
            seqEven = (static_cast<uint32_t>(GetTickCount()) << 1u) | 2u;
        vr_vm_stabilize::Mat3x4* copiedBones = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!copiedBones)
            return;

        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, copiedBones[bone]))
                return;
        }

        if (visuallyPauseViewmodel)
        {
            if (frozenCache.owner != vr)
            {
                frozenCache.Reset();
                frozenCache.owner = vr;
            }

            MagazineInteractionFrozenViewmodelPoseEntry& frozenPose = frozenCache.models[lowerModel];
            int rootBone = FindMagazineInteractionTopLevelBone(boneParents, clipBone);
            if (nativeReloadSuppressed &&
                frozenPose.valid &&
                frozenPose.modelName == modelName &&
                frozenPose.numBones == numBones &&
                static_cast<int>(frozenPose.frozenLocalBones.size()) == numBones)
            {
                rootBone = frozenPose.rootBone;
            }
            const bool needCapture =
                !frozenPose.valid ||
                frozenPose.modelName != modelName ||
                frozenPose.numBones != numBones ||
                frozenPose.rootBone != rootBone ||
                static_cast<int>(frozenPose.frozenLocalBones.size()) != numBones;

            if (needCapture)
            {
                frozenPose = {};
                frozenPose.modelName = modelName;
                frozenPose.numBones = numBones;
                frozenPose.rootBone = rootBone;
                frozenPose.valid = BuildMagazineInteractionLocalBones(
                    modelAnchor,
                    sourceBones,
                    numBones,
                    frozenPose.frozenLocalBones);
                if (frozenPose.valid)
                {
                    Game::logMsg(
                        "[VR][MagazineInteraction] cached frozen viewmodel pose model=%s bones=%d root=%d",
                        modelName.c_str(),
                        numBones,
                        rootBone);
                }
            }

            if (frozenPose.valid)
            {
                ApplyMagazineInteractionLocalPose(modelAnchor, frozenPose.frozenLocalBones, copiedBones, numBones);
            }
        }

        ApplyMagazineInteractionBoltPose(
            vr,
            modelName,
            info,
            boneParents,
            magazineInteractionBoltBone,
            copiedBones,
            numBones);

        if (hideNativeClip &&
            !vr->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed))
        {
            DrawCurrentWeaponMagazineBox(
                vr,
                drawState,
                modelName,
                HooksModelNameIsViewmodel(lowerModel),
                info.entity_index,
                info.hitboxset,
                info.pModelToWorld,
                copiedBones);

            auto isClipOrDescendant = [&](int bone)
                {
                    int current = bone;
                    for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
                    {
                        if (current == clipBone)
                            return true;
                        current = boneParents[static_cast<size_t>(current)];
                    }
                    return false;
                };

            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!isClipOrDescendant(bone))
                    continue;
                copiedBones[bone].m[0][3] += 100000.0f;
                copiedBones[bone].m[1][3] += 100000.0f;
                copiedBones[bone].m[2][3] += 100000.0f;
            }
        }

        pCustomBoneToWorld = copiedBones;
    }

#include "hooks_world_pose.inl"

    inline std::string DescribeCallerAddress(const void* address)
    {
        if (!address)
            return "unknown";

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || !mbi.AllocationBase)
            return "unknown";

        char path[MAX_PATH] = {};
        const DWORD pathLen = GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH);
        const char* moduleName = (pathLen > 0) ? path : "unknown";
        if (pathLen > 0)
        {
            const char* slash = std::strrchr(path, '\\');
            if (slash && slash[1] != '\0')
                moduleName = slash + 1;
        }

        char buffer[MAX_PATH + 64] = {};
        const uintptr_t offset =
            reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        sprintf_s(buffer, "%s+0x%IX", moduleName, offset);
        return buffer;
    }

    inline void TraceTrackedConVarWrite(
        void* convar,
        const char* requestedValue,
        const char* hookPath,
        const void* callerAddress,
        bool isIConVarThis,
        bool blocked)
    {
        if (!Hooks::m_Game || !Hooks::m_VR || Game::HasConVarWritePermit() || !Hooks::m_VR->m_LocalVScriptConvarsLogEnabled)
            return;

        const char* name = isIConVarThis
            ? Hooks::m_Game->GetConVarNameFromIConVarPointer(convar)
            : Hooks::m_Game->GetConVarNameFromPointer(convar);
        if (!name || !*name)
            return;

        std::string expectedValue;
        if (!Hooks::m_VR->TryGetTrackedProtectedConvarValue(name, expectedValue))
            return;

        std::string throttleKey = std::string(hookPath ? hookPath : "ConVarTrace") + "|" + name;
        if (ShouldThrottleTrackedConVarTrace(throttleKey))
            return;

        const std::string beforeValue = Hooks::m_Game->GetConVarString(name);
        const std::string caller = DescribeCallerAddress(callerAddress);
        Game::logMsg(
            "[VR][LocalVScriptConvars] TraceWrite path=%s name=%s before='%s' expected='%s' requested='%s' caller=%s blocked=%d",
            hookPath ? hookPath : "unknown",
            name,
            beforeValue.c_str(),
            expectedValue.c_str(),
            requestedValue ? requestedValue : "",
            caller.c_str(),
            blocked ? 1 : 0);
    }

    inline bool ShouldBlockLockedConVarWrite(void* convar, const char* requestedValue)
    {
        if (!Hooks::m_Game || !Hooks::m_VR || Game::HasConVarWritePermit())
            return false;

        const char* name = Hooks::m_Game->GetConVarNameFromIConVarPointer(convar);
        if (!name || !*name)
            return false;

        return Hooks::m_VR->ShouldBlockExternalProtectedConvarWrite(
            name,
            requestedValue ? requestedValue : "");
    }
}

void Hooks::dAdjustEngineViewport(int& x, int& y, int& width, int& height)
{
    hkAdjustEngineViewport.fOriginal(x, y, width, height);
}

void Hooks::dViewport(void* ecx, void* edx, int x, int y, int width, int height)
{
    hkViewport.fOriginal(ecx, x, y, width, height);
}

void Hooks::dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    hkGetViewport.fOriginal(ecx, x, y, width, height);
}

int Hooks::dTestMeleeSwingCollisionClient(void* ecx, void* edx, Vector const& vec)
{
    const int result = hkTestMeleeSwingCollisionClient.fOriginal(ecx, vec);
    NotifyLocalMeleeCollisionHaptics(false, ecx, result, -1, -1);
    return result;
}

int Hooks::dTestMeleeSwingCollisionServer(void* ecx, void* edx, Vector const& vec)
{
    Server_WeaponCSBase* weapon = reinterpret_cast<Server_WeaponCSBase*>(ecx);
    const int entitiesHitBefore = weapon ? weapon->entitiesHitThisSwing : -1;
    const int result = hkTestMeleeSwingCollisionServer.fOriginal(ecx, vec);
    const int entitiesHitAfter = weapon ? weapon->entitiesHitThisSwing : entitiesHitBefore;
    NotifyLocalMeleeCollisionHaptics(true, ecx, result, entitiesHitBefore, entitiesHitAfter);
    return result;
}

void Hooks::dDoMeleeSwingServer(void* ecx, void* edx)
{
    return hkDoMeleeSwingServer.fOriginal(ecx);
}

void Hooks::dStartMeleeSwingServer(void* ecx, void* edx, void* player, bool a3)
{
    return hkStartMeleeSwingServer.fOriginal(ecx, player, a3);
}

int Hooks::dPrimaryAttackServer(void* ecx, void* edx)
{
    return hkPrimaryAttackServer.fOriginal(ecx);
}

void Hooks::dItemPostFrameServer(void* ecx, void* edx)
{
    hkItemPostFrameServer.fOriginal(ecx);

    if (!m_VR || !ecx || !IsLocalServerActiveWeapon(ecx))
        return;

    Server_WeaponCSBase* weapon = reinterpret_cast<Server_WeaponCSBase*>(ecx);
    int weaponId = 0;
#ifdef _MSC_VER
    __try
    {
        weaponId = weapon->GetWeaponID();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
#else
    weaponId = weapon->GetWeaponID();
#endif

    void* serverPlayer = m_Game ? reinterpret_cast<void*>(m_Game->m_CurrentUsercmdPlayer) : nullptr;
    m_VR->MarkMagazineInteractionServerHookSeen(weaponId);
    if (MagazineInteractionWeaponIdIsShotgun(weaponId))
    {
        m_VR->TryApplyMagazineInteractionShotgunServerReloadAbort(ecx, weaponId, serverPlayer);
        return;
    }

    m_VR->TryApplyMagazineInteractionServerClipCommit(ecx, weaponId, serverPlayer);
}

int Hooks::dGetPrimaryAttackActivity(void* ecx, void* edx, void* meleeInfo)
{
    return hkGetPrimaryAttackActivity.fOriginal(ecx, meleeInfo);
}

namespace
{
    thread_local bool g_ServerUseControllerAimOverride = false;
    thread_local void* g_ServerUseControllerAimPlayer = nullptr;
    thread_local Vector g_ServerUseControllerAimOrigin = { 0.0f, 0.0f, 0.0f };
    thread_local QAngle g_ServerUseControllerAimAngles = { 0.0f, 0.0f, 0.0f };
    thread_local bool g_ClientUseControllerAimOverride = false;
    thread_local void* g_ClientUseControllerAimPlayer = nullptr;
    thread_local Vector g_ClientUseControllerAimOrigin = { 0.0f, 0.0f, 0.0f };
    thread_local QAngle g_ClientUseControllerAimAngles = { 0.0f, 0.0f, 0.0f };

    static inline bool IsFiniteVector3(const Vector& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    static bool IsServerUseControllerAimWindowActive()
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return false;

        if (Hooks::m_VR->m_ForceNonVRServerMovement)
            return false;

        const auto now = std::chrono::steady_clock::now();
        return Hooks::m_VR->m_ServerUseControllerAimActive ||
            (Hooks::m_VR->m_ServerUseControllerAimUntil.time_since_epoch().count() != 0 &&
                now <= Hooks::m_VR->m_ServerUseControllerAimUntil);
    }

    static bool TryBuildServerUseControllerPose(void* player, Vector& origin, QAngle& angles)
    {
        if (!Hooks::m_Game || !player)
            return false;

        if (!Hooks::m_VR || Hooks::m_VR->m_ForceNonVRServerMovement)
            return false;

        const int playerIndex = Hooks::m_Game->m_CurrentUsercmdID;
        if (!Hooks::m_Game->IsValidPlayerIndex(playerIndex))
            return false;

        if (Hooks::m_Game->m_CurrentUsercmdPlayer &&
            reinterpret_cast<void*>(Hooks::m_Game->m_CurrentUsercmdPlayer) != player)
            return false;

        const Player& vrPlayer = Hooks::m_Game->m_PlayersVRInfo[playerIndex];
        if (!vrPlayer.isUsingVR)
            return false;

        origin = vrPlayer.controllerPos;
        angles = vrPlayer.controllerAngle;
        NormalizeAndClampViewAngles(angles);

        return IsFiniteVector3(origin) && IsFiniteViewAngle(angles);
    }

    static bool TryGetServerControllerAimOverride(void* player, Vector& origin, QAngle& angles, int& reason)
    {
        if (!player)
            return false;

        if (g_ServerUseControllerAimOverride && player == g_ServerUseControllerAimPlayer)
        {
            origin = g_ServerUseControllerAimOrigin;
            angles = g_ServerUseControllerAimAngles;
            reason = 1;
            return IsFiniteVector3(origin) && IsFiniteViewAngle(angles);
        }

        if (Hooks::m_ServerCommandControllerAimOverride &&
            player == Hooks::m_ServerCommandControllerAimPlayer)
        {
            origin = Hooks::m_ServerCommandControllerAimOrigin;
            angles = Hooks::m_ServerCommandControllerAimAngles;
            reason = Hooks::m_ServerCommandControllerAimReason;
            return IsFiniteVector3(origin) && IsFiniteViewAngle(angles);
        }

        return false;
    }

    static bool TryBuildClientUseControllerPose(void* player, Vector& origin, QAngle& angles)
    {
        if (!Hooks::m_Game || !Hooks::m_VR || !player)
            return false;

        if (!Hooks::m_VR->m_IsVREnabled)
            return false;

        if (Hooks::m_VR->m_ForceNonVRServerMovement)
            return false;

        if (!Hooks::m_Game->m_EngineClient || !Hooks::m_Game->m_EngineClient->IsInGame())
            return false;

        const int localPlayerIndex = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
        if (localPlayerIndex <= 0 || !Hooks::m_Game->IsValidPlayerIndex(localPlayerIndex))
            return false;

        C_BaseEntity* localPlayer = Hooks::m_Game->GetClientEntity(localPlayerIndex);
        if (!localPlayer || reinterpret_cast<void*>(localPlayer) != player)
            return false;

        origin = Hooks::m_VR->GetRightControllerAbsPos();
        angles = Hooks::m_VR->GetRightControllerAbsAngle();
        NormalizeAndClampViewAngles(angles);

        return IsFiniteVector3(origin) && IsFiniteViewAngle(angles);
    }

    class ScopedServerUseControllerAimOverride
    {
    public:
        ScopedServerUseControllerAimOverride(void* player, const Vector& origin, const QAngle& angles)
            : m_prevActive(g_ServerUseControllerAimOverride),
            m_prevPlayer(g_ServerUseControllerAimPlayer),
            m_prevOrigin(g_ServerUseControllerAimOrigin),
            m_prevAngles(g_ServerUseControllerAimAngles)
        {
            g_ServerUseControllerAimOverride = true;
            g_ServerUseControllerAimPlayer = player;
            g_ServerUseControllerAimOrigin = origin;
            g_ServerUseControllerAimAngles = angles;
        }

        ~ScopedServerUseControllerAimOverride()
        {
            g_ServerUseControllerAimOverride = m_prevActive;
            g_ServerUseControllerAimPlayer = m_prevPlayer;
            g_ServerUseControllerAimOrigin = m_prevOrigin;
            g_ServerUseControllerAimAngles = m_prevAngles;
        }

    private:
        bool m_prevActive;
        void* m_prevPlayer;
        Vector m_prevOrigin;
        QAngle m_prevAngles;
    };

    class ScopedClientUseControllerAimOverride
    {
    public:
        ScopedClientUseControllerAimOverride(void* player, const Vector& origin, const QAngle& angles)
            : m_prevActive(g_ClientUseControllerAimOverride),
            m_prevPlayer(g_ClientUseControllerAimPlayer),
            m_prevOrigin(g_ClientUseControllerAimOrigin),
            m_prevAngles(g_ClientUseControllerAimAngles)
        {
            g_ClientUseControllerAimOverride = true;
            g_ClientUseControllerAimPlayer = player;
            g_ClientUseControllerAimOrigin = origin;
            g_ClientUseControllerAimAngles = angles;
        }

        ~ScopedClientUseControllerAimOverride()
        {
            g_ClientUseControllerAimOverride = m_prevActive;
            g_ClientUseControllerAimPlayer = m_prevPlayer;
            g_ClientUseControllerAimOrigin = m_prevOrigin;
            g_ClientUseControllerAimAngles = m_prevAngles;
        }

    private:
        bool m_prevActive;
        void* m_prevPlayer;
        Vector m_prevOrigin;
        QAngle m_prevAngles;
    };
}

Vector* Hooks::dEyePosition(void* ecx, void* edx, Vector* eyePos)
{
    Vector controllerOrigin;
    QAngle controllerAngles;
    int overrideReason = 0;
    if (eyePos && TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
    {
        *eyePos = controllerOrigin;
        return eyePos;
    }

    Vector* result = hkEyePosition.fOriginal(ecx, eyePos);

    if (m_Game->m_PerformingMelee)
    {
        int i = m_Game->m_CurrentUsercmdID;
        if (m_Game->IsValidPlayerIndex(i))
        {
            *result = m_Game->m_PlayersVRInfo[i].controllerPos;
        }
    }

    return result;
}

Vector* Hooks::dServerPlayerEyePosition(void* ecx, void* edx, Vector* eyePos)
{
    Vector controllerOrigin;
    QAngle controllerAngles;
    int overrideReason = 0;
    if (eyePos && TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
    {
        static std::chrono::steady_clock::time_point s_lastServerEyePositionOverrideLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastServerEyePositionOverrideLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastServerEyePositionOverrideLog).count() >= 0.50f)
        {
            s_lastServerEyePositionOverrideLog = now;
            Game::logMsg(
                "[VR][UseAim] ServerPlayerEyePosition override reason=%d origin=(%.1f %.1f %.1f)",
                overrideReason,
                controllerOrigin.x,
                controllerOrigin.y,
                controllerOrigin.z);
        }
        *eyePos = controllerOrigin;
        return eyePos;
    }

    Vector* result = hkServerPlayerEyePosition.fOriginal(ecx, eyePos);
    return result;
}

Vector* Hooks::dClientPlayerEyePosition(void* ecx, void* edx, Vector* eyePos)
{
    if (eyePos && g_ClientUseControllerAimOverride && ecx == g_ClientUseControllerAimPlayer)
    {
        *eyePos = g_ClientUseControllerAimOrigin;
        return eyePos;
    }

    return hkClientPlayerEyePosition.fOriginal(ecx, eyePos);
}

void Hooks::dClientPlayerEyeVectors(void* ecx, void* edx, Vector* forward, Vector* right, Vector* up)
{
    if (g_ClientUseControllerAimOverride && ecx == g_ClientUseControllerAimPlayer)
    {
        QAngle::AngleVectors(g_ClientUseControllerAimAngles, forward, right, up);
        return;
    }

    hkClientPlayerEyeVectors.fOriginal(ecx, forward, right, up);
}

const QAngle* Hooks::dServerPlayerEyeAngles(void* ecx, void* edx)
{
    Vector controllerOrigin;
    QAngle controllerAngles;
    int overrideReason = 0;
    if (TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
    {
        static std::chrono::steady_clock::time_point s_lastServerEyeAnglesOverrideLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastServerEyeAnglesOverrideLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastServerEyeAnglesOverrideLog).count() >= 0.50f)
        {
            s_lastServerEyeAnglesOverrideLog = now;
            Game::logMsg(
                "[VR][UseAim] ServerPlayerEyeAngles override reason=%d angles=(%.1f %.1f %.1f)",
                overrideReason,
                controllerAngles.x,
                controllerAngles.y,
                controllerAngles.z);
        }
        Hooks::m_ServerCommandControllerAimAngles = controllerAngles;
        return &Hooks::m_ServerCommandControllerAimAngles;
    }

    return hkServerPlayerEyeAngles.fOriginal(ecx);
}

void Hooks::dPlayerUse(void* ecx, void* edx, void* useEntity)
{
    if (void* objectPullTarget =
        ObjectPullSelectPendingNativePickupTarget(ecx))
    {
        // This invocation came from the real ProcessUsercmds pass for our one
        // injected +use edge. Use the exact entity that reached the controller
        // instead of letting a nearby item win the ordinary aim lookup.
        useEntity = objectPullTarget;
        if (m_VR && m_VR->m_ObjectPullDebugLog)
        {
            Game::logMsg(
                "[VR][ObjectPull][server] PlayerUse native usercmd target override player=%d entity=%p",
                m_Game ? m_Game->m_CurrentUsercmdID : -1,
                objectPullTarget);
        }
    }

    const bool useAimActive = IsServerUseControllerAimWindowActive();
    if (useAimActive)
    {
        static std::chrono::steady_clock::time_point s_lastPlayerUseEntryLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastPlayerUseEntryLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastPlayerUseEntryLog).count() >= 0.50f)
        {
            s_lastPlayerUseEntryLog = now;
            const int playerIndex = m_Game ? m_Game->m_CurrentUsercmdID : -1;
            const bool validPlayerIndex = m_Game && m_Game->IsValidPlayerIndex(playerIndex);
            const bool isUsingVR = validPlayerIndex && m_Game->m_PlayersVRInfo[playerIndex].isUsingVR;
            Game::logMsg(
                "[VR][UseAim] PlayerUse entry usercmd=%d isVR=%d hasAim=%d player=%p current=%p useEntity=%p",
                playerIndex,
                isUsingVR ? 1 : 0,
                (m_VR && m_VR->m_HasNonVRAimSolution) ? 1 : 0,
                ecx,
                m_Game ? reinterpret_cast<void*>(m_Game->m_CurrentUsercmdPlayer) : nullptr,
                useEntity);
        }
    }

    Vector controllerOrigin;
    QAngle controllerAngles;
    if (useAimActive && TryBuildServerUseControllerPose(ecx, controllerOrigin, controllerAngles))
    {
        static std::chrono::steady_clock::time_point s_lastPlayerUseLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastPlayerUseLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastPlayerUseLog).count() >= 0.50f)
        {
            s_lastPlayerUseLog = now;
            Game::logMsg(
                "[VR][UseAim] PlayerUse override origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f)",
                controllerOrigin.x, controllerOrigin.y, controllerOrigin.z,
                controllerAngles.x, controllerAngles.y, controllerAngles.z);
        }
        ScopedServerUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
        hkPlayerUse.fOriginal(ecx, useEntity);
        ObjectPullCompleteNativePickupUsercmd(
            ecx,
            useEntity);
        return;
    }

    hkPlayerUse.fOriginal(ecx, useEntity);
    ObjectPullCompleteNativePickupUsercmd(
        ecx,
        useEntity);
}

Server_BaseEntity* Hooks::dFindUseEntity(void* ecx, void* edx, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra)
{
    if (void* objectPullTarget =
        ObjectPullSelectPendingNativePickupTarget(ecx))
    {
        return reinterpret_cast<Server_BaseEntity*>(
            objectPullTarget);
    }

    const bool useAimActive = IsServerUseControllerAimWindowActive();
    if (useAimActive)
    {
        static std::chrono::steady_clock::time_point s_lastFindUseEntryLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastFindUseEntryLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastFindUseEntryLog).count() >= 0.50f)
        {
            s_lastFindUseEntryLog = now;
            const int playerIndex = m_Game ? m_Game->m_CurrentUsercmdID : -1;
            const bool validPlayerIndex = m_Game && m_Game->IsValidPlayerIndex(playerIndex);
            const bool isUsingVR = validPlayerIndex && m_Game->m_PlayersVRInfo[playerIndex].isUsingVR;
            Game::logMsg(
                "[VR][UseAim] FindUseEntity entry usercmd=%d isVR=%d hasAim=%d player=%p current=%p",
                playerIndex,
                isUsingVR ? 1 : 0,
                (m_VR && m_VR->m_HasNonVRAimSolution) ? 1 : 0,
                ecx,
                m_Game ? reinterpret_cast<void*>(m_Game->m_CurrentUsercmdPlayer) : nullptr);
        }
    }

    Vector controllerOrigin;
    QAngle controllerAngles;
    if (useAimActive && TryBuildServerUseControllerPose(ecx, controllerOrigin, controllerAngles))
    {
        static std::chrono::steady_clock::time_point s_lastFindUseLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastFindUseLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastFindUseLog).count() >= 0.50f)
        {
            s_lastFindUseLog = now;
            Game::logMsg(
                "[VR][UseAim] FindUseEntity override origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f)",
                controllerOrigin.x, controllerOrigin.y, controllerOrigin.z,
                controllerAngles.x, controllerAngles.y, controllerAngles.z);
        }
        ScopedServerUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
        return hkFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
    }

    return hkFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
}

static C_BaseEntity* ObjectPullGetClientNativeHighlightTarget(
    Game* game,
    VR* vr)
{
    if (!game ||
        !vr ||
        !game->m_ClientEntityList)
    {
        return nullptr;
    }

    int pullTargetIndex = 0;
    std::uintptr_t expectedEntityAddress = 0;
    std::uintptr_t expectedVtableAddress = 0;
    if (!vr->GetObjectPullNativeHighlightIdentity(
            pullTargetIndex,
            expectedEntityAddress,
            expectedVtableAddress) ||
        pullTargetIndex <= 0 ||
        pullTargetIndex > 2047)
    {
        return nullptr;
    }

#ifdef _MSC_VER
    __try
    {
        C_BaseEntity* current = static_cast<C_BaseEntity*>(
            game->m_ClientEntityList->GetClientEntity(
                pullTargetIndex));
        if (current &&
            reinterpret_cast<std::uintptr_t>(current) ==
                expectedEntityAddress &&
            reinterpret_cast<std::uintptr_t>(
                *reinterpret_cast<void**>(current)) ==
                expectedVtableAddress)
        {
            return current;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
#else
    C_BaseEntity* current = static_cast<C_BaseEntity*>(
        game->m_ClientEntityList->GetClientEntity(
            pullTargetIndex));
    if (current &&
        reinterpret_cast<std::uintptr_t>(current) ==
            expectedEntityAddress &&
        reinterpret_cast<std::uintptr_t>(
            *reinterpret_cast<void**>(current)) ==
            expectedVtableAddress)
    {
        return current;
    }
#endif
    return nullptr;
}

C_BaseEntity* Hooks::dClientFindUseEntity(void* ecx, void* edx, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra)
{
    if (C_BaseEntity* pullTarget =
        ObjectPullGetClientNativeHighlightTarget(m_Game, m_VR))
    {
        // This is the same target feed used by L4D2's native item-outline
        // system. Returning the exact selected entity gives Object Pull the
        // original game outline without drawing a second ray/marker effect.
        return pullTarget;
    }

    Vector controllerOrigin;
    QAngle controllerAngles;
    if (TryBuildClientUseControllerPose(ecx, controllerOrigin, controllerAngles))
    {
        static std::chrono::steady_clock::time_point s_lastClientFindUseLog{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastClientFindUseLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastClientFindUseLog).count() >= 1.00f)
        {
            s_lastClientFindUseLog = now;
            Game::logMsg(
                "[VR][UseAim] ClientFindUseEntity highlight override origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f)",
                controllerOrigin.x, controllerOrigin.y, controllerOrigin.z,
                controllerAngles.x, controllerAngles.y, controllerAngles.z);
        }
        ScopedClientUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
        return hkClientFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
    }

    return hkClientFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
}

bool __fastcall Hooks::dWorldPoseWeaponSetupBones(
    void* ecx,
    void* edx,
    matrix3x4_t* boneToWorldOut,
    int maxBones,
    int boneMask,
    float currentTime)
{
    const bool result =
        hkWorldPoseWeaponSetupBones.fOriginal(
            ecx,
            boneToWorldOut,
            maxBones,
            boneMask,
            currentTime);
    if (result && boneToWorldOut)
    {
        HooksWorldPoseApplyWeaponSetupBones(
            m_VR,
            m_Game,
            ecx,
            boneToWorldOut,
            maxBones,
            boneMask,
            currentTime);
    }
    return result;
}

void Hooks::dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld)
{
    vr_vm_stabilize::ScopedStableBoneScratch stableBoneScratchScope;
    constexpr int kStudioShadowDepthTexture = 0x40000000;
    const bool shadowDepthDraw =
        (info.flags & kStudioShadowDepthTexture) != 0;

    HooksMaybePublishAndLogPlayerModelMaterials(
        m_VR,
        m_Game,
        state,
        info,
        shadowDepthDraw);

    std::vector<IMaterial*> hiddenWorldModelMaterials;
    const bool hasHiddenWorldModelMaterials =
        !shadowDepthDraw &&
        HooksFirstPersonBodyCollectHiddenMaterials(
            m_VR,
            m_Game,
            state,
            info.pModel,
            hiddenWorldModelMaterials);

    CMatRenderContextPtr worldModelMaterialContext;
    ICallQueue* worldModelMaterialCallQueue = nullptr;
    const int worldModelMaterialQueueMode =
        (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (hasHiddenWorldModelMaterials &&
        worldModelMaterialQueueMode != 0 &&
        m_Game &&
        m_Game->m_MaterialSystem)
    {
        worldModelMaterialContext =
            m_Game->m_MaterialSystem->GetRenderContext();
        worldModelMaterialCallQueue =
            HooksNativeViewmodelHandsOnlyGetSourceRenderCallQueue(
                worldModelMaterialContext);
    }

    HooksFirstPersonBodyMaterialHideLease worldModelMaterialHideLease(
        hiddenWorldModelMaterials);
    HooksWorldModelClothingQueuedReleaseGuard
        worldModelMaterialQueuedReleaseGuard(
            worldModelMaterialHideLease,
            worldModelMaterialQueueMode,
            worldModelMaterialCallQueue);

    HooksFirstPersonBodyEyeSceneState* const firstPersonBodyState =
        g_FirstPersonBodyPublishedState.load(std::memory_order_acquire);
    const bool firstPersonBodyEyeActive =
        firstPersonBodyState != nullptr &&
        firstPersonBodyState->bodyActive &&
        firstPersonBodyState->playerGeneration ==
        g_FirstPersonBodyPlayerGeneration.load(std::memory_order_acquire) &&
        g_FirstPersonBodyPlayerReady.load(std::memory_order_acquire) &&
        g_FirstPersonBodyActualFirstPerson.load(std::memory_order_acquire) &&
        m_VR &&
        !m_VR->m_IsThirdPersonCamera &&
        InterlockedCompareExchange(
            &g_FirstPersonBodyEyeSceneActive, 0, 0) != 0;

    if (firstPersonBodyEyeActive &&
        m_VR &&
        m_Game &&
        m_VR->m_FirstPersonBodyEnabled)
    {
        if (m_VR->m_FirstPersonBodyHideWorldWeapon &&
            (info.flags & kStudioShadowDepthTexture) == 0 &&
            firstPersonBodyState->activeWeaponRenderable &&
            info.pRenderable == firstPersonBodyState->activeWeaponRenderable)
        {
            return;
        }

        const bool isLocalPlayerDraw =
            firstPersonBodyState->localPlayerRenderable &&
            info.pRenderable == firstPersonBodyState->localPlayerRenderable;
        if (isLocalPlayerDraw)
        {
            // Preserve Source's original shadow submission. HMD anchoring and bone
            // masking apply only to the per-eye color draw.
            if ((info.flags & kStudioShadowDepthTexture) != 0)
            {
                hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
                return;
            }

            HooksFirstPersonBodyBoneBufferLease bodyBufferLease;
            HooksFirstPersonBodyBoneBuffer* const bodyBuffer =
                bodyBufferLease.Get();
            if (!bodyBuffer)
            {
                static std::atomic<bool> s_loggedBodyBufferCap{ false };
                if (!s_loggedBodyBufferCap.exchange(
                    true, std::memory_order_acq_rel))
                {
                    Game::logMsg(
                        "[VR][FirstPersonBody] body skipped: more than %u nested bone draws",
                        static_cast<unsigned int>(
                            HooksFirstPersonBodyBoneBufferPool::kMaxBuffers));
                }
                return;
            }

            vr_vm_stabilize::Mat3x4* anchoredBodyBones = nullptr;
            if (!HooksFirstPersonBodyBuildAnchoredBones(
                m_VR,
                firstPersonBodyState,
                state,
                info,
                pCustomBoneToWorld,
                bodyBuffer->bones,
                anchoredBodyBones))
            {
                static std::atomic<bool> s_loggedBodyBoneFailure{ false };
                if (!s_loggedBodyBoneFailure.exchange(true, std::memory_order_acq_rel))
                {
                    Game::logMsg(
                        "[VR][FirstPersonBody] failed to build HMD-anchored masked body bones entity=%d state=%p bones=%p",
                        info.entity_index,
                        state,
                        pCustomBoneToWorld);
                }
                return;
            }

            // Anchored body bones are intentionally built even when the torso is
            // hidden (for example while incapacitated). The build above publishes
            // the left/right body shoulder anchors consumed by full-arm viewmodel
            // IK. Hiding the body must therefore happen only after that publication.
            if (!firstPersonBodyState->renderBody)
            {
                static std::atomic<bool> s_loggedBodyHiddenButAnchorsAlive{ false };
                if (!s_loggedBodyHiddenButAnchorsAlive.exchange(
                    true, std::memory_order_acq_rel))
                {
                    Game::logMsg(
                        "[VR][FirstPersonBody] body color hidden while shoulder-anchor skeleton remains active entity=%d",
                        info.entity_index);
                }
                return;
            }

            static std::atomic<bool> s_loggedBodyDrawReached{ false };
            if (!s_loggedBodyDrawReached.exchange(true, std::memory_order_acq_rel))
            {
                Game::logMsg(
                    "[VR][FirstPersonBody] HMD-anchored masked local body draw reached entity=%d bones=%p->%p",
                    info.entity_index,
                    pCustomBoneToWorld,
                    anchoredBodyBones);
            }

            hkDrawModelExecute.fOriginal(
                ecx,
                state,
                info,
                anchoredBodyBones);
            return;
        }
    }

    if (m_Game->m_SwitchedWeapons)
        m_Game->m_CachedArmsModel = false;

    const bool nativeViewmodelHandsOnlyActive =
        HooksNativeViewmodelHandsOnlyActive(m_VR);
    const bool emptyHandsPlaceholderActiveAtEntry =
        m_VR &&
        m_VR->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
    bool hideArms =
        !emptyHandsPlaceholderActiveAtEntry &&
        (m_VR->m_HideArms ||
            (m_Game->m_IsMeleeWeaponActive && !nativeViewmodelHandsOnlyActive));

    void* pBonesToWorldFinal = pCustomBoneToWorld;
    vr_vm_stabilize::Mat3x4* magazineInteractionDetachedMagazineBones = nullptr;
    bool drawMagazineInteractionDetachedMagazine = false;
    vr_vm_stabilize::Mat3x4* magazineInteractionCalibrationPreviewBones = nullptr;
    vr_vm_stabilize::Mat3x4* magazineInteractionCalibrationPreviewModelToWorld = nullptr;
    bool drawMagazineInteractionCalibrationPreview = false;
    Vector magazineInteractionCalibrationPreviewOrigin(0.0f, 0.0f, 0.0f);
    QAngle magazineInteractionCalibrationPreviewAngles(0.0f, 0.0f, 0.0f);
    bool magazineInteractionCalibrationPreviewDrawnAsPrimary = false;
    bool drawEntityIsViewmodelClass = false;
    bool drawUsesNativeViewmodelDepthHack = false;
    bool viewmodelAutoGripApplied = false;

    // Per-draw origin/angles override (used for multicore viewmodel stabilization).
    // We never write into shared entity state here; we only override the ModelRenderInfo_t
    // passed down to the renderer for this draw call (frame-stable, avoids queued-thread tearing).
    ModelRenderInfo_t drawInfo = info;
    const ModelRenderInfo_t* pDrawInfo = &info;

    std::string modelName;
    if (info.pModel)
    {
        modelName = m_Game->m_ModelInfo->GetModelName(info.pModel);
        // In desktop-mirror overlay hide mode, special-infected arrows are cached
        // from the RenderView hook at a fixed stereo-pass point. Never scan from
        // DrawModelExecute in that mode: under mat_queue_mode 2 this hook can run
        // once per model and would multiply client-entity-list scan requests.
        const bool desktopMirrorOverlayHideActiveEarly =
            m_VR->m_DesktopMirrorHidePluginOverlays && m_VR->m_DesktopMirrorEnabled;
        if (!desktopMirrorOverlayHideActiveEarly)
            m_VR->ScanSpecialInfectedEntitiesFromClientList();

        const C_BaseEntity* entity = nullptr;
        if (m_Game->m_ClientEntityList && info.entity_index > 0 && info.entity_index <= 2048)
        {
            entity = HooksSafeGetClientEntity(m_Game, info.entity_index);
        }
        bool isPlayerClass = false;
        const char* className = nullptr;
        if (entity)
        {
            className = HooksSafeGetNetworkClassName(m_Game, const_cast<C_BaseEntity*>(entity));
            isPlayerClass = className && (std::strcmp(className, "CTerrorPlayer") == 0 || std::strcmp(className, "C_TerrorPlayer") == 0);
        }
        const bool isSurvivorWorldModel =
            entity &&
            info.entity_index > 0 &&
            info.entity_index <= 64 &&
            modelName.find("models/survivors/") != std::string::npos;
        const bool isPlayerWorldModel =
            isPlayerClass || isSurvivorWorldModel;
        if (m_VR->m_WorldModelVRPoseDebugLog &&
            m_Game->m_EngineClient &&
            info.entity_index == m_Game->m_EngineClient->GetLocalPlayer() &&
            isSurvivorWorldModel)
        {
            static thread_local std::uint64_t s_lastLocalWorldPoseStageLog = 0u;
            const std::uint64_t now =
                static_cast<std::uint64_t>(GetTickCount64());
            if (now - s_lastLocalWorldPoseStageLog >= 1000u)
            {
                s_lastLocalWorldPoseStageLog = now;
                Game::logMsg(
                    "[VR][WorldPose] local draw stage class=%s model=%s thirdPerson=%d firstPersonBodyEye=%d bones=%p shadow=%d",
                    className ? className : "<null>",
                    modelName.c_str(),
                    m_VR->m_IsThirdPersonCamera ? 1 : 0,
                    firstPersonBodyEyeActive ? 1 : 0,
                    pBonesToWorldFinal,
                    shadowDepthDraw ? 1 : 0);
            }
        }
        const bool isViewmodelClassForProbe = className &&
            (std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0);
        drawEntityIsViewmodelClass = isViewmodelClassForProbe;
        if (m_VR->m_VrHandsDebugLog && (isViewmodelClassForProbe || HooksModelNameIsViewmodel(modelName)))
        {
            MaybeLogVrHandsViewmodelBoneProbe(
                state,
                modelName,
                info.entity_index,
                className,
                pCustomBoneToWorld,
                pCustomBoneToWorld != nullptr);
        }
        // A server SetOrigin teleport can leave one queued first-person viewmodel draw
        // produced against the pre-teleport anchor. Drop that short transition window
        // instead of rendering a weapon model that flashes once and disappears.
        const bool teleportSuppressibleViewmodel =
            (className && (std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0)) ||
            (modelName.find("models/weapons/v_") != std::string::npos) ||
            (modelName.find("/v_models/") != std::string::npos) ||
            (modelName.find("models/v_models/") != std::string::npos) ||
            (modelName.find("models/weapons/melee/v_") != std::string::npos) ||
            (modelName.find("/melee/v_") != std::string::npos) ||
            (modelName.find("models/weapons/arms/") != std::string::npos) ||
            (modelName.find("/arms/") != std::string::npos) ||
            (modelName.find("v_arms") != std::string::npos) ||
            (modelName.find("models/weapons/hands/") != std::string::npos) ||
            (modelName.find("/hands/") != std::string::npos) ||
            (modelName.find("v_hands") != std::string::npos);
        drawUsesNativeViewmodelDepthHack = teleportSuppressibleViewmodel;
        const std::string lowerWorldPoseModelName =
            vr_vm_stabilize::ToLowerAscii(modelName);
        const bool looksLikeHeldWorldModel =
            lowerWorldPoseModelName.find("/w_models/") !=
            std::string::npos ||
            lowerWorldPoseModelName.find("models/weapons/w_") !=
            std::string::npos ||
            lowerWorldPoseModelName.find("/weapons/w_") !=
            std::string::npos ||
            lowerWorldPoseModelName.find("/melee/w_") !=
            std::string::npos;
        if (emptyHandsPlaceholderActiveAtEntry &&
            m_VR->m_IsThirdPersonCamera &&
            looksLikeHeldWorldModel &&
            m_Game &&
            m_Game->m_EngineClient &&
            m_Game->m_ClientEntityList)
        {
            const int localPlayerIndex =
                m_Game->m_EngineClient->GetLocalPlayer();
            C_BaseEntity* const localPlayerEntity =
                (localPlayerIndex > 0)
                ? HooksSafeGetClientEntity(
                    m_Game,
                    localPlayerIndex)
                : nullptr;
            C_BaseCombatWeapon* activeWeapon = nullptr;
            void* activeWeaponRenderable = nullptr;
            if (localPlayerEntity &&
                HooksWorldPoseGetActiveWeaponSafe(
                    localPlayerEntity,
                    activeWeapon,
                    activeWeaponRenderable) &&
                activeWeaponRenderable &&
                (info.pRenderable == activeWeaponRenderable ||
                    entity == activeWeapon))
            {
                // The server-side dummy pistol keeps native shove/use behavior
                // after the real inventory is empty. Hide only that local active
                // renderable; dropped weapons and other players remain visible.
                return;
            }
        }
        if (teleportSuppressibleViewmodel && m_VR->ShouldSuppressTeleportViewmodelRender())
            return;
        if (teleportSuppressibleViewmodel)
            m_VR->DrawVrHandsWorldDepthMaskBeforeViewmodel();
        const bool emptyHandsPlaceholderActive =
            m_VR->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
        if (emptyHandsPlaceholderActive &&
            teleportSuppressibleViewmodel &&
            !HooksModelNameIsArmsOrHands(vr_vm_stabilize::ToLowerAscii(modelName)))
        {
            // The native placeholder pistol supplies stock shove behavior, but its
            // weapon model is not visible. Keep the separate survivor arms model;
            // the forced NativeViewmodelHandsOnly path below trims it to both hands.
            return;
        }

        const bool suppressDesktopMirrorPluginOverlays =
            m_VR->m_DesktopMirrorCleanRenderingPass && m_VR->m_DesktopMirrorHidePluginOverlays;
        const bool desktopMirrorOverlayHideActive =
            m_VR->m_DesktopMirrorHidePluginOverlays && m_VR->m_DesktopMirrorEnabled;
        const bool singlePassDesktopMirrorPluginOverlays = false;
        const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
        if (queueMode == 0 &&
            !suppressDesktopMirrorPluginOverlays &&
            (info.entity_index == -1 || (info.entity_index > 0 && info.entity_index <= 2048)))
        {
            m_VR->DrawItemModelLabel(info.entity_index, modelName, info.origin, entity, className);
        }
        // Scope RTT pass: optionally hide the local player model so scoped view isn't blocked by your own head/body.
        if (m_VR->m_ScopeRenderingPass && m_VR->m_ScopeHideLocalPlayerModelInScope && isPlayerClass && m_Game->m_EngineClient)
        {
            const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
            if (info.entity_index == localPlayerIndex)
                return;
        }

        // Reconstruct the observer-visible VR pose only in the render copy of
        // CTerrorPlayer's final matrices.  The local player goes through this
        // same path in a third-person scene, which makes protocol/IK testing
        // possible without a second client.
        if (isPlayerWorldModel &&
            !shadowDepthDraw &&
            pBonesToWorldFinal &&
            entity)
        {
            vr_vm_stabilize::Mat3x4* worldPoseBones = nullptr;
            if (HooksWorldPoseBuildPlayerBones(
                m_VR,
                m_Game,
                state,
                info,
                entity,
                firstPersonBodyEyeActive,
                pBonesToWorldFinal,
                worldPoseBones))
            {
                pBonesToWorldFinal = worldPoseBones;
            }
        }

        // Active world weapons are rendered separately from their owning player
        // and otherwise remain bonemerged to Source's unmodified hand.  Move the
        // weapon's render-copy bones by the exact native-hand -> VR-hand delta.
        // This keeps the stock hand-to-gun offset and internal firing/reload
        // animation while cancelling the large native idle attachment motion.
        // Do not gate this on the reported network class or a custom-bone
        // pointer: L4D2 bonemerge children can report the owning player's class
        // and can deliberately submit a null custom-bone array.
        if (!isSurvivorWorldModel &&
            !shadowDepthDraw &&
            !teleportSuppressibleViewmodel)
        {
            vr_vm_stabilize::Mat3x4* alignedWorldWeaponBones = nullptr;
            vr_vm_stabilize::Mat3x4 weaponNativeToFinal =
                vr_vm_stabilize::Identity();
            int weaponOwnerPlayerIndex = -1;
            std::uint64_t weaponHandAgeMs = 0u;
            Vector weaponHandDisplacement(0.0f, 0.0f, 0.0f);
            int weaponMatchMask = 0;
            if (HooksWorldPoseBuildActiveWeaponBones(
                m_VR,
                m_Game,
                state,
                info,
                entity,
                looksLikeHeldWorldModel,
                pBonesToWorldFinal,
                alignedWorldWeaponBones,
                weaponNativeToFinal,
                weaponOwnerPlayerIndex,
                weaponHandAgeMs,
                weaponHandDisplacement,
                weaponMatchMask))
            {
                bool weaponBoneTransformApplied = false;
                bool weaponModelTransformApplied = false;
                if (alignedWorldWeaponBones)
                {
                    pBonesToWorldFinal =
                        alignedWorldWeaponBones;
                    weaponBoneTransformApplied = true;
                }
                else if (!pCustomBoneToWorld)
                {
                    vr_vm_stabilize::Mat3x4 sourceModelWorld{};
                    bool sourceModelWorldValid = false;
                    if (info.pModelToWorld)
                    {
                        sourceModelWorldValid =
                            vr_vm_stabilize::SafeRead(
                                reinterpret_cast<
                                const vr_vm_stabilize::Mat3x4*>(
                                    info.pModelToWorld),
                                sourceModelWorld) &&
                            HooksNativeViewmodelHandsOnlyMatrixFinite(
                                sourceModelWorld);
                    }
                    if (!sourceModelWorldValid)
                    {
                        vr_vm_stabilize::BuildFromOrgAngles(
                            info.origin,
                            info.angles,
                            sourceModelWorld);
                        sourceModelWorldValid =
                            HooksNativeViewmodelHandsOnlyMatrixFinite(
                                sourceModelWorld);
                    }

                    vr_vm_stabilize::Mat3x4 targetModelWorld{};
                    QAngle targetModelAngles{};
                    if (sourceModelWorldValid)
                    {
                        vr_vm_stabilize::Mul(
                            weaponNativeToFinal,
                            sourceModelWorld,
                            targetModelWorld);
                    }
                    if (sourceModelWorldValid &&
                        HooksNativeViewmodelHandsOnlyMatrixFinite(
                            targetModelWorld) &&
                        HooksViewmodelAutoGripMatrixAngles(
                            targetModelWorld,
                            targetModelAngles))
                    {
                        std::uint32_t sequence =
                            m_VR->m_RenderFrameSeq.load(
                                std::memory_order_acquire) &
                            ~1u;
                        if (sequence == 0u)
                            sequence = 2u;
                        vr_vm_stabilize::Mat3x4*
                            stableModelToWorld =
                            vr_vm_stabilize::AllocStableBones(
                                1,
                                sequence);
                        if (stableModelToWorld)
                        {
                            *stableModelToWorld =
                                targetModelWorld;
                            drawInfo = info;
                            drawInfo.origin =
                                vr_vm_stabilize::GetOrigin(
                                    targetModelWorld);
                            drawInfo.angles =
                                targetModelAngles;
                            drawInfo.pModelToWorld =
                                reinterpret_cast<
                                const matrix3x4_t*>(
                                    stableModelToWorld);
                            pDrawInfo = &drawInfo;
                            weaponModelTransformApplied = true;
                        }
                    }
                }

                if (m_VR->m_WorldModelVRPoseDebugLog)
                {
                    static thread_local std::array<
                        std::uint64_t,
                        Game::kMaxPlayers> s_lastWeaponApplyLog{};
                    const std::uint64_t now =
                        static_cast<std::uint64_t>(GetTickCount64());
                    std::uint64_t& last =
                        s_lastWeaponApplyLog[
                            static_cast<size_t>(
                                weaponOwnerPlayerIndex)];
                    if (now - last >= 1000u)
                    {
                        last = now;
                        Game::logMsg(
                            "[VR][WorldPoseWeapon] match player=%d entity=%d model=%s age=%llums match=0x%02X apply=%s handDelta=(%.1f %.1f %.1f) bones=%p->%p",
                            weaponOwnerPlayerIndex,
                            info.entity_index,
                            modelName.c_str(),
                            static_cast<unsigned long long>(
                                weaponHandAgeMs),
                            weaponMatchMask,
                            weaponBoneTransformApplied
                            ? "bones"
                            : (weaponModelTransformApplied
                                ? "model_transform"
                                : "none"),
                            weaponHandDisplacement.x,
                            weaponHandDisplacement.y,
                            weaponHandDisplacement.z,
                            pCustomBoneToWorld,
                            alignedWorldWeaponBones);
                    }
                }
            }
        }


        // --- Multicore viewmodel stabilization (first-person viewmodel ghosting fix) ---
        // In queued rendering (mat_queue_mode!=0), viewmodels are frequently submitted with custom bone matrices.
        // In that case, overriding ModelRenderInfo_t.origin/angles does NOT move the model (it stays "head-locked").
        // So we apply a rigid delta to the bone matrices for this draw call, based on our controller-anchored target.
        if (m_VR->m_IsVREnabled && queueMode == 2 &&
            (m_VR->m_QueuedViewmodelStabilize ||
                m_VR->m_ViewmodelDisableMoveBob ||
                (!m_VR->m_MouseModeEnabled &&
                    (m_VR->m_VrHandsRightUseViewmodelPose ||
                        emptyHandsPlaceholderActive ||
                        m_VR->IsVrHandsTwoHandedGripPoseActive()))))
        {
            const bool isViewmodelClass = className &&
                (std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0);
            const bool isArmsOrHandsModel = HooksModelNameIsArmsOrHands(modelName);
            const bool isViewmodelModel = HooksModelNameIsViewmodel(modelName);

            if (isViewmodelClass || isViewmodelModel)
            {
                struct RenderSnapshotTLSGuard
                {
                    bool prev = false;
                    RenderSnapshotTLSGuard()
                    {
                        prev = VR::t_UseRenderFrameSnapshot;
                        VR::t_UseRenderFrameSnapshot = true;
                    }
                    ~RenderSnapshotTLSGuard()
                    {
                        VR::t_UseRenderFrameSnapshot = prev;
                    }
                } tlsGuard;

                const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos();
                const QAngle targetAngles = m_VR->GetRecommendedViewmodelAbsAngle();

                // Always override origin/angles for lighting/etc (even if bones are used).
                drawInfo = info;
                drawInfo.origin = targetOrigin;
                drawInfo.angles = targetAngles;
                pDrawInfo = &drawInfo;

                bool appliedBoneDelta = false;
                int numBones = 0;

                if (pCustomBoneToWorld)
                {
                    if (vr_vm_stabilize::TryGetNumBonesFromDrawState(state, numBones) && numBones > 0)
                    {
                        uint32_t seqEven = m_VR->m_RenderFrameSeq.load(std::memory_order_acquire);
                        seqEven &= ~1u;
                        if (seqEven == 0)
                            seqEven = 2;

                        vr_vm_stabilize::Mat3x4* bonesCopy = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
                        if (bonesCopy)
                        {
                            memcpy(bonesCopy, pCustomBoneToWorld, (size_t)numBones * sizeof(vr_vm_stabilize::Mat3x4));

                            // NOTE:
                            // pCustomBoneToWorld is already in WORLD space. However, bone[0] is NOT guaranteed
                            // to be at the entity origin (studio root can have a built-in offset). Using bone[0]
                            // as the reference will mis-anchor the whole model (often looks like it's still HMD-bound).
                            //
                            // Correct approach: treat the bones as (EntityToWorld * BoneLocal). Recover BoneLocal
                            // via inverse(EntityToWorld), then re-apply with TargetEntityToWorld.
                            vr_vm_stabilize::Mat3x4 origEntity{};
                            vr_vm_stabilize::BuildFromOrgAngles(info.origin, info.angles, origEntity);
                            vr_vm_stabilize::Mat3x4 origInv{};
                            vr_vm_stabilize::InvertTR(origEntity, origInv);
                            vr_vm_stabilize::Mat3x4 targetEntity{};
                            vr_vm_stabilize::BuildFromOrgAngles(targetOrigin, targetAngles, targetEntity);
                            vr_vm_stabilize::Mat3x4 delta{};
                            vr_vm_stabilize::Mul(targetEntity, origInv, delta);

                            bool splitApplied = false;
                            if (m_VR->m_SplitArmsToControllers && isArmsOrHandsModel && numBones > 8 && !m_VR->m_MouseModeEnabled)
                            {
                                const Vector leftCtrlPos = m_VR->GetLeftControllerAbsPos();
                                const QAngle leftCtrlAng = m_VR->GetLeftControllerAbsAngle();

                                Vector leftForward{}, leftRight{}, leftUp{};
                                QAngle::AngleVectors(leftCtrlAng, &leftForward, &leftRight, &leftUp);

                                leftForward = VectorRotate(leftForward, leftRight, -45.0f);
                                leftUp = VectorRotate(leftUp, leftRight, -45.0f);

                                leftForward = VectorRotate(leftForward, leftUp, m_VR->m_ViewmodelAngOffset.y);
                                leftRight = VectorRotate(leftRight, leftUp, m_VR->m_ViewmodelAngOffset.y);
                                leftForward = VectorRotate(leftForward, leftRight, m_VR->m_ViewmodelAngOffset.x);
                                leftUp = VectorRotate(leftUp, leftRight, m_VR->m_ViewmodelAngOffset.x);
                                leftRight = VectorRotate(leftRight, leftForward, m_VR->m_ViewmodelAngOffset.z);
                                leftUp = VectorRotate(leftUp, leftForward, m_VR->m_ViewmodelAngOffset.z);

                                Vector leftVmPos = leftCtrlPos
                                    - (leftForward * m_VR->m_ViewmodelPosOffset.x)
                                    - (leftRight * m_VR->m_ViewmodelPosOffset.y)
                                    - (leftUp * m_VR->m_ViewmodelPosOffset.z);

                                QAngle leftVmAng{};
                                QAngle::VectorAngles(leftForward, leftUp, leftVmAng);

                                vr_vm_stabilize::Mat3x4 leftTargetEntity{};
                                vr_vm_stabilize::BuildFromOrgAngles(leftVmPos, leftVmAng, leftTargetEntity);

                                vr_vm_stabilize::Mat3x4 leftDelta{};
                                vr_vm_stabilize::Mul(leftTargetEntity, origInv, leftDelta);

                                std::vector<float> localY((size_t)numBones, 0.0f);
                                Vector posSum{ 0.0f, 0.0f, 0.0f };
                                Vector negSum{ 0.0f, 0.0f, 0.0f };
                                int posCount = 0;
                                int negCount = 0;
                                float minY = 1e9f;
                                float maxY = -1e9f;

                                const auto* srcBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
                                for (int i = 0; i < numBones; ++i)
                                {
                                    vr_vm_stabilize::Mat3x4 localBone{};
                                    vr_vm_stabilize::Mul(origInv, srcBones[i], localBone);
                                    const float y = localBone.m[1][3];
                                    localY[(size_t)i] = y;
                                    minY = std::min(minY, y);
                                    maxY = std::max(maxY, y);

                                    const Vector worldPos = vr_vm_stabilize::GetOrigin(srcBones[i]);
                                    if (y > 1.0f)
                                    {
                                        posSum += worldPos;
                                        ++posCount;
                                    }
                                    else if (y < -1.0f)
                                    {
                                        negSum += worldPos;
                                        ++negCount;
                                    }
                                }

                                if (posCount > 0 && negCount > 0 && (maxY - minY) > 4.0f)
                                {
                                    const Vector rightCtrlPos = m_VR->GetRightControllerAbsPos();
                                    const Vector posAvg = posSum / (float)posCount;
                                    const Vector negAvg = negSum / (float)negCount;
                                    const bool positiveYIsRight = (posAvg - rightCtrlPos).LengthSqr() <= (negAvg - rightCtrlPos).LengthSqr();
                                    const float deadZone = std::max(1.0f, (maxY - minY) * 0.08f);

                                    for (int i = 0; i < numBones; ++i)
                                    {
                                        const float y = localY[(size_t)i];
                                        const bool isCenter = std::fabs(y) <= deadZone;
                                        const bool useRightDelta = isCenter || ((y > 0.0f) == positiveYIsRight);
                                        vr_vm_stabilize::Mat3x4 tmp{};
                                        vr_vm_stabilize::Mul(useRightDelta ? delta : leftDelta, bonesCopy[i], tmp);
                                        bonesCopy[i] = tmp;
                                    }
                                    splitApplied = true;
                                }
                            }

                            if (!splitApplied)
                                vr_vm_stabilize::ApplyDelta(delta, bonesCopy, numBones);

                            pBonesToWorldFinal = bonesCopy;
                            appliedBoneDelta = true;
                        }
                    }
                }

                if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
                {
                    static thread_local std::chrono::steady_clock::time_point s_last{};
                    if (!ShouldThrottleLog(s_last, m_VR->m_QueuedViewmodelStabilizeDebugLogHz))
                    {
                        const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
                        const uint32_t tid = (uint32_t)GetCurrentThreadId();
                        Vector root0 = info.origin;
                        Vector root1 = targetOrigin;
                        if (pCustomBoneToWorld)
                        {
                            vr_vm_stabilize::Mat3x4 r0{};
                            if (vr_vm_stabilize::SafeRead(pCustomBoneToWorld, r0))
                                root0 = vr_vm_stabilize::GetOrigin(r0);
                        }
                        if (appliedBoneDelta && pBonesToWorldFinal)
                        {
                            root1 = vr_vm_stabilize::GetOrigin(reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pBonesToWorldFinal)[0]);
                        }

                        const Vector eyeO = m_VR->m_HmdPosAbs;
                        const Vector rcO = m_VR->GetRightControllerAbsPos();
                        const float dTgtRc = (targetOrigin - rcO).Length();
                        const Vector entDelta = targetOrigin - info.origin;
                        Vector bone0Off(0.0f, 0.0f, 0.0f);
                        if (pCustomBoneToWorld)
                        {
                            vr_vm_stabilize::Mat3x4 r0{};
                            if (vr_vm_stabilize::SafeRead(pCustomBoneToWorld, r0))
                                bone0Off = vr_vm_stabilize::GetOrigin(r0) - info.origin;
                        }

                        Game::logMsg(
                            "[VR][VM][draw] tid=%u qmode=%d seq=%u ent=%d model=\"%s\" customBones=%d bones=%d applied=%d slot=%u root0=(%.2f %.2f %.2f) root1=(%.2f %.2f %.2f) eyeO=(%.2f %.2f %.2f) rcO=(%.2f %.2f %.2f) dTgtRc=%.2f entD=(%.2f %.2f %.2f) bone0Off=(%.2f %.2f %.2f) origO=(%.2f %.2f %.2f) origA=(%.2f %.2f %.2f) tgtO=(%.2f %.2f %.2f) tgtA=(%.2f %.2f %.2f)"
                            ,
                            tid, queueMode, seq, info.entity_index, modelName.c_str(),
                            (pCustomBoneToWorld != nullptr) ? 1 : 0,
                            numBones,
                            appliedBoneDelta ? 1 : 0,
                            (uint32_t)((seq >> 1) % 64),
                            root0.x, root0.y, root0.z,
                            root1.x, root1.y, root1.z,
                            eyeO.x, eyeO.y, eyeO.z,
                            rcO.x, rcO.y, rcO.z,
                            dTgtRc,
                            entDelta.x, entDelta.y, entDelta.z,
                            bone0Off.x, bone0Off.y, bone0Off.z,
                            info.origin.x, info.origin.y, info.origin.z,
                            info.angles.x, info.angles.y, info.angles.z,
                            targetOrigin.x, targetOrigin.y, targetOrigin.z,
                            targetAngles.x, targetAngles.y, targetAngles.z);
                    }
                }
            }
        }

        viewmodelAutoGripApplied = ApplyViewmodelAutoGripAlignment(
            m_VR,
            state,
            modelName,
            drawEntityIsViewmodelClass,
            drawInfo,
            pDrawInfo,
            pBonesToWorldFinal);

        const VR::SpecialInfectedType entityInfectedType =
            entity ? m_VR->GetSpecialInfectedType(entity) : VR::SpecialInfectedType::None;
        const VR::SpecialInfectedType modelInfectedType = m_VR->GetSpecialInfectedTypeFromModel(modelName);
        const bool useWitchModelFallback =
            modelInfectedType == VR::SpecialInfectedType::Witch &&
            entityInfectedType == VR::SpecialInfectedType::None;

        if (!suppressDesktopMirrorPluginOverlays && !desktopMirrorOverlayHideActive && useWitchModelFallback)
        {
            if (m_VR->m_SpecialInfectedArrowDebugLog && m_VR->m_SpecialInfectedArrowDebugLogHz > 0.0f)
            {
                static std::unordered_map<int, std::chrono::steady_clock::time_point> s_lastWitchModelDebugLog;
                const int debugKey = info.entity_index > 0 ? info.entity_index : -1;
                bool doDebugLog = true;
                auto& last = s_lastWitchModelDebugLog[debugKey];
                const auto now = std::chrono::steady_clock::now();
                if (last.time_since_epoch().count() != 0)
                {
                    const float minInterval = 1.0f / std::max(1.0f, m_VR->m_SpecialInfectedArrowDebugLogHz);
                    const float elapsed = std::chrono::duration<float>(now - last).count();
                    if (elapsed >= 0.0f && elapsed < minInterval)
                        doDebugLog = false;
                }
                if (doDebugLog)
                {
                    last = now;
                    Game::logMsg(
                        "[VR][SIArrow][model] idx=%d class=%s model=\"%s\" type=%d origin=(%.1f %.1f %.1f)",
                        info.entity_index,
                        (className && *className) ? className : "<null>",
                        modelName.c_str(),
                        static_cast<int>(modelInfectedType),
                        info.origin.x,
                        info.origin.y,
                        info.origin.z);
                }
            }

            m_VR->RefreshSpecialInfectedPreWarning(info.origin, modelInfectedType, info.entity_index, false);

            bool doOverlay = true;
            if (!singlePassDesktopMirrorPluginOverlays && info.entity_index > 0 && m_VR->m_SpecialInfectedOverlayMaxHz > 0.0f)
            {
                auto& last = m_VR->m_LastSpecialInfectedOverlayTime[info.entity_index];
                const auto now = std::chrono::steady_clock::now();
                if (last.time_since_epoch().count() != 0)
                {
                    const float minInterval = 1.0f / std::max(1.0f, m_VR->m_SpecialInfectedOverlayMaxHz);
                    const float elapsed = std::chrono::duration<float>(now - last).count();
                    if (elapsed >= 0.0f && elapsed < minInterval)
                        doOverlay = false;
                }
                if (doOverlay)
                    last = now;
            }

            if (doOverlay)
            {
                if (m_VR->m_RearMirrorEnabled && m_VR->m_RearMirrorShowOnlyOnSpecialWarning
                    && m_VR->m_RearMirrorSpecialShowHoldSeconds > 0.0f && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
                {
                    Vector to = info.origin - m_VR->m_HmdPosAbs;
                    to.z = 0.0f;
                    const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
                    if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
                    {
                        Vector fwd = m_VR->m_HmdForward;
                        fwd.z = 0.0f;
                        if (VectorNormalize(fwd) == 0.0f)
                            fwd = { 1.0f, 0.0f, 0.0f };
                        VectorNormalize(to);
                        if (DotProduct(to, fwd) < 0.0f)
                            m_VR->NotifyRearMirrorSpecialWarning();
                        m_VR->m_RearMirrorSawSpecialThisPass = true;
                    }
                }

                m_VR->DrawSpecialInfectedArrow(info.origin, modelInfectedType);
            }
        }

        if (!suppressDesktopMirrorPluginOverlays && !desktopMirrorOverlayHideActive && entity && entityInfectedType != VR::SpecialInfectedType::None)
        {
            if (m_VR->IsEntityAlive(entity))
            {
                // 1) 高优先级：自瞄/目标刷新不要被 Overlay 节流影响（否则锁定会飘）
                // RefreshSpecialInfectedPreWarning 内部会用到 Trace 缓存（TraceMaxHz），所以这里高频调用不会把 CPU 打爆。
                m_VR->RefreshSpecialInfectedPreWarning(info.origin, entityInfectedType, info.entity_index, isPlayerClass);

                // Rear mirror pop-up: if enabled, show the mirror briefly when a special infected is behind you
                // within the configured warning distance. This detection runs on the main render pass so the
                // mirror can wake up without relying on the mirror RTT pass.
                if (m_VR->m_RearMirrorEnabled && m_VR->m_RearMirrorShowOnlyOnSpecialWarning
                    && m_VR->m_RearMirrorSpecialShowHoldSeconds > 0.0f && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
                {
                    Vector to = info.origin - m_VR->m_HmdPosAbs;
                    to.z = 0.0f;
                    const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
                    if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
                    {
                        Vector fwd = m_VR->m_HmdForward;
                        fwd.z = 0.0f;
                        if (VectorNormalize(fwd) == 0.0f)
                            fwd = { 1.0f, 0.0f, 0.0f };
                        VectorNormalize(to);
                        // Behind = more likely you want the rear mirror.
                        if (DotProduct(to, fwd) < 0.0f)
                            m_VR->NotifyRearMirrorSpecialWarning();
                    }
                }

                // 2) 低优先级：视觉 Overlay（箭头/盲区提示）继续按实体节流，避免 dDrawModelExecute 多次调用导致尖峰
                bool doOverlay = true;
                if (!singlePassDesktopMirrorPluginOverlays && info.entity_index > 0 && m_VR->m_SpecialInfectedOverlayMaxHz > 0.0f)
                {
                    auto& last = m_VR->m_LastSpecialInfectedOverlayTime[info.entity_index];
                    const auto now = std::chrono::steady_clock::now();
                    if (last.time_since_epoch().count() != 0)
                    {
                        const float minInterval = 1.0f / std::max(1.0f, m_VR->m_SpecialInfectedOverlayMaxHz);
                        const float elapsed = std::chrono::duration<float>(now - last).count();
                        if (elapsed < minInterval)
                            doOverlay = false;
                    }
                    if (doOverlay)
                        last = now;
                }

                if (doOverlay)
                {
                    // Rear-mirror hint: if this special-infected arrow is being rendered during the rear-mirror RTT pass
                    // and within the configured distance, enlarge the mirror overlay.
                    if (m_VR->m_RearMirrorRenderingPass && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
                    {
                        Vector to = info.origin - m_VR->m_HmdPosAbs;
                        to.z = 0.0f;
                        const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
                        if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
                            m_VR->m_RearMirrorSawSpecialThisPass = true;
                    }
                    if (entityInfectedType != VR::SpecialInfectedType::Tank
                        && entityInfectedType != VR::SpecialInfectedType::Witch
                        && entityInfectedType != VR::SpecialInfectedType::Charger)
                    {
                        m_VR->RefreshSpecialInfectedBlindSpotWarning(info.origin);
                    }
                    m_VR->DrawSpecialInfectedArrow(info.origin, entityInfectedType);
                }
            }
        }

        {
            MagazineInteractionRenderSnapshotScope detachedMagazineSnapshot(queueMode != 0);
            drawMagazineInteractionDetachedMagazine = BuildMagazineInteractionDetachedMagazineBones(
                m_VR,
                state,
                modelName,
                pBonesToWorldFinal,
                magazineInteractionDetachedMagazineBones);
        }

        ApplyMagazineInteractionViewmodelOverride(
            m_VR,
            state,
            modelName,
            *pDrawInfo,
            pBonesToWorldFinal);

        ApplyManualThrowViewmodelAnimationFreeze(
            m_VR,
            state,
            modelName,
            drawEntityIsViewmodelClass,
            *pDrawInfo,
            pBonesToWorldFinal);

        PublishMagazineInteractionCalibrationSnapshotFromDraw(
            m_VR,
            state,
            modelName,
            className,
            drawEntityIsViewmodelClass,
            info.entity_index,
            pBonesToWorldFinal);

        {
            MagazineInteractionRenderSnapshotScope calibrationPreviewSnapshot(queueMode != 0);
            drawMagazineInteractionCalibrationPreview = BuildMagazineInteractionCalibrationPreviewBones(
                m_VR,
                state,
                modelName,
                *pDrawInfo,
                drawEntityIsViewmodelClass,
                info.entity_index,
                info.hitboxset,
                pBonesToWorldFinal,
                magazineInteractionCalibrationPreviewBones,
                magazineInteractionCalibrationPreviewModelToWorld,
                magazineInteractionCalibrationPreviewOrigin,
                magazineInteractionCalibrationPreviewAngles);
        }

        MaybeDrawViewmodelBoneLabels(
            m_VR,
            state,
            modelName,
            className,
            pBonesToWorldFinal);

        const bool magazineInteractionCalibrationOverlayActive =
            m_VR &&
            m_VR->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed);
        if (!m_VR ||
            (!magazineInteractionCalibrationOverlayActive &&
                !m_VR->ShouldHideMagazineInteractionNativeClip()))
        {
            const bool isViewmodelModel = HooksModelNameIsViewmodel(modelName);
            if (isViewmodelModel)
                DrawCurrentWeaponMagazineBox(
                    m_VR,
                    state,
                    modelName,
                    drawEntityIsViewmodelClass,
                    info.entity_index,
                    info.hitboxset,
                    info.pModelToWorld,
                    pBonesToWorldFinal);
        }
    }

    // Capture the exact arm matrices submitted to Source. In queued rendering
    // pBonesToWorldFinal contains the same stabilization delta as the visible gun,
    // so the standalone right glove follows controller rotation and HMD movement.
    MaybeCaptureViewmodelMuzzleSmokePose(m_VR, state, modelName, *pDrawInfo, pBonesToWorldFinal);
    MaybeCaptureVrHandsVmPose(
        m_VR,
        state,
        modelName,
        *pDrawInfo,
        pBonesToWorldFinal,
        viewmodelAutoGripApplied);
    const std::string lowerModelForCalibrationHide = vr_vm_stabilize::ToLowerAscii(modelName);
    const bool nativeViewmodelArmsOrHandsModel =
        HooksModelNameIsArmsOrHands(lowerModelForCalibrationHide);
    const bool nativeViewmodelPendingFreezeHide =
        nativeViewmodelArmsOrHandsModel &&
        HooksNativeViewmodelHandsOnlyShouldHidePendingFreeze(m_VR);
    // Full-arm IK is an all-or-nothing render product. If controller tracking,
    // either arm chain, either target, or either analytic solve is unavailable,
    // keep the original unanchored viewmodel branches hidden instead of falling
    // back to a single floating native arm.
    const bool nativeViewmodelFullArmIkFailClosed =
        nativeViewmodelArmsOrHandsModel &&
        HooksNativeViewmodelFullArmIkActive(m_VR);
    const bool hideMagazineInteractionCalibrationOriginalViewmodel =
        m_VR &&
        m_VR->m_MagazineInteractionCalibrationOverlayActive.load(std::memory_order_relaxed) &&
        (drawEntityIsViewmodelClass || HooksModelNameIsViewmodel(lowerModelForCalibrationHide)) &&
        !nativeViewmodelArmsOrHandsModel;

    const bool protectNativeViewmodelNearClip =
        drawUsesNativeViewmodelDepthHack &&
        !shadowDepthDraw &&
        m_VR &&
        m_VR->m_IsVREnabled &&
        m_VR->m_VRNearClipSelfBody;
    const int viewmodelNearClipQueueMode =
        (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    CRefPtr<IMatRenderContext> viewmodelDepthContext;
    ICallQueue* viewmodelNearClipCallQueue = nullptr;
    if (drawUsesNativeViewmodelDepthHack &&
        !shadowDepthDraw &&
        m_VR &&
        m_VR->m_IsVREnabled &&
        m_Game &&
        m_Game->m_MaterialSystem)
    {
        viewmodelDepthContext =
            m_Game->m_MaterialSystem->GetRenderContext();
        if (viewmodelDepthContext)
        {
            // Source brackets the viewmodel list with DepthRange(0, 0.1) to
            // force it over the world. Appending 0..1 immediately before each
            // actual model submission preserves command order in queued mode;
            // Source already restores 0..1 after the list completes.
            viewmodelDepthContext->DepthRange(0.0f, 1.0f);

            if (protectNativeViewmodelNearClip && viewmodelNearClipQueueMode != 0)
            {
                viewmodelNearClipCallQueue =
                    HooksNativeViewmodelHandsOnlyGetSourceRenderCallQueue(
                        viewmodelDepthContext);
            }
        }
    }

    // Keep the scene projection and depth mapping intact. D3DRS_CLIPPING=FALSE
    // selects Vulkan depth clamping only around this native viewmodel's queued
    // draw submissions; world geometry therefore still occludes it correctly.
    ScopedNativeViewmodelDepthClamp viewmodelDepthClamp(
        protectNativeViewmodelNearClip,
        m_VR,
        viewmodelNearClipQueueMode,
        viewmodelNearClipCallQueue);

    if (info.pModel && hideArms && !m_Game->m_CachedArmsModel)
    {
        if (modelName.find("/arms/") != std::string::npos)
        {
            m_Game->m_ArmsMaterial = m_Game->m_MaterialSystem->FindMaterial(modelName.c_str(), "Model textures");
            m_Game->m_ArmsModel = info.pModel;
            m_Game->m_CachedArmsModel = true;
        }
    }

    if (info.pModel && info.pModel == m_Game->m_ArmsModel && hideArms)
    {
        m_Game->m_ArmsMaterial->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
        m_Game->m_ModelRender->ForcedMaterialOverride(m_Game->m_ArmsMaterial);
        hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, pBonesToWorldFinal);
        m_Game->m_ModelRender->ForcedMaterialOverride(NULL);
        return;
    }

    {
        struct NativeHandsOnlyRenderSnapshotTLSGuard
        {
            bool active = false;
            bool previous = false;

            explicit NativeHandsOnlyRenderSnapshotTLSGuard(bool enable)
                : active(enable)
            {
                if (active)
                {
                    previous = VR::t_UseRenderFrameSnapshot;
                    VR::t_UseRenderFrameSnapshot = true;
                }
            }

            ~NativeHandsOnlyRenderSnapshotTLSGuard()
            {
                if (active)
                    VR::t_UseRenderFrameSnapshot = previous;
            }
        };

        const int nativeHandsOnlyQueueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
        NativeHandsOnlyRenderSnapshotTLSGuard nativeHandsOnlySnapshotGuard(nativeHandsOnlyQueueMode != 0);

        bool nativeHandsOnlyDrawn = false;
        vr_vm_stabilize::Mat3x4* nativeViewmodelFullArmIkBones = nullptr;
        if (!hideMagazineInteractionCalibrationOriginalViewmodel &&
            HooksNativeViewmodelBuildFullArmIkBones(
                m_VR,
                state,
                modelName,
                pBonesToWorldFinal,
                nativeViewmodelFullArmIkBones))
        {
            hkDrawModelExecute.fOriginal(
                ecx,
                state,
                *pDrawInfo,
                nativeViewmodelFullArmIkBones);
            nativeHandsOnlyDrawn = true;
        }
        std::vector<HooksNativeViewmodelHandsOnlyClipSet> nativeHandsOnlyClipSets;
        ICallQueue* nativeHandsOnlyCallQueue = nullptr;
        CRefPtr<IMatRenderContext> nativeHandsOnlyRenderContext;
        if (!hideMagazineInteractionCalibrationOriginalViewmodel &&
            nativeHandsOnlyQueueMode != 0 && m_Game && m_Game->m_MaterialSystem)
        {
            // GetRenderContext returns an owned reference. Adopt it and retain
            // the exact context through both clip-plane functor insertions.
            nativeHandsOnlyRenderContext =
                m_Game->m_MaterialSystem->GetRenderContext();
            nativeHandsOnlyCallQueue =
                HooksNativeViewmodelHandsOnlyGetSourceRenderCallQueue(
                    nativeHandsOnlyRenderContext);
        }
        if (!nativeHandsOnlyDrawn &&
            !hideMagazineInteractionCalibrationOriginalViewmodel &&
            HooksNativeViewmodelHandsOnlyBuildSplitClipSets(
                m_VR,
                state,
                modelName,
                pBonesToWorldFinal,
                nativeHandsOnlyClipSets))
        {
            for (const HooksNativeViewmodelHandsOnlyClipSet& clipSet : nativeHandsOnlyClipSets)
            {
                if (nativeHandsOnlyQueueMode == 0)
                {
                    ScopedNativeViewmodelHandsOnlyClipPlane nativeHandsOnlyClip(m_VR, clipSet);
                    if (!nativeHandsOnlyClip.Active())
                        continue;
                    hkDrawModelExecute.fOriginal(
                        ecx,
                        state,
                        *pDrawInfo,
                        clipSet.isolatedBones);
                    nativeHandsOnlyDrawn = true;
                }
                else if (clipSet.isolatedBones && nativeHandsOnlyCallQueue)
                {
                    HooksQueuedNativeViewmodelHandsOnlyClipState* queuedClip =
                        new HooksQueuedNativeViewmodelHandsOnlyClipState(m_VR, clipSet);
                    nativeHandsOnlyCallQueue->QueueFunctor(
                        new HooksQueuedNativeViewmodelHandsOnlyClipBeginFunctor(queuedClip));
                    hkDrawModelExecute.fOriginal(
                        ecx,
                        state,
                        *pDrawInfo,
                        clipSet.isolatedBones);
                    nativeHandsOnlyCallQueue->QueueFunctor(
                        new HooksQueuedNativeViewmodelHandsOnlyClipEndFunctor(queuedClip));
                    nativeHandsOnlyDrawn = true;
                }
                else
                {
                    ScopedNativeViewmodelHandsOnlyClipPlane nativeHandsOnlyClip(m_VR, clipSet);
                    if (!nativeHandsOnlyClip.Active())
                        continue;
                    hkDrawModelExecute.fOriginal(
                        ecx,
                        state,
                        *pDrawInfo,
                        clipSet.isolatedBones);
                    nativeHandsOnlyDrawn = true;
                }
            }
        }

        if (!nativeHandsOnlyDrawn)
        {
            if (hideMagazineInteractionCalibrationOriginalViewmodel)
            {
                if (drawMagazineInteractionCalibrationPreview && magazineInteractionCalibrationPreviewBones)
                {
                    ModelRenderInfo_t previewInfo = *pDrawInfo;
                    previewInfo.origin = magazineInteractionCalibrationPreviewOrigin;
                    previewInfo.angles = magazineInteractionCalibrationPreviewAngles;
                    if (magazineInteractionCalibrationPreviewModelToWorld)
                    {
                        previewInfo.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(magazineInteractionCalibrationPreviewModelToWorld);
                        previewInfo.pLightingOffset = nullptr;
                        previewInfo.pLightingOrigin = nullptr;
                    }
                    hkDrawModelExecute.fOriginal(
                        ecx,
                        state,
                        previewInfo,
                        magazineInteractionCalibrationPreviewBones);
                    magazineInteractionCalibrationPreviewDrawnAsPrimary = true;
                }
            }
            else if (!nativeViewmodelPendingFreezeHide &&
                !nativeViewmodelFullArmIkFailClosed)
            {
                ScopedNativeViewmodelHandsOnlyClipPlane nativeHandsOnlyClip(
                    m_VR,
                    state,
                    modelName,
                    pBonesToWorldFinal);
                hkDrawModelExecute.fOriginal(
                    ecx,
                    state,
                    *pDrawInfo,
                    pBonesToWorldFinal);
            }
        }
    }

    // Draw the detached/new magazine as a second pass of the original weapon
    // viewmodel. Every non-clip bone is moved out of view, so Source applies the
    // same active material, shader, skin, lighting and post-processing as the gun.
    if (drawMagazineInteractionDetachedMagazine && magazineInteractionDetachedMagazineBones)
        hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, magazineInteractionDetachedMagazineBones);

    if (drawMagazineInteractionCalibrationPreview &&
        magazineInteractionCalibrationPreviewBones &&
        !magazineInteractionCalibrationPreviewDrawnAsPrimary)
    {
        ModelRenderInfo_t previewInfo = *pDrawInfo;
        previewInfo.origin = magazineInteractionCalibrationPreviewOrigin;
        previewInfo.angles = magazineInteractionCalibrationPreviewAngles;
        if (magazineInteractionCalibrationPreviewModelToWorld)
        {
            previewInfo.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(magazineInteractionCalibrationPreviewModelToWorld);
            previewInfo.pLightingOffset = nullptr;
            previewInfo.pLightingOrigin = nullptr;
        }
        hkDrawModelExecute.fOriginal(ecx, state, previewInfo, magazineInteractionCalibrationPreviewBones);
    }
}

// Returns true if the engine RT being pushed looks like the HUD/VGUI render target.
// This is a heuristic (names + dimensions) to avoid hijacking other offscreen passes.
static bool IsHudRenderTarget(ITexture* texture, ITexture* hudTexture)
{
    if (!texture)
        return false;

    const char* name = texture->GetName();
    if (name && *name)
    {
        auto ciFind = [](const char* haystack, const char* needle) -> bool
            {
                const size_t nLen = strlen(needle);
                for (const char* p = haystack; *p; ++p)
                {
                    if (_strnicmp(p, needle, nLen) == 0)
                        return true;
                }
                return false;
            };

        // Exclude obvious non-HUD targets
        if (ciFind(name, "backbuffer") || ciFind(name, "left") || ciFind(name, "right") ||
            ciFind(name, "blank") || ciFind(name, "scope") || ciFind(name, "rearmirror"))
            return false;

        if (ciFind(name, "vgui") || ciFind(name, "hud"))
            return true;
    }

    // Fallback: match the HUD texture size
    if (hudTexture)
    {
        const int hudW = hudTexture->GetMappingWidth();
        const int hudH = hudTexture->GetMappingHeight();
        if (hudW > 0 && hudH > 0)
        {
            if (texture->GetMappingWidth() == hudW && texture->GetMappingHeight() == hudH)
                return true;
        }
    }

    return false;
}

void Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (m_VR->m_RenderPipelineDebugLog)
    {
        static thread_local std::chrono::steady_clock::time_point s_lastPushRtLog{};
        if (!ShouldThrottleLog(s_lastPushRtLog, m_VR->m_RenderPipelineDebugLogHz))
        {
            int texMapW = 0;
            int texMapH = 0;
            int texActualW = 0;
            int texActualH = 0;
            DebugTextureFullSize(pTexture, texMapW, texMapH, texActualW, texActualH);

            ITexture* hudTexture = nullptr;
            {
                std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
                hudTexture = m_VR->m_HUDTexture;
            }
            int hudMapW = 0;
            int hudMapH = 0;
            int hudActualW = 0;
            int hudActualH = 0;
            DebugTextureFullSize(hudTexture, hudMapW, hudMapH, hudActualW, hudActualH);

            CRefPtr<IMatRenderContext> contextRef;
            if (m_Game && m_Game->m_MaterialSystem)
                contextRef = m_Game->m_MaterialSystem->GetRenderContext();
            IMatRenderContext* const ctx = contextRef;
            int windowW = 0;
            int windowH = 0;
            int backBufferW = 0;
            int backBufferH = 0;
            int clientW = 0;
            int clientH = 0;
            int curVpX = 0;
            int curVpY = 0;
            int curVpW = 0;
            int curVpH = 0;
            DebugRenderContextWindowSize(ctx, windowW, windowH);
            DebugBackBufferDimensions(m_Game ? m_Game->m_MaterialSystem : nullptr, backBufferW, backBufferH);
            DebugClientRectSize(clientW, clientH);
            DebugGetViewport(ctx, curVpX, curVpY, curVpW, curVpH);

            Game::logMsg("[VR][DesktopHUD][PushRT] tid=%lu q=%d step=%d pushed=%d hudPainted=%d suppress=%d win=%dx%d client=%dx%d bb=%dx%d curVp=%d,%d %dx%d tex=%s(map=%dx%d actual=%dx%d) reqVp=%d,%d %dx%d hudTex=%s(map=%dx%d actual=%dx%d)",
                GetCurrentThreadId(), queueMode,
                static_cast<int>(m_HUDStep), m_PushedHud ? 1 : 0,
                m_VR->m_HudPaintedThisFrame.load(std::memory_order_acquire) ? 1 : 0,
                m_VR->m_SuppressHudCapture ? 1 : 0,
                windowW, windowH, clientW, clientH, backBufferW, backBufferH, curVpX, curVpY, curVpW, curVpH,
                DebugTextureName(pTexture), texMapW, texMapH, texActualW, texActualH,
                nViewX, nViewY, nViewW, nViewH,
                DebugTextureName(hudTexture), hudMapW, hudMapH, hudActualW, hudActualH);
        }
    }

    // Extra offscreen passes (scope/rear-mirror RTT) must not hijack HUD capture
    if (m_VR->m_SuppressHudCapture)
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

    if (queueMode != 0)
    {
        // Queued/multicore path: the Pop->IsSplitScreen->PrePush->Push sequence
        // isn't reliable, so never attempt RT hijack here.
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    // Single-threaded path (mat_queue_mode 0): use state machine to detect HUD push.
    bool overrideHudRT = (m_HUDStep == HUDPushStep::ReadyToOverride) &&
        !m_VR->m_HudPaintedThisFrame.load(std::memory_order_relaxed);

    if (overrideHudRT)
    {
        std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
        if (!m_VR->m_HUDTexture || !IsHudRenderTarget(pTexture, m_VR->m_HUDTexture))
            overrideHudRT = false;
    }

    if (!overrideHudRT)
    {
        m_PushedHud = false;
        m_HUDStep = HUDPushStep::None;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    ITexture* hudTexture = nullptr;
    {
        std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
        hudTexture = m_VR->m_HUDTexture;
    }

    if (!hudTexture)
    {
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    CRefPtr<IMatRenderContext> renderContextRef;
    if (m_Game->m_MaterialSystem)
        renderContextRef = m_Game->m_MaterialSystem->GetRenderContext();
    IMatRenderContext* const renderContext = renderContextRef;
    if (!renderContext)
    {
        m_VR->HandleMissingRenderContext("Hooks::dPushRenderTargetAndViewport");
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    // Clear depth/stencil first, then push RT and clear color to transparent.
    renderContext->ClearBuffers(false, true, true);
    hkPushRenderTargetAndViewport.fOriginal(ecx, hudTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    renderContext->OverrideAlphaWriteEnable(true, true);
    renderContext->ClearColor4ub(0, 0, 0, 0);
    renderContext->ClearBuffers(true, false);

    if (m_VR->m_RenderPipelineDebugLog)
    {
        int vpX = 0;
        int vpY = 0;
        int vpW = 0;
        int vpH = 0;
        DebugGetViewport(renderContext, vpX, vpY, vpW, vpH);
        int hudMapW = 0;
        int hudMapH = 0;
        int hudActualW = 0;
        int hudActualH = 0;
        DebugTextureFullSize(hudTexture, hudMapW, hudMapH, hudActualW, hudActualH);
        Game::logMsg("[VR][DesktopHUD][PushOverride] tid=%lu requestedVp=%d,%d %dx%d actualVp=%d,%d %dx%d hudTex=%s(map=%dx%d actual=%dx%d)",
            GetCurrentThreadId(), nViewX, nViewY, nViewW, nViewH, vpX, vpY, vpW, vpH,
            DebugTextureName(hudTexture), hudMapW, hudMapH, hudActualW, hudActualH);
    }

    m_PushedHud = true;
    m_HUDStep = HUDPushStep::None;
}

void Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkPopRenderTargetAndViewport.fOriginal(ecx);

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    m_HUDStep = (queueMode == 0) ? HUDPushStep::AfterPop : HUDPushStep::None;

    if (m_PushedHud)
    {
        CRefPtr<IMatRenderContext> renderContextRef;
        if (m_Game->m_MaterialSystem)
            renderContextRef = m_Game->m_MaterialSystem->GetRenderContext();
        IMatRenderContext* const renderContext = renderContextRef;
        if (renderContext)
        {
            renderContext->OverrideAlphaWriteEnable(false, true);
            renderContext->ClearColor4ub(0, 0, 0, 255);
        }
    }

    hkPopRenderTargetAndViewport.fOriginal(ecx);
    m_PushedHud = false;
}

void Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkVgui_Paint.fOriginal(ecx, mode);

    const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    const bool isPaused = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsPaused();
    const bool cursorVisible = (m_Game && m_Game->m_VguiSurface) ? m_Game->m_VguiSurface->IsCursorVisible() : false;
    const bool focusedInGameVgui = inGame && (isPaused || cursorVisible);
    const bool gameplayHudRequested = inGame && m_VR->IsGameplayHudRequested();

    auto BuildFullHudPaintMode = [&](int paintMode)
        {
            int fullHudMode = PAINT_UIPANELS | PAINT_INGAMEPANELS;
            if (cursorVisible)
                fullHudMode |= PAINT_CURSOR;
            return paintMode | fullHudMode;
        };

    // Extra offscreen passes such as scope / rear mirror should not recurse through
    // the VGUI capture path. The selected desktop-mirror clean pass is the one
    // exception: when the gameplay HUD is requested, let Source paint VGUI directly
    // into desktopMirrorClean0 so spectators see the same HUD state without placing
    // the HUD inside the VR eye textures.
    if (m_VR->m_SuppressHudCapture)
    {
        if (!inGame)
            return hkVgui_Paint.fOriginal(ecx, mode);

        if (m_VR->m_DesktopMirrorCleanRenderingPass && (focusedInGameVgui || gameplayHudRequested))
            return hkVgui_Paint.fOriginal(ecx, BuildFullHudPaintMode(mode));

        return;
    }

    auto IsPaintingToNativeBackBuffer = [&]() -> bool
        {
            CRefPtr<IMatRenderContext> contextRef;
            if (m_Game && m_Game->m_MaterialSystem)
                contextRef = m_Game->m_MaterialSystem->GetRenderContext();
            IMatRenderContext* const ctx = contextRef;
            if (!ctx)
                return false;

            // In Source's material system a null render target represents the current backbuffer.
            // Only allow native desktop VGUI in that case. If this is an eye RT, HUD RT, scope RT,
            // mirror RT, water RT, etc., capture to m_HUDTexture only and do not draw into that target.
            return ctx->GetRenderTarget() == nullptr;
        };

    auto PaintToHudOnce = [&](int paintMode)
        {
            bool expected = false;
            if (!m_VR->m_HudPaintedThisFrame.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return;

            CRefPtr<IMatRenderContext> contextRef;
            if (m_Game && m_Game->m_MaterialSystem)
                contextRef = m_Game->m_MaterialSystem->GetRenderContext();
            IMatRenderContext* const ctx = contextRef;
            if (!ctx)
            {
                m_VR->HandleMissingRenderContext("Hooks::dVGui_Paint");
                return;
            }

            ITexture* hudTexture = nullptr;
            {
                std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
                hudTexture = m_VR->m_HUDTexture;
            }
            if (!hudTexture)
                return;

            ITexture* prevTarget = ctx->GetRenderTarget();
            int hudMapW = hudTexture->GetMappingWidth();
            int hudMapH = hudTexture->GetMappingHeight();
            if (hudMapW <= 0)
                hudMapW = 1;
            if (hudMapH <= 0)
                hudMapH = 1;

            int oldX = 0;
            int oldY = 0;
            int oldW = 0;
            int oldH = 0;
            const bool canRestoreViewport = hkGetViewport.fOriginal && hkViewport.fOriginal &&
                DebugGetViewport(ctx, oldX, oldY, oldW, oldH);

            ctx->SetRenderTarget(hudTexture);
            if (hkViewport.fOriginal)
                hkViewport.fOriginal(ctx, 0, 0, hudMapW, hudMapH);

            ctx->OverrideAlphaWriteEnable(true, true);
            ctx->ClearColor4ub(0, 0, 0, isPaused ? 255 : 0);
            ctx->ClearBuffers(true, false, false);

            hkVgui_Paint.fOriginal(ecx, paintMode);
            m_VR->DrawKillIndicators(ctx, hudTexture);

            ctx->OverrideAlphaWriteEnable(false, true);
            ctx->SetRenderTarget(prevTarget);
            if (canRestoreViewport)
                hkViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);

            m_VR->m_RenderedHud.store(true, std::memory_order_release);
            m_VR->MarkQueuedHudFresh();
        };

    if (inGame)
    {
        // In VR gameplay, capture is allowed only while the HUD is actually meant to be visible:
        // focused UI, HudAlwaysVisible=true, or the off-hand lift request. Every other gameplay
        // state is a hard capture stop. When the current target is the native backbuffer, paint
        // there too so desktop spectators match the requested HUD state.
        if (focusedInGameVgui || gameplayHudRequested)
        {
            const int fullHudMode = BuildFullHudPaintMode(mode);
            PaintToHudOnce(fullHudMode);

            // HudAlwaysVisible/lift/menu must also be visible on the desktop when Source is
            // currently painting the native backbuffer. This does not weaken the capture-stop
            // rule: the branch is reached only while the HUD is explicitly requested.
            if (IsPaintingToNativeBackBuffer())
            {
                hkVgui_Paint.fOriginal(ecx, fullHudMode);
                m_VR->m_NativeDesktopHudPainted.store(true, std::memory_order_release);
            }
        }
        else
        {
            m_VR->m_RenderedHud.store(false, std::memory_order_release);
            m_VR->ClearQueuedHudFresh();
        }
        return;
    }

    // Main menu / loading screens are not VR gameplay; keep normal desktop VGUI.
    hkVgui_Paint.fOriginal(ecx, mode);
}

//
int Hooks::dIsSplitScreen()
{
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
    {
        if (m_HUDStep == HUDPushStep::AfterPop)
            m_HUDStep = HUDPushStep::AfterIsSplitScreen;
        else
            m_HUDStep = HUDPushStep::None;
    }
    else
    {
        m_HUDStep = HUDPushStep::None;
    }

    return hkIsSplitScreen.fOriginal();
}

DWORD* Hooks::dPrePushRenderTarget(void* ecx, void* edx, int a2)
{
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
    {
        if (m_HUDStep == HUDPushStep::AfterIsSplitScreen)
            m_HUDStep = HUDPushStep::ReadyToOverride;
        else
            m_HUDStep = HUDPushStep::None;
    }
    else
    {
        m_HUDStep = HUDPushStep::None;
    }

    return hkPrePushRenderTarget.fOriginal(ecx, a2);
}

void Hooks::dSayText(void* msgData)
{
    TryLogHudUserMessagePayload("SayText", msgData);
    hkSayText.fOriginal(msgData);
}

void Hooks::dSayText2(void* msgData)
{
    TryLogHudUserMessagePayload("SayText2", msgData);
    hkSayText2.fOriginal(msgData);
}

void Hooks::dTextMsg(void* msgData)
{
    TryLogHudUserMessagePayload("TextMsg", msgData);
    hkTextMsg.fOriginal(msgData);
}

void Hooks::dConVarSetValueString(void* ecx, void* edx, const char* value)
{
    const bool blocked = ShouldBlockLockedConVarWrite(ecx, value);
    TraceTrackedConVarWrite(ecx, value, "IConVar::SetValue(string)", _ReturnAddress(), true, blocked);
    if (blocked)
        return;

    hkConVarSetValueString.fOriginal(ecx, value);
}

void Hooks::dConVarSetValueFloat(void* ecx, void* edx, float value)
{
    char buffer[64] = {};
    sprintf_s(buffer, "%.9g", static_cast<double>(value));
    const bool blocked = ShouldBlockLockedConVarWrite(ecx, buffer);
    TraceTrackedConVarWrite(ecx, buffer, "IConVar::SetValue(float)", _ReturnAddress(), true, blocked);
    if (blocked)
        return;

    hkConVarSetValueFloat.fOriginal(ecx, value);
}

void Hooks::dConVarSetValueInt(void* ecx, void* edx, int value)
{
    char buffer[32] = {};
    sprintf_s(buffer, "%d", value);
    const bool blocked = ShouldBlockLockedConVarWrite(ecx, buffer);
    TraceTrackedConVarWrite(ecx, buffer, "IConVar::SetValue(int)", _ReturnAddress(), true, blocked);
    if (blocked)
        return;

    hkConVarSetValueInt.fOriginal(ecx, value);
}

void Hooks::dConVarPrimarySetValueString(void* ecx, void* edx, const char* value)
{
    TraceTrackedConVarWrite(ecx, value, "ConVar::SetValue(string)", _ReturnAddress(), false, false);
    hkConVarPrimarySetValueString.fOriginal(ecx, value);
}

void Hooks::dConVarPrimarySetValueFloat(void* ecx, void* edx, float value)
{
    char buffer[64] = {};
    sprintf_s(buffer, "%.9g", static_cast<double>(value));
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::SetValue(float)", _ReturnAddress(), false, false);
    hkConVarPrimarySetValueFloat.fOriginal(ecx, value);
}

void Hooks::dConVarPrimarySetValueInt(void* ecx, void* edx, int value)
{
    char buffer[32] = {};
    sprintf_s(buffer, "%d", value);
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::SetValue(int)", _ReturnAddress(), false, false);
    hkConVarPrimarySetValueInt.fOriginal(ecx, value);
}

void Hooks::dConVarInternalSetValueString(void* ecx, void* edx, const char* value)
{
    TraceTrackedConVarWrite(ecx, value, "ConVar::InternalSetValue(string)", _ReturnAddress(), false, false);
    hkConVarInternalSetValueString.fOriginal(ecx, value);
}

void Hooks::dConVarInternalSetValueFloat(void* ecx, void* edx, float value)
{
    char buffer[64] = {};
    sprintf_s(buffer, "%.9g", static_cast<double>(value));
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::InternalSetValue(float)", _ReturnAddress(), false, false);
    hkConVarInternalSetValueFloat.fOriginal(ecx, value);
}

void Hooks::dConVarInternalSetValueInt(void* ecx, void* edx, int value)
{
    char buffer[32] = {};
    sprintf_s(buffer, "%d", value);
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::InternalSetValue(int)", _ReturnAddress(), false, false);
    hkConVarInternalSetValueInt.fOriginal(ecx, value);
}


void Hooks::dEmitSoundAttenuation(
    void* ecx,
    void* edx,
    void* filter,
    int entIndex,
    int channel,
    const char* sample,
    float volume,
    float attenuation,
    int flags,
    int pitch,
    const Vector* origin,
    const Vector* direction,
    void* origins,
    bool updatePositions,
    float soundTime,
    int speakerEntity)
{
    if (m_VR && m_VR->CaptureMagazineInteractionSound(entIndex, sample, volume, flags, pitch))
        return;

    hkEmitSoundAttenuation.fOriginal(
        ecx,
        filter,
        entIndex,
        channel,
        sample,
        volume,
        attenuation,
        flags,
        pitch,
        origin,
        direction,
        origins,
        updatePositions,
        soundTime,
        speakerEntity);
}

void Hooks::dEmitSoundLevel(
    void* ecx,
    void* edx,
    void* filter,
    int entIndex,
    int channel,
    const char* sample,
    float volume,
    int soundLevel,
    int flags,
    int pitch,
    const Vector* origin,
    const Vector* direction,
    void* origins,
    bool updatePositions,
    float soundTime,
    int speakerEntity)
{
    if (m_VR && m_VR->CaptureMagazineInteractionSound(entIndex, sample, volume, flags, pitch))
        return;

    hkEmitSoundLevel.fOriginal(
        ecx,
        filter,
        entIndex,
        channel,
        sample,
        volume,
        soundLevel,
        flags,
        pitch,
        origin,
        direction,
        origins,
        updatePositions,
        soundTime,
        speakerEntity);
}
