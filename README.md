# sshtab

Bash 下 `ssh + Tab` 选择最近连接并回填命令的工具。

## 安装

### 方式一：本地构建安装

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/*.cpp -o sshtab
./scripts/install.sh
```

安装后执行：

```bash
source ~/.bashrc
```

或重新打开终端。

### 方式二：Release 二进制（TODO）

从 Release 下载对应平台的 `sshtab` 二进制，放到仓库根目录后执行：

```bash
./scripts/install.sh
```

## 使用方法

- 自动记录：执行成功（exit code 0）的 `ssh` 会写入历史。
- 触发选择：输入 `ssh` 或 `ssh ` 后按 Tab，会弹出最近列表；↑/↓ 选择，Enter 回填，Esc/Ctrl+C 取消。
- 不影响原生补全：`ssh a<Tab>` 仍走原生 ssh/known_hosts 补全。

## 卸载

```bash
./scripts/uninstall.sh
```

删除历史记录：

```bash
./scripts/uninstall.sh --purge
```

卸载后建议执行 `source ~/.bashrc` 或重开终端。

## 限制与兼容性

- 仅支持 Bash。
- 若用户已有 DEBUG trap，工具会降级并禁用记录（仅提示一次）。
- /dev/tty 不可用时，`pick` 会失败并回退补全。

## 安全说明

- `exec` 不使用 eval/system，仅做最小 tokenization 后 exec 真正 ssh。
- 拒绝控制字符与明显 shell 元字符（`; | & \` $ ( ) < >`）。

