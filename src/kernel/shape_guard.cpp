// Exception and structured-exception guard for kernel entry points (task T-004). See shape_impl.h.
#include "shape_impl.h"

#include <Standard_Failure.hxx>

#include <atomic>
#include <cstdint>
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

std::atomic<std::uint64_t> gStructuredExceptions{0};

#if defined(_MSC_VER)
// Faults are counted where they are raised, not where they are caught: OCCT also swallows them itself. Its thread
// pool runs every job under catch (...), and OCCT is built with /EHa, so an access violation in a parallel mesh job
// becomes "Error: Unknown exception" and never reaches runSeh(). A first-chance vectored handler sees every one of
// them, on any thread, before any handler runs; it only counts and lets the search continue.
bool isFault(DWORD code) noexcept
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return true;
    default:
        return false;
    }
}

LONG CALLBACK countFault(EXCEPTION_POINTERS* info) noexcept
{
    if (info != nullptr && info->ExceptionRecord != nullptr && isFault(info->ExceptionRecord->ExceptionCode))
        gStructuredExceptions.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_SEARCH;
}

void installFaultCounter() noexcept
{
    static const bool installed = AddVectoredExceptionHandler(1, countFault) != nullptr;
    (void)installed;
}

// Installed when the kernel is loaded (and again, harmlessly, on first use).
const bool gFaultCounterAtLoad = (installFaultCounter(), true);

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
    installFaultCounter();
    unsigned long code = 0;
    const bool ok = runSeh(thunk, context, message, code);
    if (!ok && code != 0) {
        // A fault was counted by countFault() when it was raised; anything else (a DLL that cannot be loaded) here.
        if (!isFault(code))
            gStructuredExceptions.fetch_add(1, std::memory_order_relaxed);
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

std::uint64_t structuredExceptionCount() noexcept
{
    return gStructuredExceptions.load(std::memory_order_relaxed);
}

Error makeError(ErrorCode code, std::string message, std::string_view where)
{
    return Error{code, std::move(message), {std::string(where)}};
}

} // namespace ray::kernel::detail
