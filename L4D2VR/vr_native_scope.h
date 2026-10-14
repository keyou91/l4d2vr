#pragma once
#include <type_traits>

namespace l4d2vr_native_scope
{
    // Native callbacks can leave through a structured exception. Restore the
    // execution pointer on that path as well as normal/C++ exception returns.
    template<class T, class Callback> void Run(T*& slot, T* current, Callback callback)
    {
        static_assert(std::is_trivially_destructible<Callback>::value,
            "Native scope callbacks must not require object unwinding");
        T* previous = slot;
        slot = current;
#ifdef _MSC_VER
        __try { callback(); }
        __finally { slot = previous; }
#else
        struct Restore
        {
            T*& slot;
            T* previous;
            ~Restore() { slot = previous; }
        } restore{slot, previous};
        callback();
#endif
    }
}
