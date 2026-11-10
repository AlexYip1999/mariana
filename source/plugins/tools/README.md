# tools（内置工具插件）

内置只读工具：`read_file` / `list_dir` / `search_text`。全部走 `core::Tool` + `ToolDescriptor`（Read、无副作用、NotRequired、worker）。

## read_file

读取 workspace 内的 UTF-8 文本文件，只读。

安全边界：

- `WorkspacePathPolicy`：canonical workspace root + 越界/敏感路径判断；
- 拒绝绝对路径、`..` 越界、目录、symlink/junction 逃逸；
- 拒绝敏感文件（`.env*`、`config.local.json`、`.git`、`.docker`、`.kube`、私钥/证书等）；
- Windows 使用 `CreateFileW` 打开后，用 `GetFinalPathNameByHandleW` 对**句柄**做二次路径复核，避免 check→open TOCTOU；
- 单文件上限 256 KiB，只接受 UTF-8 文本。

## list_dir

列出 workspace 内某个目录的条目，只读。

- 参数 `{"path":"relative/dir"}`；`path` 缺省为 workspace 根；
- 输出按文件名排序：目录为 `dir <name>`，文件为 `file <name> (<size> bytes)`；
- 跳过 symlink、敏感项以及设备/管道等特殊文件；默认最多展示 200 条，超出追加 `... (<N> more entries)`；
- 拒绝绝对路径、`..` 越界与敏感路径；目录不存在或不是目录时返回错误。

## search_text

在 workspace 内的文本文件中做大小写敏感子串搜索，只读。

- 参数 `{"query":"substring","path":"relative/file-or-dir","max_results":50}`；`query` 必填非空，
  `path` 缺省为 workspace 根，`max_results` 缺省 50；
- 目录递归扫描并按相对路径排序；跳过 symlink、敏感文件、含 NUL 的二进制文件与超过 256 KiB 的文件；
- 输出 `relative/path:line: text`（去掉行尾 `\r`）；
- 命中行数超过 `max_results` 追加 `... (results truncated)`；扫描文件数超过 500 追加 `... (scan truncated)`。

后续写工具、编辑工具、命令执行应复用 `WorkspacePathPolicy`，不要各自实现路径判断。
