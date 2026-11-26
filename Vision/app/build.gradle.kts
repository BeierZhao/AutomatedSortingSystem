plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "ca.mcmaster.sorting.vision"
    compileSdk = 34

    defaultConfig {
        applicationId = "ca.mcmaster.sorting.vision"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        viewBinding = true
    }
    testOptions {
        // org.json is stubbed on the plain JVM; the real one is added below.
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.lifecycle.service)
    implementation(libs.lifecycle.runtime.ktx)
    implementation(libs.camera.core)
    implementation(libs.camera.camera2)
    implementation(libs.camera.lifecycle)
    // Bundled ML Kit models: work offline, no Play Services download on first use.
    implementation(libs.mlkit.text)
    implementation(libs.mlkit.barcode)
    implementation(libs.nanohttpd)

    testImplementation(libs.junit)
    testImplementation(libs.json)
}
