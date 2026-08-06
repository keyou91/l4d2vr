#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Wire-only definitions shared by the client DLL's built-in listen relay and
// the optional dedicated-server plugin. The payload deliberately contains no
// Source SDK types so both paths validate identical data.
namespace l4d2vr_pose
{
    constexpr std::uint8_t kMagic = 0xB7u;
    constexpr std::uint8_t kVersion = 2u;

    constexpr std::uint8_t kValidHmd = 1u << 0;
    constexpr std::uint8_t kValidLeftHand = 1u << 1;
    constexpr std::uint8_t kValidRightHand = 1u << 2;
    constexpr std::uint8_t kValidMask =
        kValidHmd | kValidLeftHand | kValidRightHand;

    constexpr std::uint8_t kFeatureBodyYaw = 1u << 0;
    constexpr std::uint8_t kFeatureLeftFingerCurls = 1u << 1;
    constexpr std::uint8_t kFeatureRightFingerCurls = 1u << 2;
    constexpr std::uint8_t kFeatureMask =
        kFeatureBodyYaw |
        kFeatureLeftFingerCurls |
        kFeatureRightFingerCurls;

    // A native-animation bit means that side must retain the receiver's
    // current world-model finger animation. When it is clear, the receiver
    // freezes that hand on its stable rest base and may layer transmitted
    // OpenVR curls over it.
    constexpr std::uint8_t kHandStateLeftNativeFingerAnimation = 1u << 0;
    constexpr std::uint8_t kHandStateRightNativeFingerAnimation = 1u << 1;
    constexpr std::uint8_t kHandStateTwoHandedGrip = 1u << 2;
    constexpr std::uint8_t kHandStateEmptyHands = 1u << 3;
    constexpr std::uint8_t kHandStateMask =
        kHandStateLeftNativeFingerAnimation |
        kHandStateRightNativeFingerAnimation |
        kHandStateTwoHandedGrip |
        kHandStateEmptyHands;

    constexpr float kPositionUnitsPerStep = 0.125f;
    constexpr float kFingerCurlMaximum = 2.0f;

#pragma pack(push, 1)
    struct PackedTrackedPose
    {
        std::int16_t position[3]{};
        std::int16_t rotation[3]{};
    };

    struct WirePacket
    {
        std::uint8_t magic = kMagic;
        // High nibble is the protocol version; low nibble is the tracked-point
        // validity mask.
        std::uint8_t versionAndValidMask =
            static_cast<std::uint8_t>((kVersion << 4) | kValidMask);
        std::uint8_t featureMask = 0u;
        std::uint8_t handStateFlags =
            kHandStateRightNativeFingerAnimation;
        std::int16_t bodyYaw = 0;
        PackedTrackedPose hmd{};
        PackedTrackedPose leftHand{};
        PackedTrackedPose rightHand{};
        std::uint8_t leftFingerCurls[5]{};
        std::uint8_t rightFingerCurls[5]{};
        std::uint16_t crc16 = 0u;
    };
#pragma pack(pop)

    static_assert(sizeof(PackedTrackedPose) == 12, "Unexpected VR pose transform size.");
    static_assert(sizeof(WirePacket) == 54, "VR pose protocol 2 wire packet must be 54 bytes.");

    constexpr std::size_t kWirePacketBytes = sizeof(WirePacket);
    constexpr std::size_t kEncodedPayloadChars =
        (kWirePacketBytes * 8u + 5u) / 6u;
    static_assert(kEncodedPayloadChars == 72u, "Unexpected protocol 2 payload length.");

    inline std::uint8_t PacketVersion(const WirePacket& packet)
    {
        return static_cast<std::uint8_t>(packet.versionAndValidMask >> 4);
    }

    inline std::uint8_t PacketValidMask(const WirePacket& packet)
    {
        return static_cast<std::uint8_t>(packet.versionAndValidMask & 0x0Fu);
    }

    inline std::uint8_t PacketFeatureMask(const WirePacket& packet)
    {
        return static_cast<std::uint8_t>(packet.featureMask & kFeatureMask);
    }

    inline std::uint8_t PacketHandStateFlags(const WirePacket& packet)
    {
        return static_cast<std::uint8_t>(packet.handStateFlags & kHandStateMask);
    }

    inline std::uint16_t Crc16Ccitt(const void* bytes, std::size_t byteCount)
    {
        const auto* input = static_cast<const std::uint8_t*>(bytes);
        std::uint16_t crc = 0xFFFFu;
        for (std::size_t index = 0; index < byteCount; ++index)
        {
            crc ^= static_cast<std::uint16_t>(input[index]) << 8;
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc & 0x8000u)
                    ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u)
                    : static_cast<std::uint16_t>(crc << 1);
            }
        }
        return crc;
    }

    inline void FinalizePacket(WirePacket& packet)
    {
        packet.magic = kMagic;
        packet.versionAndValidMask = static_cast<std::uint8_t>(
            (kVersion << 4) | (PacketValidMask(packet) & kValidMask));
        packet.featureMask = PacketFeatureMask(packet);
        packet.handStateFlags = PacketHandStateFlags(packet);
        if ((packet.featureMask & kFeatureBodyYaw) == 0u)
            packet.bodyYaw = 0;
        if ((packet.featureMask & kFeatureLeftFingerCurls) == 0u)
            std::memset(packet.leftFingerCurls, 0, sizeof(packet.leftFingerCurls));
        if ((packet.featureMask & kFeatureRightFingerCurls) == 0u)
            std::memset(packet.rightFingerCurls, 0, sizeof(packet.rightFingerCurls));
        packet.crc16 = 0u;
        packet.crc16 = Crc16Ccitt(
            &packet,
            offsetof(WirePacket, crc16));
    }

    inline bool ValidatePacket(const WirePacket& packet)
    {
        const std::uint8_t validMask = PacketValidMask(packet);
        if (packet.magic != kMagic ||
            PacketVersion(packet) != kVersion ||
            (validMask & ~kValidMask) != 0u ||
            (packet.featureMask & ~kFeatureMask) != 0u ||
            (packet.handStateFlags & ~kHandStateMask) != 0u)
        {
            return false;
        }

        const std::uint8_t features = PacketFeatureMask(packet);
        const std::uint8_t handState = PacketHandStateFlags(packet);
        const bool leftNative =
            (handState & kHandStateLeftNativeFingerAnimation) != 0u;
        const bool rightNative =
            (handState & kHandStateRightNativeFingerAnimation) != 0u;
        const bool twoHanded =
            (handState & kHandStateTwoHandedGrip) != 0u;
        const bool emptyHands =
            (handState & kHandStateEmptyHands) != 0u;

        if (((features & kFeatureLeftFingerCurls) != 0u &&
             ((validMask & kValidLeftHand) == 0u || leftNative)) ||
            ((features & kFeatureRightFingerCurls) != 0u &&
             ((validMask & kValidRightHand) == 0u || rightNative)) ||
            (twoHanded && (!leftNative || !rightNative || emptyHands)) ||
            (emptyHands && rightNative))
        {
            return false;
        }

        return packet.crc16 == Crc16Ccitt(
            &packet,
            offsetof(WirePacket, crc16));
    }

    inline bool IsSequenceNewer(std::uint16_t candidate, std::uint16_t reference)
    {
        return static_cast<std::int16_t>(candidate - reference) > 0;
    }

    inline bool EncodePayload(const WirePacket& packet, std::string& out)
    {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789-_";

        out.clear();
        out.reserve(kEncodedPayloadChars);

        const auto* input = reinterpret_cast<const std::uint8_t*>(&packet);
        std::uint32_t accumulator = 0u;
        int accumulatedBits = 0;
        for (std::size_t index = 0; index < sizeof(packet); ++index)
        {
            accumulator = (accumulator << 8) | input[index];
            accumulatedBits += 8;
            while (accumulatedBits >= 6)
            {
                accumulatedBits -= 6;
                out.push_back(kAlphabet[(accumulator >> accumulatedBits) & 0x3Fu]);
            }
        }

        if (accumulatedBits > 0)
            out.push_back(kAlphabet[(accumulator << (6 - accumulatedBits)) & 0x3Fu]);

        return out.size() == kEncodedPayloadChars;
    }

    inline int DecodePayloadCharacter(unsigned char value)
    {
        if (value >= 'A' && value <= 'Z')
            return static_cast<int>(value - 'A');
        if (value >= 'a' && value <= 'z')
            return static_cast<int>(value - 'a') + 26;
        if (value >= '0' && value <= '9')
            return static_cast<int>(value - '0') + 52;
        if (value == '-')
            return 62;
        if (value == '_')
            return 63;
        return -1;
    }

    inline bool DecodePayload(const char* encoded, WirePacket& out)
    {
        if (!encoded || std::strlen(encoded) != kEncodedPayloadChars)
            return false;

        std::uint8_t decoded[kWirePacketBytes]{};
        std::size_t decodedBytes = 0;
        std::uint32_t accumulator = 0u;
        int accumulatedBits = 0;

        for (std::size_t index = 0; index < kEncodedPayloadChars; ++index)
        {
            const int value = DecodePayloadCharacter(
                static_cast<unsigned char>(encoded[index]));
            if (value < 0)
                return false;

            accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
            accumulatedBits += 6;
            if (accumulatedBits >= 8)
            {
                accumulatedBits -= 8;
                if (decodedBytes >= kWirePacketBytes)
                    return false;
                decoded[decodedBytes++] = static_cast<std::uint8_t>(
                    (accumulator >> accumulatedBits) & 0xFFu);
            }
        }

        if (decodedBytes != kWirePacketBytes)
            return false;

        std::memcpy(&out, decoded, sizeof(out));
        return ValidatePacket(out);
    }
}
