plugins {
    `java-library`
    `maven-publish`
}

group = "com.nullframe"
version = "0.1.2"

repositories {
    mavenCentral()
}

dependencies {
    // log4j comes from the host (Minecraft) at runtime — compile-only here.
    compileOnly("org.apache.logging.log4j:log4j-api:2.20.0")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(21))
    }
    withSourcesJar()
}

publishing {
    publications {
        create<MavenPublication>("mavenJava") {
            from(components["java"])
            artifactId = "nullframe-core"
        }
    }
}

val coreLicenseFiles = listOf("COPYING", "COPYING.LESSER")
tasks.named<Jar>("jar") {
    from(coreLicenseFiles)
    from("licenses") { into("licenses") }
}
tasks.named<Jar>("sourcesJar") {
    from(coreLicenseFiles)
    from("licenses") { into("licenses") }
}

tasks.withType<JavaCompile>().configureEach {
    options.release.set(21)
}

// ── Native (cubiomes) build — CMake drives this; the DLL/.so lands in
// src/main/resources/native and is packaged into the core jar. ──
val nativeBuildDir = layout.buildDirectory.dir("native").get().asFile
val nativeOutputDir = file("src/main/resources/native")

tasks.register("prepareNativeOutputDir") {
    group = "native"
    description = "Creates native output directory in resources."
    doLast {
        nativeOutputDir.mkdirs()
    }
}

tasks.register<Exec>("configureNativeWithCMake") {
    group = "native"
    description = "Configures JNI native bridge with CMake."
    dependsOn("prepareNativeOutputDir")
    doFirst {
        nativeBuildDir.mkdirs()
    }
    commandLine("cmake", "-S", ".", "-B", nativeBuildDir.absolutePath)
}

tasks.register<Exec>("buildNativeWithCMake") {
    group = "native"
    description = "Builds JNI native bridge with CMake."
    dependsOn("configureNativeWithCMake")
    commandLine("cmake", "--build", nativeBuildDir.absolutePath, "--config", "Release")
}
