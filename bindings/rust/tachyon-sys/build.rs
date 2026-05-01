use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let core_dir = {
        let vendored = manifest_dir.join("vendor/core");
        if vendored.exists() {
            vendored
        } else {
            manifest_dir.join("../../../core")
        }
    };

    let include_dir = core_dir.join("include");
    let src_dir = core_dir.join("src");

    cc::Build::new()
        .cpp(true)
        .opt_level(3)
        .flag("-std=c++23")
        .flag("-march=native")
        .flag("-mtune=native")
        .flag("-fno-exceptions")
        .flag("-fno-rtti")
        .flag("-fPIC")
        .flag("-fvisibility=hidden")
        .flag("-Wall")
        .flag("-Wextra")
        .include(&include_dir)
        .file(src_dir.join("arena.cpp"))
        .file(src_dir.join("shm.cpp"))
        .file(src_dir.join("tachyon_c.cpp"))
        .file(src_dir.join("tachyon_rpc.cpp"))
        .file(src_dir.join("transport_uds.cpp"))
        .compile("tachyon");

    if std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default() == "linux" {
        println!("cargo:rustc-link-lib=rt")
    }

    for src in &[
        "arena.cpp",
        "shm.cpp",
        "tachyon_c.cpp",
        "tachyon_rpc.cpp",
        "transport_uds.cpp",
    ] {
        println!("cargo:rerun-if-changed={}", src_dir.join(src).display())
    }

    for hdr in &[
        "tachyon.h",
        "tachyon.hpp",
        "tachyon/arena.hpp",
        "tachyon/shm.hpp",
        "tachyon/transport.hpp",
    ] {
        println!("cargo:rerun-if-changed={}", include_dir.join(hdr).display());
    }

    let bindings = bindgen::Builder::default()
        .header(include_dir.join("tachyon.h").to_str().unwrap())
        .clang_arg(format!("-I{}", include_dir.display()))
        .allowlist_type("tachyon_.*")
        .allowlist_function("tachyon_.*")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("Failed to generate tachyon bindings");

    let out_path = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Failed to write bindings");
}
