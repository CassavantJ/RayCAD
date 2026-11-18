// Exception and structured-exception guard for kernel entry points (task T-004). See shape_impl.h.
#include "shape_impl.h"

#include <Standard_Failure.hxx>

#include <exception>
#include <new>
#include <sstream>

#if defined(_MSC_VER)
#include <windows.h>

#include <excpt.h>
#include <malloc.h>
#endif

namespace ray::kernel::detail {

namespace {

// C++ exceptions only. Returns true when thunk returned normally.
bool runCatching(GuardedThunk thunk, void* context, std::string& message) noexcept
{
    try {
        try {
            thunk(context);
            return true;
        } catch (const Standard_Failure& failure) {
            const char* text = failure.what();
            message = std::string("OpenCASCADE ") + failure.ExceptionType() +
                      ((text != nullptr && *text != '\0') ? std::string(": ") + text : std::string());
        } catch (const std::bad_alloc&) {
            message = "out of memory";
        } catch (const std::exception& failure) {
            message = failure.what();
        } catch (...) {
            message = "unknown exception";
        }
    } catch (...) {
        // Building the message itself failed (out of memory). Leave it as it is.
    }
    return false;
}

#if defined(_MSC_VER)
// A structured exception (an access violation inside OCCT, a stack overflow, a delay-loaded DLL that cannot be
// loaded) is caught here. This frame holds no C++ object with a destructor, which __try requires.
bool runSeh(GuardedThunk thunk, void* context, std::string& message, unsigned long& code) noexcept
{
    __try {
        return runCatching(thunk, context, message);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        code = GetExceptionCode();
        return false;
    }
}
#endif

} // namespace

bool runGuarded(GuardedThunk thunk, void* context, std::string& message) noexcept
{
#if defined(_MSC_VER)
    unsigned long code = 0;
    const bool ok = runSeh(thunk, context, message, code);
    if (!ok && code != 0) {
        if (code == static_cast<unsigned long>(EXCEPTION_STACK_OVERFLOW))
            (void)_resetstkoflw();
        try {
            std::ostringstream text;
            text << "structured exception 0x" << std::hex << code;
            if (code == 0xC06D007Eul || code == 0xC06D007Ful)
                text << " (an OpenCASCADE DLL or one of its functions could not be loaded)";
            message = text.str();
        } catch (...) {
        }
    }
    return ok;
#else
    return runCatching(thunk, context, message);
#endif
}

Error makeError(ErrorCode code, std::string message, std::string_view where)
{
    return Error{code, std::move(message), {std::string(where)}};
}

} // namespace ray::kernel::detail
