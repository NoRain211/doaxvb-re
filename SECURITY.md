# Security Policy

## Scope

This project is a native Windows port whose public runtime loads a locally
prepared game payload. This policy covers the tracked source, build and import
tools, release workflows, and public builds in this repository. The runtime
executes the locally prepared game code in its own process; it is not an
operating-system sandbox.

Security reports are especially useful when they show a reachable problem in
file or image parsing, guest-memory access, dispatch, host adapters, save-file
handling, or the authentication checks for local generated inputs. The project
does not operate a network service.

## Reporting a vulnerability

Do not report security vulnerabilities in a public issue or pull request. If
GitHub shows **Report a vulnerability** on this repository's Security page,
use that private reporting channel. GitHub only makes private vulnerability
reporting available when the repository has enabled it. If it is unavailable,
contact the maintainer privately through the contact options on
[GitHub](https://github.com/NoRain211), without posting vulnerability details
publicly.

Include the affected commit or release, relevant build configuration, impact,
reproduction steps, and a minimal proof of concept when safe. Do not attach
game executables, generated game source, extracted assets or filenames, saves,
BIOS data, private paths, or run evidence. Share only safe metadata and the
observed behavior.

Please allow maintainers time to investigate and prepare a fix before public
disclosure. No response or remediation timeframe is promised.

## Security expectations

- Treat file contents, command-line values, and guest-controlled state as
  untrusted when they cross into host code.
- Keep guest-memory accesses within guest memory and validate sizes before
  parsing or allocating from input data.
- Keep file operations within their intended game or save roots.
- Preserve fail-closed checks for authenticated generated sources and program
  manifests.
- Do not weaken these boundaries to make a malformed or unauthenticated input
  appear to work.

## Known issues

- The lifter's standalone section extractor,
  `tools/xboxrecomp/tools/xbe_parser/xbe_parser.py --extract-sections`, builds
  each output file name from the section name stored in the XBE. It replaces
  only `$` and `.`, so an absolute name such as `C:\x` writes `C:\x.bin`
  outside the chosen directory. Run it only on XBE files you trust. The
  supported build does not call it: `BuildGame` accepts only the verified XBE
  hash and does not extract sections. The fix belongs on the
  `codex/doaxbv-recipe` branch of `NoRain211/xboxrecomp`: name files by
  section index or address and check that each resolved path stays inside the
  output directory. Move the pin with `tools/update_lifter_pin.py`.
