load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "embfmt",
    srcs = ["embformat.c"],
    hdrs = ["embformat.h"],
    copts = ["-w"],
    # Sources include it as <embfmt/embformat.h>. Under CMake that works because Modules/ is on
    # the include path; here the prefix is added explicitly.
    include_prefix = "embfmt",
    visibility = ["//visibility:public"],
)
