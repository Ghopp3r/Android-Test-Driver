import java.io.ByteArrayOutputStream

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// This shared development key is only suitable for the internal test APK.
val keystoreDir = rootProject.file("keystore")
val keystoreFile = File(keystoreDir, "debug.jks")
val keystorePass = "android"
val keyAliasName = "debug"
val keyAliasPass = "android"

fun ensureKeystore() {
    if (keystoreFile.exists()) return
    if (!keystoreDir.exists()) keystoreDir.mkdirs()
    val javaHome = System.getProperty("java.home")
    val keytool = File(javaHome, if (System.getProperty("os.name").lowercase().contains("win")) "bin/keytool.exe" else "bin/keytool")
    val cmd = listOf(
        keytool.absolutePath,
        "-genkeypair", "-alias", keyAliasName,
        "-keyalg", "RSA", "-keysize", "2048",
        "-validity", "10000",
        "-storepass", keystorePass, "-keypass", keyAliasPass,
        "-dname", "CN=MyDriverTest, O=Dev, L=Local, C=US",
        "-keystore", keystoreFile.absolutePath
    )
    val proc = ProcessBuilder(cmd).redirectErrorStream(true).start()
    val out = ByteArrayOutputStream()
    proc.inputStream.copyTo(out)
    val rc = proc.waitFor()
    if (rc != 0) throw GradleException("keytool failed rc=$rc\n${out.toString(Charsets.UTF_8)}")
    println("[MyDriverTest] Generated fresh keystore at ${keystoreFile.absolutePath}")
}

android {
    namespace = "com.mydriver.test"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.mydriver.test"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

    }

    signingConfigs {
        create("dev") {
            ensureKeystore()
            storeFile = keystoreFile
            storePassword = keystorePass
            keyAlias = keyAliasName
            keyPassword = keyAliasPass
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".reportcheck"
            signingConfig = signingConfigs.getByName("dev")
            isMinifyEnabled = false
        }
        release {
            signingConfig = signingConfigs.getByName("dev")
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    sourceSets.getByName("androidTest").java.srcDir("src/test/java")

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("com.google.android.material:material:1.12.0")
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}
