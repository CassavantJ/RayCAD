// Each module stub reports the modules it links. This test links every headless module through ray_api and
// compares those reports with the "may depend on" column of PLAN.md section 1.1.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
std::vector<const char*> core_links();
const char* kernel_module() noexcept;
std::vector<const char*> kernel_links();
const char* naming_module() noexcept;
std::vector<const char*> naming_links();
const char* sketch_module() noexcept;
std::vector<const char*> sketch_links();
const char* lib_module() noexcept;
std::vector<const char*> lib_links();
const char* cam_module() noexcept;
std::vector<const char*> cam_links();
const char* doc_module() noexcept;
std::vector<const char*> doc_links();
const char* store_module() noexcept;
std::vector<const char*> store_links();
const char* sim_module() noexcept;
std::vector<const char*> sim_links();
const char* post_module() noexcept;
std::vector<const char*> post_links();
const char* api_module() noexcept;
std::vector<const char*> api_links();
} // namespace ray::detail

namespace {
void check(const std::string& expectedName, const char* name, const std::vector<const char*>& links,
           const std::vector<std::string>& expectedLinks)
{
    INFO("module " << expectedName);
    REQUIRE(std::string(name) == expectedName);
    const std::vector<std::string> actual(links.begin(), links.end());
    REQUIRE(actual == expectedLinks);
}
} // namespace

TEST_CASE("every headless module links and reports the dependencies PLAN.md 1.1 allows", "[modules]")
{
    check("core", ray::detail::core_module(), ray::detail::core_links(), {});
    check("kernel", ray::detail::kernel_module(), ray::detail::kernel_links(), {"core"});
    check("naming", ray::detail::naming_module(), ray::detail::naming_links(), {"core", "kernel"});
    check("sketch", ray::detail::sketch_module(), ray::detail::sketch_links(), {"core", "kernel"});
    check("lib", ray::detail::lib_module(), ray::detail::lib_links(), {"core"});
    check("cam", ray::detail::cam_module(), ray::detail::cam_links(), {"core", "kernel", "naming", "lib"});
    check("doc", ray::detail::doc_module(), ray::detail::doc_links(), {"core", "kernel", "naming", "sketch", "cam"});
    check("store", ray::detail::store_module(), ray::detail::store_links(), {"core", "doc"});
    check("sim", ray::detail::sim_module(), ray::detail::sim_links(), {"core", "kernel", "cam", "lib"});
    check("post", ray::detail::post_module(), ray::detail::post_links(), {"core", "cam", "lib"});
    check("api", ray::detail::api_module(), ray::detail::api_links(),
          {"core", "kernel", "naming", "sketch", "lib", "doc", "store", "cam", "sim", "post"});
}
