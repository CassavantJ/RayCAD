// Test hooks of src/lib. Private to src/lib and its tests.
#pragma once

#include <ray/core.h>

#include <functional>
#include <string>

namespace ray::lib::detail {

// The polling interval of ILibrary::watch, 5000 ms unless a test changes it. Read when a watch thread waits.
void setWatchIntervalMs(int ms);
int watchIntervalMs();

// Called on the worker of every background root read (LibraryStack::setRootsInBackground) before the folder is read,
// with the read job's cancel token: a test makes a root slow by blocking here (until the token is cancelled). Empty
// (the default): nothing. Thread-safe.
void setBackgroundReadHook(std::function<void(const std::string& dir, const CancelToken& cancel)> hook);

} // namespace ray::lib::detail
