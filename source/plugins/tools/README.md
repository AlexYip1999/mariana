# tools（内置工具插件）

## read_file

读取 workspace 内的 UTF-8 文本文件，只读。

安全边界：

- `WorkspacePathPolicy`：canonical workspace root + 越界/敏感路径判断；
- 拒绝绝对路径、`..` 越界、目录、symlink/junction 逃逸；
- 拒绝敏感文件（`.env*`、`config.local.json`、`.git`、`.docker`、`.kube`、私钥/证书等）；
- Windows 使用 `CreateFileW` 打开后，用 `GetFinalPathNameByHandleW` 对**句柄**做二次路径复核，避免 check→open TOCTOU；
- 单文件上限 256 KiB，只接受 UTF-8 文本。

后续写工具、编辑工具、命令执行应复用 `WorkspacePathPolicy`，不要各自实现路径判断。
