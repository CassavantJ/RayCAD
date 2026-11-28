// Private to src/naming and its tests (T-081b round 8). Not a public interface. Includes no kernel or OCCT header.
// Hooks for the resolver's mutation checks: a pin of the post-condition must fail with it switched off.
#pragma once

namespace ray::naming::testing {

// Switches the resolver's post-condition (postcondition.h) for every later resolve() in the process. Default: on, or
// off from the start when the environment variable RAYCAD_NAMING_NO_POSTCONDITION is set (read once, at the first
// resolve). Never call it while another thread may be resolving.
void setPostconditionEnabled(bool on) noexcept;
bool postconditionEnabled() noexcept;

// T-081b round 9, one switch per mechanism, each on by default, for the pins' mutation checks (never call while another
// thread may be resolving or making references):
//  - Generator identity (naming.h Resolved::identity): off, no answer is Exact by identity (the post-condition decides
//    every answer by its samples, as in round 8).
//  - Pick-time grounds through ancestor records (ElementMap::Impl::ancestors): off, a split piece's owner and
//    ground are found from the records the map holds only, as in round 8 (references made while it is off).
//  - Body moves (NamingInput::bodyPlacement): off, the post-condition ignores them, as if no body had a placement.
void setGeneratorIdentityEnabled(bool on) noexcept;
bool generatorIdentityEnabled() noexcept;
void setAncestorsEnabled(bool on) noexcept;
bool ancestorsEnabled() noexcept;
void setBodyMovesEnabled(bool on) noexcept;
bool bodyMovesEnabled() noexcept;

// T-081b round 10, the same kind of switches:
//  - Each of generator identity's three refusals (postcondition.cpp, refusedIdentity): off, that refusal does not
//    refuse, and identity (generator or derived) decides where it would have. Default on, or off from the start for
//    those the environment variable RAYCAD_NAMING_NO_REFUSAL lists (1 LandsElsewhere, 2 EndAgainstSense, 3
//    OnPickSurfaces, comma-separated; read once, for diagnosis).
//  - Derived identity (naming.h Resolved::derived): off, no answer is Exact by derived identity.
//  - Face moves (NamingInput::faceMoves): off, the post-condition ignores them, as if no direct edit said how it moved
//    its faces (and references made while it is off record none).
enum class IdentityRefusal { LandsElsewhere, EndAgainstSense, OnPickSurfaces };
void setIdentityRefusalEnabled(IdentityRefusal which, bool on) noexcept;
bool identityRefusalEnabled(IdentityRefusal which) noexcept;
void setDerivedIdentityEnabled(bool on) noexcept;
bool derivedIdentityEnabled() noexcept;
void setFaceMovesEnabled(bool on) noexcept;
bool faceMovesEnabled() noexcept;

} // namespace ray::naming::testing
