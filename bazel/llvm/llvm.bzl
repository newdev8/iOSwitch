"""@llvm: the LLVM binaries the build runs, from the official releases (23.1.4, the version Homebrew had when the
CMake flow was qualified). LLVM dropped x86_64 macOS binaries after 19, so Intel Macs use 19.1.7. Only these members
of the archive are extracted (the whole release is 7.6 GB unpacked); the archive itself is the one download the
repository cache keeps (the GPU's @gpu_llvm_as reads the same sha256)."""

LLVM_RELEASES = {
    "arm64": {
        "url": "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-macOS-ARM64.tar.xz",
        "sha256": "5d3179db39adeca5b17a34d82708fe976bc60e055b05fe557878fb3fb64d39ef",
        "prefix": "LLVM-23.1.4-macOS-ARM64",
        "resource_headers": "lib/clang/23/include",
    },
    "x86_64": {
        "url": "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/LLVM-19.1.7-macOS-X64.tar.xz",
        "sha256": "49405e75fbe7ad6f8139a33f59ec8c5112b75b3027405c7b92d19f4c6f02c78a",
        "prefix": "LLVM-19.1.7-macOS-X64",
        "resource_headers": "lib/clang/19/include",
    },
}

# ld.lld, the loader's linker (a symlink to lld); llvm-objcopy (late objects, loader.bin); llvm-readelf (a symlink to
# llvm-readobj) and llvm-objdump (the loader builder's checks); clang-format and clang-tidy (//bazel/lint); the clang
# resource headers, which clang-tidy parses every unit with.
TOOLS = ["lld", "ld.lld", "llvm-objcopy", "llvm-readobj", "llvm-readelf", "llvm-objdump", "clang-format", "clang-tidy"]


def _llvm_impl(rctx):
    release = LLVM_RELEASES[rctx.os.arch]
    archive = "llvm.tar.xz"
    rctx.download(url = release["url"], output = archive, sha256 = release["sha256"])
    result = rctx.execute(["/usr/bin/tar", "-xJf", archive, "--strip-components", "1"] +
                          [release["prefix"] + "/bin/" + tool for tool in TOOLS] +
                          [release["prefix"] + "/" + release["resource_headers"]])
    if result.return_code:
        fail("extracting LLVM tools failed: " + result.stderr)
    rctx.delete(archive)
    rctx.file("BUILD.bazel", """package(default_visibility = ["//visibility:public"])

exports_files({})

filegroup(
    name = "clang_resource_headers",
    srcs = glob(["{}/**"]),
)
""".format(repr(["bin/" + tool for tool in TOOLS]), release["resource_headers"]))

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)


llvm_tools = repository_rule(implementation = _llvm_impl)
