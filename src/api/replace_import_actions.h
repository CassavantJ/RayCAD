// The action file.replaceImport (task T-084, replace_import.cpp; ray_api/replace_import.h describes it).
#pragma once

#include <ray/core.h>

namespace ray::api::detail {

class AppImpl;
Status registerReplaceImportActions(AppImpl& app);

} // namespace ray::api::detail
