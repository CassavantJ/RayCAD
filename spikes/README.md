# Spikes (milestone M0)

Throwaway experiments that answer one question each. Definitions are in `PLAN.md` section 13, "M0: spikes".
Nothing here is product code and nothing here is reused by copy; findings feed back into `PLAN.md`.

Rules for every spike:

- Code lives only in `spikes/<id>/`. The result is `spikes/<id>/REPORT.md`.
- Third-party sources are fetched into `spikes/<id>/third_party/` (ignored by git) at a pinned tag or commit,
  which the report records. Only open-source code with a licence listed in `PLAN.md` 2.4.
- Build on Windows with `spikes\build.cmd <id>` (MSVC 14.51 from Visual Studio 2026, CMake and Ninja that ship with it).
- A report states the question, the method, the measured numbers with the machine they were measured on,
  what was not measured, and a recommendation. Guesses are labelled as guesses.

Machine used for the first run: Core i9-13900K (24 cores), 64 GB RAM, RTX 3090 and Intel UHD 770, Windows 11.
This is a High-tier machine. Low-tier numbers from it are estimates only.
