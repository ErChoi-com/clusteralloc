//! Builds the shared C (alloc/alloc.c, bench/pipeline.c) for bare metal with
//! gcc and ar, and links it in.

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

/// No libc, no SSE (vector state is never saved), no red zone (interrupts
/// run on the current stack).
const CFLAGS: &[&str] = &[
    "-O2",
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-ffreestanding",
    "-fno-pic",
    "-fno-pie",
    "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables",
    "-mno-red-zone",
    "-mgeneral-regs-only",
];

fn run(cmd: &mut Command) {
    let status = cmd.status().unwrap_or_else(|e| panic!("could not run {cmd:?}: {e}"));
    assert!(status.success(), "{cmd:?} failed");
}

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let top = manifest.parent().unwrap();
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let cc = env::var("CC").unwrap_or_else(|_| "gcc".into());

    let mut objects = Vec::new();
    for src in ["alloc/alloc.c", "bench/pipeline.c"] {
        let src = top.join(src);
        let obj = out.join(Path::new(src.file_name().unwrap()).with_extension("o"));
        run(Command::new(&cc).args(CFLAGS).arg("-c").arg(&src).arg("-o").arg(&obj));
        println!("cargo:rerun-if-changed={}", src.display());
        objects.push(obj);
    }
    for header in ["alloc/alloc.h", "bench/pipeline.h"] {
        println!("cargo:rerun-if-changed={}", top.join(header).display());
    }

    let lib = out.join("libshared.a");
    let _ = std::fs::remove_file(&lib);
    run(Command::new("ar").arg("crs").arg(&lib).args(&objects));

    let script = manifest.join("linker.ld");
    println!("cargo:rerun-if-changed={}", script.display());
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=shared");
    println!("cargo:rustc-link-arg-bins=--script={}", script.display());
}
