# 📋 Flutter Linux 桌面端 `media_kit` 音视频插件 环境适配文档
## 一、问题背景
在 Flutter Linux 桌面端开发中，使用 `media_kit` / `media_kit_video` 音视频插件时，CMake 构建阶段会出现如下报错：
```
CMake Error at .../media_kit_video/linux/CMakeLists.txt:53 (target_link_libraries):
  Target "media_kit_video_plugin" links to:
    PkgConfig:::mpv
  but the target was not found.
```
**根因**：Linux 系统缺失 `mpv` 开发库（`libmpv-dev`），CMake 无法找到 `mpv` 链接目标，导致构建失败。

---

## 二、前置依赖说明
`media_kit` 是 Flutter 生态高性能音视频播放插件，**Linux 桌面端强制依赖系统级 `mpv` 播放器**作为底层解码引擎：
| 依赖包 | 作用 |
|--------|------|
| `libmpv-dev` | `mpv` 开发库，提供 CMake 链接所需的头文件、`.pc` 配置文件 |
| `mpv` | `mpv` 运行时，提供实际音视频解码能力 |
| `pkg-config` | CMake 依赖的系统工具，用于解析 `.pc` 配置文件 |

---

## 三、分发行版依赖安装步骤
### 1. Ubuntu / Debian 系列（最常用）
```bash
# 1. 更新软件源
sudo apt update && sudo apt upgrade -y

# 2. 安装核心依赖（mpv 开发库 + 运行时 + pkg-config）
sudo apt install -y libmpv-dev mpv pkg-config

# 3. 补充安装 Flutter Linux 构建必备基础工具（首次构建必装）
sudo apt install -y build-essential cmake clang libgtk-3-dev
```

### 2. Fedora / RHEL / CentOS 系列
```bash
# 1. 安装核心依赖
sudo dnf install -y mpv-devel mpv pkg-config

# 2. 补充 Flutter Linux 构建基础工具
sudo dnf install -y gcc-c++ cmake clang gtk3-devel
```

### 3. Arch Linux / Manjaro 系列
```bash
# 1. 安装核心依赖
sudo pacman -S mpv pkg-config

# 2. 补充 Flutter Linux 构建基础工具
sudo pacman -S base-devel cmake clang gtk3
```

### 4. openSUSE 系列
```bash
# 1. 安装核心依赖
sudo zypper install -y libmpv-devel mpv pkg-config

# 2. 补充 Flutter Linux 构建基础工具
sudo zypper install -y gcc-c++ cmake clang gtk3-devel
```

---

## 四、依赖有效性验证
安装完成后，执行以下命令验证依赖是否被 CMake 正确识别：
```bash
# 验证 pkg-config 能否找到 mpv 开发库
pkg-config --exists mpv && echo "✅ mpv 依赖安装成功" || echo "❌ 依赖未找到，请重新安装"

# 查看 mpv 版本（可选，用于确认）
pkg-config --modversion mpv

# 查看编译链接参数（可选，用于排查）
pkg-config --cflags --libs mpv
```
> 若输出 `✅ mpv 依赖安装成功`，说明依赖配置正常；若失败，需检查安装命令是否执行完整、系统源是否正常。

---

## 五、Flutter 项目构建流程
依赖安装完成后，需彻底清理旧缓存，重新构建项目：
```bash
# 1. 进入 Flutter 项目根目录
cd /path/to/your/flutter/project

# 2. 清理项目构建缓存（关键步骤，避免旧配置残留）
flutter clean

# 3. 重新获取项目依赖
flutter pub get

# 4. 运行 Linux 桌面端
flutter run -d linux

# （可选）打包 Linux 桌面端应用
flutter build linux --release
```

---

## 六、常见问题与避坑指南
### 1. 安装后仍报错？排查 Flutter 安装方式
**禁止使用 `snap` 安装 Flutter**：snap 沙箱会隔离系统依赖，导致 CMake 无法访问系统 `libmpv` 库。
- 解决方案：卸载 snap 版 Flutter，改用[官方手动安装](sslocal://flow/file_open?url=https%3A%2F%2Fdocs.flutter.dev%2Fget-started%2Finstall%2Flinux&flow_extra=eyJsaW5rX3R5cGUiOiJjb2RlX2ludGVycHJldGVyIn0=)的版本，解压到 `~/flutter` 并配置环境变量。

### 2. 手动编译 mpv 后找不到依赖？
若手动编译 `mpv` 源码安装，需满足以下条件：
1.  编译时必须添加 `--enable-libmpv-shared` 选项，生成 `libmpv.so` 动态库
2.  安装后执行 `sudo ldconfig` 更新系统库缓存
3.  确保 `mpv.pc` 文件位于 `/usr/lib/pkgconfig/` 或 `/usr/local/lib/pkgconfig/` 路径下

### 3. 其他 CMake 报错？补充完整多媒体依赖
若后续出现其他依赖缺失，可一次性安装完整多媒体开发工具链：
```bash
# Ubuntu/Debian 系列
sudo apt install -y build-essential cmake meson libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
```

### 4. 权限问题导致安装失败？
执行 `sudo apt install` 时若报错权限不足，需检查当前用户是否在 `sudoers` 列表中，或使用 `root` 用户执行安装。

---

## 七、项目级鲁棒性增强（可选）
为避免不同环境依赖缺失导致构建失败，可在项目 `linux/CMakeLists.txt` 中添加依赖检查（非必须，仅用于增强稳定性）：
```cmake
# 在 CMakeLists.txt 开头添加
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

# 原插件链接语句保持不变
target_link_libraries(media_kit_video_plugin PRIVATE PkgConfig::mpv)
```

---

## 八、文档维护说明
- 适用版本：`media_kit` v1.0+、`media_kit_video` v1.0+、Flutter 3.0+
- 适用系统：所有主流 Linux 发行版（Ubuntu/Debian、Fedora、Arch、openSUSE 等）
- 维护建议：若后续 `media_kit` 插件更新，需同步检查依赖版本兼容性

---

需要我再补充一份**一键执行的自动化脚本**，直接复制到终端就能完成所有依赖安装、验证和项目构建吗？