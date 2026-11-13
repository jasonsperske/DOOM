pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "exerdoom"

include(":app")

// Vendored from soloton, GPL-2.0-or-later: see libs/UPSTREAM.
include(":libs:bike-core")
include(":libs:bike")
