// glint - internal environment-variable reader
// MIT License - Clean-room implementation
//
// Read an experiment switch (GLINT_*) without tripping MSVC's C4996 deprecation of getenv(). The
// value is copied into storage this file owns, so no raw pointer into the process environment
// escapes. A variable set to an empty string reads as unset, which is what every call site assumed.

#ifndef GLINT_ENV_HPP
#define GLINT_ENV_HPP

namespace glint {

// Returns the variable's value, or nullptr when it is unset or empty. The pointer stays valid
// until the next call on the same thread: call sites copy or parse it immediately.
const char* glint_getenv(const char* name);

} // namespace glint

#endif // GLINT_ENV_HPP