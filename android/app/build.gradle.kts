// VTesse for Android: VTun client with a local proxy, no VpnService (#13).
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Version from include/vtesse/version.h ("MAJOR.MM" -> versionCode MAJOR*100+MM).
val vtesseVersion: String = Regex("#define VTESSE_VERSION \"([0-9.]+)\"")
    .find(rootProject.file("../include/vtesse/version.h").readText())!!.groupValues[1]

// Release signing from android/keystore.properties (not in the repository) or env.
val keystoreProps = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
fun signingValue(key: String, env: String): String? = keystoreProps.getProperty(key) ?: System.getenv(env)

android {
    namespace = "org.openipc.vtesse"
    compileSdk = 35
    ndkVersion = "27.2.12479018"
    buildToolsVersion = "35.0.0"

    defaultConfig {
        applicationId = "org.openipc.vtesse"
        minSdk = 21
        targetSdk = 35
        versionName = vtesseVersion
        versionCode = vtesseVersion.split(".").let { it[0].toInt() * 100 + it[1].toInt() }
        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86", "x86_64")
        }
    }

    signingConfigs {
        create("release") {
            val store = signingValue("storeFile", "VTESSE_KEYSTORE")
            if (store != null) {
                storeFile = file(store)
                storePassword = signingValue("storePassword", "VTESSE_KEYSTORE_PASSWORD")
                keyAlias = signingValue("keyAlias", "VTESSE_KEY_ALIAS")
                keyPassword = signingValue("keyPassword", "VTESSE_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (signingValue("storeFile", "VTESSE_KEYSTORE") != null)
                signingConfig = signingConfigs.getByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
