from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy

from pathlib import Path
import os


def _read_version():
    try:
        return (
            Path(__file__)
            .parent.joinpath("VERSION")
            .read_text(encoding="utf-8")
            .strip()
        )
    except Exception:
        return "0.0.0"


class AeronetConan(ConanFile):
    name = "aeronet"
    version = _read_version()
    license = "MIT"
    url = "https://github.com/sjanel/aeronet"
    description = "Linux-only C++23 HTTP/1.1 server library with optional TLS (OpenSSL)"
    topics = ("http", "server", "epoll", "tls", "networking")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_openssl": [True, False],
        "with_spdlog": [True, False],
        "with_br": [True, False],
        "with_zlib": [True, False],
        "with_zstd": [True, False],
        "with_opentelemetry": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_openssl": False,
        "with_spdlog": False,
        "with_br": False,
        "with_zlib": False,
        "with_zstd": False,
        "with_opentelemetry": False,
    }
    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "aeronet/*",
        "examples/*",
        "LICENSE",
        "README.md",
        "VERSION",
    )
    package_type = "library"
    required_conan_version = ">=2.0"

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["AERONET_ENABLE_ASAN"] = "OFF"
        tc.variables["AERONET_ENABLE_OPENSSL"] = (
            "ON" if self.options.with_openssl else "OFF"
        )
        tc.variables["AERONET_ENABLE_SPDLOG"] = (
            "ON" if self.options.with_spdlog else "OFF"
        )
        tc.variables["AERONET_ENABLE_BROTLI"] = "ON" if self.options.with_br else "OFF"
        tc.variables["AERONET_ENABLE_ZLIB"] = "ON" if self.options.with_zlib else "OFF"
        tc.variables["AERONET_ENABLE_ZSTD"] = "ON" if self.options.with_zstd else "OFF"
        tc.variables["AERONET_ENABLE_OPENTELEMETRY"] = (
            "ON" if self.options.with_opentelemetry else "OFF"
        )
        # Force OFF for tests/examples in package context
        tc.variables["AERONET_BUILD_TESTS"] = "OFF"
        tc.variables["AERONET_BUILD_BENCHMARKS"] = "OFF"
        tc.variables["AERONET_BUILD_EXAMPLES"] = "OFF"
        tc.variables["AERONET_BUILD_SHARED"] = "ON" if self.options.shared else "OFF"
        tc.variables["AERONET_INSTALL"] = "ON"
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cm = CMake(self)
        cm.configure()
        cm.build()
        # Tests intentionally not part of package build (run in project CI instead)

    def requirements(self):
        # Prefer consuming dependency packages (portable builds) when features enabled.
        # If you intend to rely purely on system packages, you can remove these.
        if self.options.with_openssl:
            self.requires("openssl/[~3.3]")
        if self.options.with_spdlog:
            self.requires("spdlog/[~1.15]")
        if self.options.with_br:
            self.requires("brotli/[~1.1]")
        if self.options.with_zlib:
            self.requires("zlib/[~1.3]")
        if self.options.with_zstd:
            self.requires("zstd/[~1.5]")
        if self.options.with_opentelemetry:
            self.requires("opentelemetry-cpp/[~1.22]")
            # We prefer OTLP HTTP exporter in the codebase (otel-tracer.cpp)
            # and do not currently use the gRPC exporter path. Avoid pulling
            # the heavy gRPC dependency transitively. Keep protobuf since
            # some opentelemetry pieces and generated protos may need it.
            self.requires("protobuf/[~5.27]")

    def package(self):
        cm = CMake(self)
        cm.install()
        # Conan 2 removed self.copy(); use tools.files.copy instead.
        copy(
            self,
            "LICENSE",
            dst=os.path.join(self.package_folder, "licenses"),
            src=self.source_folder,
        )

    def package_info(self):
        # Core libs always present
        self.cpp_info.libs = [
            "aeronet",
            "aeronet_http",
            "aeronet_tech",
            "aeronet_objects",
            "aeronet_sys",
        ]
        if self.options.with_openssl:
            # TLS module built separately
            self.cpp_info.libs.append("aeronet_tls")
        # Provide a convenient CMake target namespace expectation
        self.cpp_info.set_property("cmake_file_name", "aeronet")
        self.cpp_info.set_property("cmake_target_name", "aeronet::aeronet")
        if self.options.with_openssl:
            self.cpp_info.requires.append("openssl::openssl")
        if self.options.with_spdlog:
            self.cpp_info.requires.append("spdlog::spdlog")
        if self.options.with_zlib:
            self.cpp_info.requires.append("zlib::zlib")
        if self.options.with_zstd:
            self.cpp_info.requires.append("zstd::zstd")
        if self.options.with_opentelemetry:
            self.cpp_info.requires.append("opentelemetry-cpp::opentelemetry-cpp")
            # Protobuf is a direct requirement when OpenTelemetry support is enabled
            # and some exported CMake targets / generated protos may need it. Ensure
            # the Conan package_info references the protobuf requirement so Conan
            # doesn't treat it as unused when creating the package.
            self.cpp_info.requires.append("protobuf::protobuf")
