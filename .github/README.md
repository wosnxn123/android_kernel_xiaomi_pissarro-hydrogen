# GitHub Actions 内核构建

工作流：[`Build Hydrogen Kernel`](workflows/kernel-build.yml)。

## 触发方式

- 推送到 `lineage-23.2` 或向该分支提交 PR：构建 `pissarro_defconfig`。
- 仅修改 `Documentation/` 或 Markdown 文件时不自动构建。
- 手动构建：打开仓库 **Actions → Build Hydrogen Kernel → Run workflow**，选择分支与配置：
  - `pissarro`：普通版，默认选项。
  - `pissarro_debug`：调试版。
  - `pissarro_stability`：稳定性诊断版，包含额外调试检查。

Fork 仓库可能需要先在 Actions 页面启用工作流。手动运行按钮需要该工作流已经存在于仓库默认分支。

## 构建与产物

工作流使用 Ubuntu 22.04，复用根目录 `build.sh`：

- AOSP `android-16.0.0_r4` 的 `clang-r563880c`，首次下载后缓存。
- 按配置缓存 ccache，最多 3 GB；不缓存 `out/`，每次重新配置并构建。
- 编译 `Image.gz`，通过现有 AnyKernel3 `hydrogen-4.14.y` 分支打包，使用仓库自带的 `prebuilts/dtb.img`、`prebuilts/dtbo.img`。
- 成功后上传 `HydrogenKernel-<配置>-<运行编号>`，包含刷机 ZIP、`Image.gz` 和 `SHA256SUMS`。
- 无论成功或失败，均尝试上传 `build-diagnostics-<配置>-<运行编号>`，包含构建信息及已经生成的日志、内核配置和 `System.map`。
- 产物保留 14 天；单次任务最多运行 180 分钟。同一分支/PR、同一配置的新运行会取消旧运行。

在完成的 Actions 运行页面底部 **Artifacts** 下载产物。刷机包是产物压缩包中的 `HydrogenKernel-*.zip`，不是 GitHub 外层的下载压缩包。

工作流只申请仓库读取权限，不需要额外 Secrets，不会自动创建 Release 或推送提交。修改 `build.sh` 的工具链版本时，也要同步工作流中的 `CLANG_VERSION`、`CLANG_TAG`。

CI 编译成功不代表已通过实机启动、功耗或稳定性测试。
