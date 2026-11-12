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
