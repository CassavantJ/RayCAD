# Contributing to RayCAD

## Licence of contributions: DCO, no CLA

RayCAD is licensed under LGPL-2.1-or-later. There is no contributor licence agreement. Instead, every
commit must be signed off under the [Developer Certificate of Origin 1.1](https://developercertificate.org/):
by adding a `Signed-off-by` line you state that you wrote the change, or otherwise have the right to submit
it, under the project's licence.

```
Signed-off-by: Your Name <you@example.com>
```

`git commit -s` adds the line for you. Use your real name and an address you can be reached at.

The text of the certificate:

> Developer Certificate of Origin, Version 1.1
>
> By making a contribution to this project, I certify that:
>
> (a) The contribution was created in whole or in part by me and I have the right to submit it under the
> open source license indicated in the file; or
>
> (b) The contribution is based upon previous work that, to the best of my knowledge, is covered under an
> appropriate open source license and I have the right under that license to submit that work with
> modifications, whether created in whole or in part by me, under the same open source license (unless I
> am permitted to submit under a different license), as indicated in the file; or
>
> (c) The contribution was provided directly to me by some other person who certified (a), (b) or (c) and
> I have not modified it.
>
> (d) I understand and agree that this project and the contribution are public and that a record of the
> contribution (including all personal information I submit with it, including my sign-off) is maintained
> indefinitely and may be redistributed consistent with this project or the open source license(s)
> involved.

## What may not be contributed

- No code, assets, icons or file formats copied from any commercial CAD/CAM product.
- Code adapted from FreeCAD is welcome only where its licence is LGPL-2.1-or-later. Keep the original
  attribution in the file header and add the source to `THIRD_PARTY.md`.
- A new dependency needs a row in `THIRD_PARTY.md` in the same change. GPL-only dependencies cannot be used.

## Rules for code

`PLAN.md` section 0.4 lists the standing rules. The ones the tools check on every pull request:

1. Only `src/kernel` includes OpenCASCADE headers. Only `src/ui` and `src/render` include Qt headers.
2. `src/cam`, `src/post`, `src/sim`, `src/kernel`, `src/naming`, `src/doc` and `src/store` never link
   `src/render` or `src/ui`. A module links only the modules `PLAN.md` section 1.1 allows.
3. Deterministic code (`src/cam`, `src/post`, `src/naming`, `src/sketch`, `src/sim`) calls `ray::dmath`,
   never `<cmath>` trigonometric or exponential functions.
4. No colour literals in UI or viewport code; colours live in `resources/themes/`.

Others the reviewer checks: no exception crosses a module boundary (return `ray::Result<T>`); anything
that can take more than 100 ms runs on a worker and honours cancellation; every user-visible string goes
through `qsTr()` or `ray::tr()`; every user-visible document change is a `doc::Command`.

The headers in `include/ray/` are the contracts between modules. A change to one of them is its own
change, agreed with the maintainer first.

## Before you open a pull request

```sh
python tools/run_lints.py            # the lint tools and their self-tests
python tools/check_format.py --fix   # clang-format 22, style in .clang-format
ctest --preset <your preset>         # all tests
```

See `README.md` for how to build.
