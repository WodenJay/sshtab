# sshtab

Bash 下 `ssh <Tab>` 选择最近连接并回填命令的工具。

## 项目介绍

sshtab 是一个专注于 Bash 环境的命令行增强工具，用于解决“常用 SSH 目标难以快速复用”的问题。它在不改变你现有 `ssh` 使用习惯的前提下，提供一个轻量的交互式历史选择器：输入 `ssh` 后按 Tab，即可从最近成功连接的记录中选择并回填完整参数，继续编辑后再执行。

核心特性：
- 仅在 `ssh` + Tab 的空参数位置触发，不抢占原生 host 补全。
- 仅记录成功退出（exit code 0）的 `ssh` 命令。
- 安全执行：不使用 eval/system，仅做最小 tokenization 后 exec 真正 ssh。
- Bash-only，开箱即用，卸载可逆。

## 一行命令安装（curl|bash）

```bash
curl -fsSL https://raw.githubusercontent.com/WodenJay/sshtab/main/scripts/install-remote.sh | bash
```

安装后执行：

```bash
source ~/.bashrc
```

或重新打开终端。

后续使用的ssh命令都会被自动记录，被记录的ssh命令就可以通过sshtab来自动补全

## 安装（本地构建）

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/*.cpp -o sshtab
./scripts/install.sh
```

## 使用方法

- 自动记录：执行成功（exit code 0）的 `ssh` 会写入历史。
- 触发选择：输入 `ssh` 或 `ssh ` 后按 Tab，会弹出最近列表；↑/↓ 选择，Enter 回填，Esc/Ctrl+C 取消。
- 不影响原生补全：`ssh a<Tab>` 仍走原生 ssh/known_hosts 补全。

## 卸载

本地安装卸载：

```bash
./scripts/uninstall.sh
```

删除历史记录：

```bash
./scripts/uninstall.sh --purge
```

远程安装卸载（可选）：

```bash
curl -fsSL https://raw.githubusercontent.com/WodenJay/sshtab/main/scripts/uninstall-remote.sh | bash
```

删除历史记录：

```bash
curl -fsSL https://raw.githubusercontent.com/WodenJay/sshtab/main/scripts/uninstall-remote.sh | bash -s -- --purge
```

卸载后建议执行 `source ~/.bashrc` 或重开终端。

## 限制与兼容性

- 仅支持 Bash。
- 若用户已有 DEBUG trap，工具会降级并禁用记录（仅提示一次）。
- /dev/tty 不可用时，`pick` 会失败并回退补全。
- 远程安装默认仅支持 Linux x86_64。
- 预编译二进制基于 ubuntu-22.04 构建，兼容性更广；如仍遇到 glibc 过旧问题，请使用本地源码构建。

## 安全说明

- `exec` 不使用 eval/system，仅做最小 tokenization 后 exec 真正 ssh。
- 拒绝控制字符与明显 shell 元字符（`; | & \` $ ( ) < >`）。

## Acknowledge

代码由codex编写
