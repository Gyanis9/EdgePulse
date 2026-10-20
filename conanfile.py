from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class EdgePulseRecipe(ConanFile):
    """开发用配方：只负责为本地构建解析依赖，不产出应用包。"""

    settings = "os", "compiler", "build_type", "arch"
    requires = (
        "asyngyanis/4.0.0",
        "openssl/3.6.5",
        "gtest/1.17.0",
    )
    default_options = {"gtest/*:shared": False}

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        # 工程预设固定用 Ninja；不显式指定时 Conan 在 Windows 上会选 Visual Studio 生成器，
        # 并把 CMAKE_GENERATOR_PLATFORM=x64 写进工具链，Ninja 随后以「不支持平台规格」中止
        toolchain = CMakeToolchain(self, generator="Ninja")
        toolchain.user_presets_path = False
        toolchain.variables["EDGEPULSE_BUILD_TESTS"] = True
        toolchain.generate()
