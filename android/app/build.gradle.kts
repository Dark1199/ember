plugins {
    id("com.android.application")
}

android {
    namespace = "org.ember.engine"
    compileSdk = 34

    defaultConfig {
        applicationId = "org.ember.engine"
        // Android 10: Vulkan 1.1 is guaranteed on devices that declare it.
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.0.1"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_STL=c++_static") }
        }
    }
    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildTypes {
        release { isMinifyEnabled = false }
    }
}
