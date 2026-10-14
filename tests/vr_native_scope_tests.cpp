#include "../L4D2VR/vr_native_scope.h"
#include <cstdio>
#include <cstdlib>
#ifdef _MSC_VER
#include <Windows.h>
#endif
static const int* current = nullptr;
#if defined(_MSC_VER) && defined(_M_IX86)
// Native melee writes AL and leaves the upper result register unspecified.
__declspec(naked) static bool __cdecl NativeFalseByte()
{
    __asm { mov eax, 01000000h }
    __asm { ret }
}
__declspec(naked) static bool __cdecl NativeTrueByte()
{
    __asm { mov eax, 01000001h }
    __asm { ret }
}
#endif
static void Check(bool value)
{
    if (!value) { std::puts("Native execution scope check failed"); std::abort(); }
}
#ifdef _MSC_VER
static bool StructuredFailure(const int* value)
{
    __try
    {
        l4d2vr_native_scope::Run(current, value, []() { RaiseException(0xe0421001u, 0, 0, nullptr); });
    }
    __except (GetExceptionCode() == 0xe0421001u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return true; }
    return false;
}
#endif
int main()
{
#if defined(_MSC_VER) && defined(_M_IX86)
    Check(!NativeFalseByte() && NativeTrueByte());
#endif
    const int outer = 1, first = 2, second = 3;
    current = &outer;
    l4d2vr_native_scope::Run(current, &first, [&]()
    {
        Check(current == &first);
        l4d2vr_native_scope::Run(current, &second, [&]() { Check(current == &second); });
        Check(current == &first);
    });
    Check(current == &outer);
    try
    {
        l4d2vr_native_scope::Run(current, &first, []() { throw 7; });
        Check(false);
    }
    catch (int value) { Check(value == 7 && current == &outer); }
#ifdef _MSC_VER
    Check(StructuredFailure(&first) && current == &outer);
#endif
    current = nullptr;
    l4d2vr_native_scope::Run(current, &first, []() {});
    Check(current == nullptr);
    std::puts("Native execution scope restoration checks passed");
}
