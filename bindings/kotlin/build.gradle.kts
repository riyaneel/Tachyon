import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import com.vanniktech.maven.publish.SonatypeHost

plugins {
    kotlin("jvm") version "2.2.21"
    id("com.vanniktech.maven.publish") version "0.28.0"
}

repositories {
    mavenCentral()
}

dependencies {
    implementation("dev.tachyon-ipc:tachyon-java:${project.version}")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
    testImplementation(kotlin("test"))
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.10.2")
    testImplementation("app.cash.turbine:turbine:1.2.1")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(21))
    }

    withSourcesJar()
    withJavadocJar()
}

tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile> {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_21)
        freeCompilerArgs.addAll("-Xjvm-default=all")
    }
}

tasks.withType<JavaCompile> {
    options.compilerArgs.add("--enable-preview")
}

tasks.withType<Test> {
    useJUnitPlatform()
    jvmArgs("--enable-preview", "--enable-native-access=ALL-UNNAMED")
}

mavenPublishing {
    coordinates("dev.tachyon-ipc", "tachyon-kotlin", project.version.toString())

    pom {
        name.set("Tachyon IPC Kotlin Bindings")
        description.set("Coroutines and Flow wrappers for Tachyon IPC (Panama FFM)")
        inceptionYear.set("2026")
        url.set("https://github.com/riyaneel/Tachyon")

        licenses {
            license {
                name.set("The Apache License, Version 2.0")
                url.set("http://www.apache.org/licenses/LICENSE-2.0.txt")
            }
        }

        developers {
            developer {
                id.set("riyaneel")
                name.set("Riyane El Qoqui")
            }
        }

        scm {
            connection.set("scm:git:git://github.com/riyaneel/Tachyon.git")
            developerConnection.set("scm:git:ssh://github.com/riyaneel/Tachyon.git")
            url.set("https://github.com/riyaneel/Tachyon")
        }
    }

    publishToMavenCentral(SonatypeHost.CENTRAL_PORTAL)
    signAllPublications()
}
