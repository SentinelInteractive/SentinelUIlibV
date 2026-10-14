# Code style

Most of the style is enforced by tooling. This page lists which config covers
what, then the conventions that no tool can check.

## Formatting and analysis

| Scope | Config | How to run |
| --- | --- | --- |
| Whole tree | `.editorconfig`, `.gitattributes` | Read by Visual Studio, VS Code and Rider. `.gitattributes` normalizes line endings to LF on commit. |
| C++ (`native/`) | `native/.clang-format` | `clang-format -i <files>` |
| C# (`managed/`, `samples/`) | `.editorconfig` (`[*.cs]`) | `dotnet format managed/SentinelUI.sln` |
| HTML/CSS/JS (`web/`, `samples/`) | `.prettierrc.json` | `npx prettier --write <files>` |
| PowerShell (`tools/`, `samples/`) | `PSScriptAnalyzerSettings.psd1` | `Invoke-ScriptAnalyzer -Path tools -Settings ./PSScriptAnalyzerSettings.psd1` |

The formatters are configured per tree but have not been run over the whole
repository yet, so run the one that matches the tree you are touching. Line
endings: everything is stored as LF; after changing `.gitattributes`, run
`git add --renormalize .` once.

## Conventions the tools do not check

### C++ (`native/`)

- Names: types and free functions `PascalCase` (`Renderer`, `SetCaptureState`),
  locals `camelCase`, constants `kPascalCase` (`kBaseLayerId`, `kMaxLayers`),
  file-scope state `g_camelCase`, namespaces lowercase (`sui_host`). Class
  members and private state end with an underscore (`layers_`, `shmBase_`).
- Headers are `#pragma once`, C++20, MSVC, CMake. Prefer `nullptr` and
  `static_cast`; C-style casts are only for integer width conversions at API
  boundaries (Win32 parameters, printf arguments).
- Game-thread rules (the failure ladder): no exceptions across hooks, no heap
  allocation in the Present path, no unbounded waits.
- `extern` state is defined in one place and re-declared next to its users with
  a comment saying where it lives.

### C# (`managed/`, `samples/`)

- net48, x64, `LangVersion 9`, nullable disabled. Keep it that way unless the
  whole solution moves.
- API entry points never throw at plugin authors: return `null` or `false`, and
  log.
- Handlers fire from `Sui.Pump()` on the game thread; game natives are legal
  there and only there.
- Naming is set in `.editorconfig`: private fields `_camelCase`, constants
  `PascalCase`, everything else `PascalCase` for types and members.

### PowerShell (`tools/`, `samples/`)

- `Verb-Noun` function names using approved verbs (`Get-GtaProcess`,
  `Set-GtaForeground`), `PascalCase` parameters, `camelCase` locals.
- `param()` block first. Scripts must be re-runnable, and deploy scripts refuse
  to run while the game is open.
- Fail with `throw` and a message that names the missing thing.

### Web (`web/`, `samples/`)

- Plain HTML/CSS/JS: no frameworks and no build step.
- Runtime pages are ES5-style (`var`, function declarations). Match the file you
  are editing and do not mix `var` with `const`/`let` in one file.
- CSS classes are lowercase (`.brand`), custom properties `--kebab-case`, JS and
  DOM ids `camelCase`.
- Runtime constraints are not negotiable: scheme-served pages load JS/CSS as
  separate files (CSP `script-src 'self'`), fetch/XHR does not work on
  `sentinelui://`, and pages talk through the injected `window.sui` only.
- Click-through layers stay non-interactive; interactive pages must be keyboard
  reachable with a visible focus state.

### Files

`lowercase.ext` for web files, `PascalCase.cs` for C# types, `snake_case.cpp`
and `snake_case.h` for native.

## Comments

- Explain why, not what. The field notes in this repo (dates, hardware,
  protocol gotchas) are its most valuable comments: keep them and keep them
  accurate.
- No banner comments, ASCII rulers, emoji or end-of-block markers, and no
  commented-out code.
- A `TODO` states what, why and what unblocks it.
- File headers: one short block, and only when the file's job is not obvious
  from its name.
