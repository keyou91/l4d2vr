#include "../L4D2VR/vr_client_command.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void* sentServer = nullptr;
static int* sentEntity = nullptr;
static char sentText[128]{};
static unsigned calls = 0u;
static void Check(bool value)
{
    if (!value) { std::puts("Client command transport check failed"); std::abort(); }
}
#ifdef _MSC_VER
static void __cdecl Capture(void* server, int* entity, const char* format, ...)
#else
static void Capture(void* server, int* entity, const char* format, ...)
#endif
{
    sentServer = server; sentEntity = entity; ++calls;
    std::va_list arguments; va_start(arguments, format);
    std::vsnprintf(sentText, sizeof(sentText), format, arguments);
    va_end(arguments);
}
int main()
{
    using namespace l4d2vr_client_command;
    unsigned char native[] = {0x55,0x8b,0xec,0x81,0xec,0x5c,0x04,0,0,0xa1,0x98,0xdd,0x43,0x10,
        0x33,0xc5,0x89,0x45,0xfc,0x8b,0x4d,0x10,0x53,0x56,0x8b,0x75,0x0c,0x8d,0x45,0x14};
    Check(SupportedCode(native, sizeof(native)));
    native[10] ^= 0x55u; // The security-cookie address relocates with the module.
    Check(SupportedCode(native, sizeof(native)));
    native[21] = 0x0cu; // A different argument convention must not be called.
    Check(!SupportedCode(native, sizeof(native)));
    Check(!SupportedCode(native, 29u));
    Check(!SupportedCode(nullptr, 30u));
    int server = 1, entity = 2;
    Check(Send(&Capture, &server, &entity, "l4d2vr_pose_ack 2\n"));
    Check(sentServer == &server && sentEntity == &entity);
    Check(std::strcmp(sentText, "l4d2vr_pose_ack 2\n") == 0);
    Check(Send(&Capture, &server, &entity, "literal %s %n %%"));
    Check(std::strcmp(sentText, "literal %s %n %%") == 0);
    Check(!Send(&Capture, nullptr, &entity, "test"));
    Check(!Send(&Capture, &server, static_cast<int*>(nullptr), "test"));
    Check(!Send(&Capture, &server, &entity, ""));
    Check(calls == 2u);
    std::puts("Client command transport checks passed");
}
