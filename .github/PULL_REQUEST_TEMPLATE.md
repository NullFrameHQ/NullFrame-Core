## Summary

What does this change do, and why is it needed?

## Related issues

Fixes #

## How this was tested

- [ ] `./gradlew build publishToMavenLocal` passes (or `gradlew.bat` on Windows)
- [ ] Myelin still builds and runs against this change
- [ ] Native code changed: rebuilt with `buildNativeWithCMake`, and the platform I built on is noted below
- [ ] Native code changed: the output is identical on the scalar, AVX2 and (if available) AVX-512 paths

The Windows and Linux binaries are built separately. If you could only rebuild one, say so here.

Describe the setup you used (OS, CPU, Java version) when it is relevant.

## Output and API

- [ ] This change does not alter generated terrain or biome output
- [ ] This change alters generated output, and the difference is described above
- [ ] This change does not alter the public Java API
- [ ] This change alters the public Java API, and the change is described above

## Checklist

- [ ] The change is limited to what this PR describes
- [ ] New source files include the LGPL-3.0 license header
- [ ] Any third-party code has its license added under `licenses/`
- [ ] Build directories and local files are not committed
- [ ] README or BUILDING.md is updated if behavior or build steps changed
