# Building NullFrame Core

Requires JDK 21. Core is a standalone Gradle Java library; it does not depend
on Myelin, Fabric Loom or the old combined workspace.

```powershell
.\gradlew.bat build publishToMavenLocal
```

This creates `build/libs/nullframe-core-<version>.jar` and publishes the library,
sources and Maven metadata to your local Maven repository as
`com.nullframe:nullframe-core:<version>`, where `<version>` is the version in
`build.gradle.kts`. Myelin can then resolve that version
without including Core as a subproject.

On Linux/macOS, use `./gradlew` instead (run `chmod +x gradlew` if needed).

## Native library

The normal Gradle build packages the native binaries already present in
`src/main/resources/native`. It does not silently rebuild them.

To rebuild the native bridge from C source, install CMake, a compatible C
compiler and JDK 21, then run:

```powershell
.\gradlew.bat buildNativeWithCMake
.\gradlew.bat build publishToMavenLocal
```

Set `JAVA_HOME` to your JDK when configuring CMake. The native build requires
AVX2 support and produces the library for the current platform; building on
Windows does not rebuild the bundled Linux binary or vice versa.

The native task copies the result into `src/main/resources/native` before
the second command packages and publishes it. Do not commit local build
directories. The Gradle wrapper, C sources, licence files and required
packaged native resources belong to the project.
