import com.android.build.api.variant.ApplicationAndroidComponentsExtension

plugins {
    id("com.android.application")
}

android {
    namespace = "dev.soloton.doom"
    compileSdk = 36
    // The gen 1 Peloton tablet NDK and CMake; install with
    // sdkmanager "ndk;29.0.14206865" "cmake;3.31.6"
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "dev.soloton.doom"
        minSdk = 24 // Android 7.0, the gen 1 Peloton bike
        targetSdk = 35
        versionCode = providers.environmentVariable("DOOM_VERSION_CODE").map { it.toInt() }.getOrElse(1)
        versionName = providers.environmentVariable("DOOM_VERSION_NAME").getOrElse("0.1.0-dev")

        ndk {
            // Bike: arm64-v8a (MT8173), armeabi-v7a as a fallback. x86_64 is for the emulator.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=none"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Sideloaded: sign with the debug key until a release key is set up.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        disable += setOf("ExpiredTargetSdkVersion", "OldTargetApi", "GradleDependency", "AndroidGradlePluginVersion")
    }
}

dependencies {
    implementation(project(":libs:bike"))
}

// The shareware IWAD, packed into the APK when there is one to pack: DOOM1.WAD (or doom1.wad) at
// the repository root, or any file given with -PembedWad=/path/to/doom1.wad. WADs are not in
// git, so a build without one still works and the app asks for a WAD as before. The app copies
// the embedded one to its own storage on first run if no other IWAD is installed.
abstract class EmbedWadTask : DefaultTask() {
    @get:org.gradle.api.tasks.InputFiles
    @get:org.gradle.api.tasks.PathSensitive(org.gradle.api.tasks.PathSensitivity.NONE)
    abstract val wad: ConfigurableFileCollection

    @get:org.gradle.api.tasks.OutputDirectory
    abstract val outputDir: DirectoryProperty

    @org.gradle.api.tasks.TaskAction
    fun copy() {
        val out = outputDir.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        wad.files.firstOrNull { it.isFile }?.copyTo(File(out, "doom1.wad"))
    }
}

val embeddedWad = providers.gradleProperty("embedWad").map { listOf(file(it)) }
    .orElse(listOf(rootProject.file("../DOOM1.WAD"), rootProject.file("../doom1.wad")))

val embedWad = tasks.register<EmbedWadTask>("embedWad") {
    description = "Copies the shareware IWAD, if present, into the APK's assets."
    wad.from(embeddedWad)
    outputDir.set(layout.buildDirectory.dir("generated/wadAssets"))
}

extensions.configure<ApplicationAndroidComponentsExtension> {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(embedWad, EmbedWadTask::outputDir)
    }
}
