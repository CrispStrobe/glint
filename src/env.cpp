// glint - internal environment-variable reader
// MIT License - Clean-room implementation
//
// THE REASON THIS EXISTS: every GLINT_* experiment switch used getenv(), and MSVC reports C4996 for
// it at /W4, as does the strict build of every consumer that compiles these sources. The switch is
// read-only and never expands in a loop, so the fix belongs here rather than in a per-consumer
// /wd4996 or a _CRT_SECURE_NO_WARNINGS that would hide the same warning in their own code.
//
// MSVC gets getenv_s, which writes into a buffer this file owns and cannot alias the process
// environment; a variable that is set to an empty string reads as unset, which is what every call
// site already assumed. getenv_s itself is not in the C++ standard, so it is the reason for the
// two branches rather than one.

#include "env.hpp"

#include <cstdlib>

namespace glint {

const char* glint_getenv(const char* name) {
#if defined(_MSC_VER)
    static thread_local char buf[256];
    std::size_t len = 0;
    if (name == nullptr || getenv_s(&len, nullptr, 0, name) != 0) return nullptr;
    if (len == 0 || len > sizeof(buf)) return nullptr;
    if (getenv_s(&len, buf, sizeof(buf), name) != 0) return nullptr;
    return len > 0 ? buf : nullptr;
#else
    return name != nullptr ? std::getenv(name) : nullptr;
#endif
}

} // namespace glint
