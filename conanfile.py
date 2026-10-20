from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class EdgePulseRecipe(ConanFile):
    """开发用配方：只负责为本地构建解析依赖，不产出应用包。"""

    settings = "os", "compiler", "build_type", "arch"
    requires = (
        "asyngyanis/1.0.0",
        "openssl/3.6.5",
        "gtest/1.17.0",
    )
    default_options = {"gtest/*:shared": False, "asyngyanis/*:with_mysql": True}

    def layout(self):
        # Conan 2.25 的单配置布局会把生成器放进 build/<build_type>/generators，
        # 而仓库预设固定引用 build/generators；这里统一成不随平台与构建类型变化的路径
        cmake_layout(self, generator="Ninja")
        self.folders.build = "build"
        self.folders.generators = "build/generators"

    def generate(self):
        CMakeDeps(self).generate()
        # 工程预设固定用 Ninja；不显式指定时 Conan 在 Windows 上会选 Visual Studio 生成器，
        # 并把 CMAKE_GENERATOR_PLATFORM=x64 写进工具链，Ninja 随后以「不支持平台规格」中止
        toolchain = CMakeToolchain(self, generator="Ninja")
        toolchain.user_presets_path = False
        toolchain.variables["EDGEPULSE_BUILD_TESTS"] = True
        toolchain.generate()
