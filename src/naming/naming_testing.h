// Private to src/naming and its tests (T-081b round 8). Not a public interface. Includes no kernel or OCCT header.
// Hooks for the resolver's mutation checks: a pin of the post-condition must fail with it switched off.
#pragma once

namespace ray::naming::testing {

// Switches the resolver's post-condition (postcondition.h) for every later resolve() in the process. Default: on, or
// off from the start when the environment variable RAYCAD_NAMING_NO_POSTCONDITION is set (read once, at the first
// resolve). Never call it while another thread may be resolving.
void setPostconditionEnabled(bool on) noexcept;
bool postconditionEnabled() noexcept;

} // namespace ray::naming::testing
