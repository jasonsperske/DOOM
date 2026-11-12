// SPDX-License-Identifier: GPL-2.0-or-later
// Vendored from soloton (libs/bike, published there as bike-android); see libs/UPSTREAM. The sources
// are upstream's as they are; this build file replaces upstream's, which uses soloton's convention
// plugins and Maven publishing.
plugins {
    id("com.android.library")
}

android {
    namespace = "dev.soloton.bike"
    compileSdk = 36
    defaultConfig {
        minSdk = 24
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    api(project(":libs:bike-core"))

    // Instrumented tests against stand-in Peloton services: ./gradlew :libs:bike:connectedDebugAndroidTest
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test:rules:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
}
