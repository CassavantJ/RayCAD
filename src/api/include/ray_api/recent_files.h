// The one thing the desktop start screen (src/ui, task T-019) needs from the session that IApp does not offer:
// removing an entry from the recent-files list (a file that was moved or deleted). Not one of the contract headers
// in include/ray/; a later interface revision may move it into IApp.
#pragma once

#include <ray/api.h>

#include <string>

namespace ray::api {

// UI thread. Removes `path` from the app's recent-files list (compared as the same file, case-insensitively on
// Windows), persists the list and posts AppEvent::Kind::RecentFilesChanged. False when the path is not listed or
// `app` was not made by makeApp / makeHeadlessApp.
bool forgetRecentFile(IApp& app, const std::string& path);

} // namespace ray::api
