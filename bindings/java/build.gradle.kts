plugins {
    java
    `maven-publish`
    signing
}

repositories {
    mavenCentral()
}

sourceSets {
    create("jmh") {
        java.srcDir("src/jmh/java")
        compileClasspath += sourceSets.main.get().output
        runtimeClasspath += output + compileClasspath
    }
}

val jmhImplementation by configurations.getting {
    extendsFrom(configurations.implementation.get())
}

val jmhAnnotationProcessor by configurations.getting

dependencies {
    testImplementation("org.junit.jupiter:junit-jupiter-api:6.0.3")
    testRuntimeOnly("org.junit.jupiter:junit-jupiter-engine:6.0.3")
    "jmhImplementation"("org.openjdk.jmh:jmh-core:1.37")
    "jmhAnnotationProcessor"("org.openjdk.jmh:jmh-generator-annprocess:1.37")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(21))
    }
    withSourcesJar()
    withJavadocJar()
}

tasks.withType<JavaCompile> {
    options.compilerArgs.add("--enable-preview")
}

tasks.withType<Test> {
    useJUnitPlatform()
    jvmArgs("--enable-preview", "--enable-native-access=ALL-UNNAMED")
}

tasks.register<JavaExec>("jmh") {
    description = "Run JMH benchmarks"
    group = "benchmark"
    classpath = sourceSets["jmh"].runtimeClasspath
    mainClass.set("org.openjdk.jmh.Main")
    jvmArgs("--enable-preview", "--enable-native-access=ALL-UNNAMED")
}

tasks.withType<Javadoc> {
    val stdOptions = options as StandardJavadocDocletOptions
    stdOptions.addStringOption("-enable-preview", "-quiet")
    stdOptions.source = "21"
}

publishing {
    publications {
        create<MavenPublication>("mavenJava") {
            groupId = "io.tachyon"
            artifactId = "tachyon-java"
            version = "0.2.0"

            from(components["java"])

            pom {
                name.set("Tachyon IPC Java Bindings")
                description.set("Zero-copy SPSC lock-free IPC via Panama FFM")
                url.set("https://github.com/riyaneel/tachyon")

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
                    connection.set("scm:git:git://github.com/riyaneel/tachyon.git")
                    developerConnection.set("scm:git:ssh://github.com/riyaneel/tachyon.git")
                    url.set("https://github.com/riyaneel/tachyon")
                }
            }
        }
    }
}

signing {
    sign(publishing.publications["mavenJava"])
}
