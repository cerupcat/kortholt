plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
    alias(libs.plugins.ksp)
    id("dagger.hilt.android.plugin")
    id("maven-publish")
    id("signing")
}

android {
    namespace = "net.simno.kortholt"
    compileSdk = 35
    ndkVersion = "28.1.13356709"
    defaultConfig {
        minSdk = 30
        externalNativeBuild {
            cmake {
                cppFlags("-std=c++14")
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
                )
                // For debug builds, only build arm64-v8a to save ~2-3 minutes
                // Most modern devices/emulators use arm64-v8a
                abiFilters("arm64-v8a")
            }
        }
    }

    buildTypes {
        getByName("release") {
            externalNativeBuild {
                cmake {
                    // Release builds include all ABIs for maximum device compatibility
                    abiFilters("armeabi-v7a", "arm64-v8a", "x86", "x86_64")
                }
            }
        }
    }
    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/cpp/libpd/java")
        }
    }
    externalNativeBuild {
        cmake {
            path("src/main/cpp/CMakeLists.txt")
        }
    }
    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
    lint {
        warningsAsErrors = true
        abortOnError = true
    }
    publishing {
        singleVariant("release") {
            withSourcesJar()
            withJavadocJar()
        }
        multipleVariants {
            withSourcesJar()
            withJavadocJar()
            allVariants()
        }
    }
}

dependencies {
    api("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    api("androidx.annotation:annotation:1.9.1")
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("com.getkeepsafe.relinker:relinker:1.4.5")
    implementation("net.lingala.zip4j:zip4j:2.11.5")

    // Hilt dependency injection - migrated from KAPT to KSP for faster builds
    implementation("com.google.dagger:hilt-android:2.51.1")
    add("ksp", "com.google.dagger:hilt-compiler:2.51.1")
}

val siteUrl = "https://github.com/simonnorberg/kortholt"
val gitUrl = "https://github.com/simonnorberg/kortholt.git"

version = "3.4.0"
group = "net.simno.kortholt"

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])
                groupId = "net.simno.kortholt"
                artifactId = "kortholt"
                version = "3.4.0"
                pom {
                    name.set("kortholt")
                    url.set(siteUrl)
                    description.set("Pure Data for Android with libpd and Oboe.")
                    licenses {
                        license {
                            name.set("Apache-2.0")
                            url.set("http://www.apache.org/licenses/LICENSE-2.0.txt")
                        }
                    }
                    scm {
                        connection.set(gitUrl)
                        developerConnection.set(gitUrl)
                        url.set(siteUrl)
                    }
                    developers {
                        developer {
                            id.set("simonnorberg")
                            name.set("Simon Norberg")
                        }
                    }
                }
            }
        }
    }
}

tasks.register<Jar>("sourcesJar") {
    from(android.sourceSets["main"].java.srcDirs)
    archiveClassifier.set("sources")
}

tasks.register<Javadoc>("javadoc") {
    exclude("**/*.kt")
    source = android.sourceSets["main"].java.getSourceFiles()
    classpath += files(android.bootClasspath.joinToString(File.pathSeparator))
}

tasks.register<Jar>("javadocJar") {
    dependsOn("javadoc")
    archiveClassifier.set("javadoc")
    from(tasks["javadoc"].outputs.files)
}

artifacts {
    archives(tasks["javadocJar"])
    archives(tasks["sourcesJar"])
}

signing {
    val signingKey = rootProject.properties["SIGNING_KEY"]?.toString().orEmpty()
    val signingPassword = rootProject.properties["SIGNING_PASSWORD"]?.toString().orEmpty()
    useInMemoryPgpKeys(signingKey, signingPassword)
    sign(publishing.publications)
}
