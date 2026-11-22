// Test hooks of src/lib. Private to src/lib and its tests.
#pragma once

namespace ray::lib::detail {

// The polling interval of ILibrary::watch, 5000 ms unless a test changes it. Read when a watch thread waits.
void setWatchIntervalMs(int ms);
int watchIntervalMs();

} // namespace ray::lib::detail
