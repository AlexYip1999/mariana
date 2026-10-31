# third_party（vendored single headers）

- `nlohmann/json.hpp` — [nlohmann/json](https://github.com/nlohmann/json) 3.11.2，MIT
- `nlohmann/LICENSE.MIT` — nlohmann/json 许可证
- `doctest/doctest.h` — [doctest/doctest](https://github.com/doctest/doctest) 2.4.6，MIT

说明：当前 A0 阶段使用 vendored 单头文件，离线可构建（从本机已有工程复制，避免联网下载）。升级时直接替换文件并核对版本号。

## 后续可切换的依赖管理方式

1. **git submodule**：版本锁在 commit，可追溯、可改源码；代价是仓库体积、需要网络/代理、克隆要 `--recursive`。

   ```powershell
   git submodule add --depth 1 https://github.com/nlohmann/json.git third_party/json
   git submodule add --depth 1 https://github.com/doctest/doctest.git third_party/doctest
   ```

   切换后删除对应的 vendored 单头文件，CMake 改为 include 子模块路径，并在 README 注明 `git clone --recurse-submodules`。

2. **CMake FetchContent**：在 `CMakeLists.txt` 里 pin tag/hash，配置时下载，不把源码入库；适合 header-only 库。

3. **vcpkg manifest**：适合 libcurl / SQLite 这类二进制依赖；Visual Studio 自带 vcpkg，可做 manifest 模式。

结论：header-only 的单头库用 submodule 收益有限（仓库大、网络依赖强）；等需要 Git 级版本锁定或要改源码时再切换，当前 vendored 方案最适合离线开发。
