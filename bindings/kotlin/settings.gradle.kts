rootProject.name = "tachyon-kotlin"

includeBuild("../java") {
    dependencySubstitution {
        substitute(module("dev.tachyon-ipc:tachyon-java")).using(project(":"))
    }
}
