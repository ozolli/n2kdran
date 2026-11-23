plugins {
    id("com.android.application")
}

android {
    namespace = "oz.n2kdran"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "oz.n2kdran"
        minSdk = 33                      // Android 13 (SailProof SP10X)
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += "arm64-v8a" } // MediaTek MT6877
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    // Clé de version hors dépôt : chemin et mot de passe dans ~/.gradle/gradle.properties
    // (N2KDRAN_KEYSTORE, N2KDRAN_KEY_ALIAS, N2KDRAN_KEY_PASSWORD). Sans eux, APK de version non signé.
    val ks = providers.gradleProperty("N2KDRAN_KEYSTORE").orNull
    signingConfigs {
        if (ks != null) create("release") {
            storeFile = file(ks)
            storePassword = providers.gradleProperty("N2KDRAN_KEY_PASSWORD").get()
            keyAlias = providers.gradleProperty("N2KDRAN_KEY_ALIAS").get()
            keyPassword = storePassword
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (ks != null) signingConfig = signingConfigs.getByName("release")
        }
    }
}

// APK à partager : ./gradlew distApk → app/build/dist/n2kdran-<versionName>.apk
tasks.register<Copy>("distApk") {
    dependsOn("assembleRelease")
    from(layout.buildDirectory.file("outputs/apk/release/app-release.apk"))
    into(layout.buildDirectory.dir("dist"))
    rename { "n2kdran-${android.defaultConfig.versionName}.apk" }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")
}
