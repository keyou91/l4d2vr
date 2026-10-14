#pragma once
#include <cstddef>
#include <cstring>

namespace l4d2vr_client_command
{
    constexpr std::size_t EngineSlot = 39u;

    inline bool SupportedCode(const unsigned char* code, std::size_t length)
    {
        // Verified VEngineServer022 ClientCommand in the installed x86 engine.
        // Its stack arguments are this, edict, format and then varargs.
        constexpr unsigned char prefix[] = {0x55,0x8b,0xec,0x81,0xec,0x5c,0x04,0,0,0xa1};
        constexpr unsigned char arguments[] = {0x33,0xc5,0x89,0x45,0xfc,0x8b,0x4d,0x10,
            0x53,0x56,0x8b,0x75,0x0c,0x8d,0x45,0x14};
        return code && length >= 30u && std::memcmp(code, prefix, sizeof(prefix)) == 0 &&
            std::memcmp(code + 14u, arguments, sizeof(arguments)) == 0;
    }

    template<class Function, class Entity>
    bool Send(Function function, void* server, Entity* entity, const char* command)
    {
        if (!function || !server || !entity || !command || !*command) return false;
        // Never interpret protocol text as a printf format.
        function(server, entity, "%s", command);
        return true;
    }
}
