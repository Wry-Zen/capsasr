# CapsASR 语音输入

长按 **CapsLock** 说话，松手自动识别并把文本粘贴到光标处。全程本地推理——不联网、不落盘、无 PyTorch。

- **交互**：按住 >325ms 进入语音态；短敲仍是原生 CapsLock（keyd 直接发出，守护进程不在线也有效）
- **实测**：松手到出字 0.4–0.7s（5–20s 语句）；常驻显存 ~2.7GB（RX 9070），可与本机 LLM 异卡共存
- **后端**：cuda → vulkan → cpu 自动探测降级；支持按设备名或序号选卡
- **实现**：Python 仅标准库（≥3.11，零 pip 依赖）+ 自建 Fun-ASR-Nano GGUF 引擎（llama.cpp 系，纯 C++ 直驱 GPU）

## 原理

```
CapsLock ──keyd(/etc/keyd, overloadt 325ms)──> voice层 ──keyd listen──> capsasr daemon
                                              ├─ parec 常驻采集（300ms 预卷，s16le bytes 直传）
                                              ├─ 松手 → 能量裁剪首尾静音
                                              ├─ ASR：llama-funasr-cli --stream 常驻子进程
                                              │    （GGUF Q8；后端探测 cuda→vulkan→cpu）
                                              │    松手时 SIGUSR1 触发尾段提交（tools/funasr-cli.patch）
                                              └─ wl-copy + ydotool Ctrl+V 粘贴
                                                   （0.5s 后按原 MIME 恢复剪贴板，含图片等二进制）
```

## 安装（Arch）

包分两个：`capsasr` 主包（Python 运行体 + Vulkan 引擎，独立完整可用）与 `capsasr-cuda`
（CUDA 引擎，仅 NVIDIA 用户加装，连带安装 nvidia-utils/cuda/nccl）。auto 模式下 CUDA 优先于
Vulkan；未安装则探测自动跳过，不影响使用。

```bash
# A. 预构建包（GitHub Release 下载）
sudo pacman -U capsasr-0.6.0-2-x86_64.pkg.tar.zst
#   要用 CUDA 后端再追加：
#   sudo pacman -U capsasr-cuda-0.6.0-2-x86_64.pkg.tar.zst

# B. 从源码构建（自动拉预编译引擎；需 base-devel 与 patchelf）
git clone https://github.com/Wry-Zen/capsasr.git && cd capsasr/packaging && makepkg -sf
sudo pacman -U capsasr-0.6.0-2-x86_64.pkg.tar.zst   # 需要时再装 capsasr-cuda-*.zst
```

命令补全（bash/zsh/fish）随包安装；仓库直跑可 `source completions/capsasr.bash`（zsh/fish 同理）。

**平台要求**：x86_64 Linux（Arch；CPU 需支持 AVX2，约 2013 年后）；**Wayland 会话**（粘贴链依赖 wl-copy/wl-paste）；GPU 显存 ~2.7GB（Vulkan 或 CUDA，不足则自动降级 CPU，零显存但延迟约翻倍）；CUDA 引擎需 NVIDIA 驱动 + CUDA 13 运行库（`capsasr-cuda` 会自动拉齐）。

## 初始化

```bash
capsasr init            # 向导：模型下载、后端/选卡、激活时长、通知通道、热词、自启
sudo capsasr setup-keyd # 唯一需要 root 的环节：把 CapsLock 长按阈值写入 keyd
capsasr-daemon &        # init 选了自启则下次登录生效；这是立即启动
```

- 包内**不含模型**：三个 gguf（encoder 469MB + LLM 805MB + VAD 1.7MB，约 1.3GB）由 init 从
  Hugging Face / hf-mirror / ModelScope 依次回退下载，sha256 校验，中断重跑续传（已就绪自动跳过）
- init 选 autostart 会立即后台启动守护进程
- 通知通道在向导里四选一：DMS 悬浮 toast / 通用通知 / 两者都要 / 不启用（见下文「通知通道」）

## 控制命令

| 命令 | 说明 |
|---|---|
| `capsasr init` | 向导：模型下载、后端/选卡、通知通道、热词、自启（`--force` 重配，`--yes` 全默认） |
| `capsasr doctor` | 环境体检：python/模型/后端探测/设备/daemon/keyd/ydotoold（`--verify` 做完整 sha256） |
| `capsasr config` | 列出全部配置（`*`=配置文件显式设置） |
| `capsasr config get backend.mode` | 查单键 |
| `capsasr config set backend.mode vulkan` | **终端改写配置**：行级编辑，保留注释与结构 |
| `capsasr config edit` / `config path` | 用 $EDITOR 打开配置 / 显示路径 |
| `capsasr hotwords add 词1 词2` | 热词管理：`list`（默认）查看 / `add`·`remove` 增删 / `edit` 编辑器 / `reload` 显式重载；改动自动经 socket 通知 daemon 重载（引擎重启约 1s），无需 pkill |
| `capsasr setup-keyd` | 把 `hotkey.hold_ms` 写入 keyd（sudo）；`--hold-ms N` 直接指定，`--remove` 回滚 |
| `capsasr uninstall` | 清理用户级痕迹（自启/服务/进程/数据）；**应在 `pacman -R` 之前执行**，`--purge` 连模型一起删 |
| `capsasr cli a.wav` | 经守护进程转写任意音频（16kHz 16bit PCM wav） |
| `capsasr daemon` | 前台运行守护进程（systemd/调试用） |
| `capsasr-daemon` | 带环境自愈 + 崩溃 3s 重拉的包装壳（桌面自启条目指向它） |

改 `hotkey.hold_ms` 后需 `sudo capsasr setup-keyd` 生效；改 `notify.enabled` / `notify.channel` 后重启 daemon。

## 目录布局

```
/usr/bin/{capsasr,capsasr-daemon}                    控制命令（PATH）
/usr/lib/capsasr/{capsasr,engines,tools}             核心运行体（不进 PATH，防与系统 llama.cpp 冲突）
~/.capsasr/                                          全部用户数据（卸载 = rm -rf）
  config.toml    配置（全部字段可省略，默认见 capsasr/config.py DEFAULTS）
  hotwords.txt   热词（每行一个，# 注释）
  models/        三个 gguf
  logs/          daemon.log / engine.log
```

热词是**生成偏置**（解码 LLM 的先验上下文，非硬约束）：放专名/术语/人名，宁缺毋滥，句子式条目无效。管理走 `capsasr hotwords add|remove|edit|list`，改动自动重载；也可直接编辑 `~/.capsasr/hotwords.txt`（每行一个，`#` 注释）后 `capsasr hotwords reload`。

## 配置参考

`~/.capsasr/config.toml` 全部可省略；`capsasr config` 以 `*` 标出显式设置项。

| 键 | 默认 | 说明 |
|---|---|---|
| `backend.mode` | auto | auto=cuda→vulkan→cpu 降级；显式 cuda/vulkan/cpu 只加 CPU 兜底 |
| `backend.device` | auto | 设备名子串（如 "RADV"、"RTX"；纯数字按序号）；auto=多卡取 0 号；点名不存在则拒绝上 GPU，宁降 CPU 不占别的卡 |
| `backend.ngl` | 99 | LLM 层 offload 数（0=纯 CPU） |
| `backend.enc_threads` | 16 | 编码器 CPU 回退线程数 |
| `models.enc / llm / vad` | 文件名 | 相对模型目录；绝对路径原样 |
| `engine.language` | 中文 | 中 / 英 / 日 |
| `engine.hotwords` | hotwords.txt | 空串=禁用热词 |
| `hotkey.layer` | voice | keyd 层名，须与 /etc/keyd 配置一致 |
| `hotkey.hold_ms` | 325 | 长按判定阈值（毫秒） |
| `notify.enabled` | false | 状态提示总开关（含后端降级通知） |
| `notify.channel` | desktop | desktop=通用通知留档；dms=DMS 悬浮 toast；both=悬浮+留档 |
| `capture.*` | 16000 / 300 / -40 / 100 / 200 / 0.35 / 60 | 采样率Hz / 预卷ms / 裁剪阈值dB / 首垫ms / 尾垫ms / 最短s / 最长s |
| `paste.restore_clipboard / restore_delay_ms` | true / 500 | 剪贴板恢复 |
| `server.socket` | capsasr.sock | 位于 `$XDG_RUNTIME_DIR` 下 |

## 通知通道

`[notify] channel` 决定状态提示走哪条路（`enabled = false` 时两条都不发）：

| 值 | 行为 |
|---|---|
| `desktop`（默认） | `notify-send` → 通知中心，有历史可回看 |
| `dms` | DMS 顶部居中悬浮 toast（`dms ipc call toast …`，namespace `dms:toast`），不进通知中心 |
| `both` | toast + `notify-send`（悬浮提示 + 留档） |

```bash
capsasr config set notify.enabled true  # 总开关，默认关；改了要重启 daemon
capsasr config set notify.channel dms   # desktop / dms / both
capsasr doctor                          # 会显示当前通道与 dms 是否可用
```

- **DMS toast 时长固定、不能自定义**：info 1.5s、warn 3s、error 5s，error 带明细 8s；一次只显示一条，多条排队（上限 3，同级同文本自动去重）
- `dms` 未安装/未启动/调用失败时**自动退回 `notify-send`**，提示不会静默丢失；需要历史记录就用 `both` 或 `desktop`
- 没有 DMS 的桌面（GNOME/KDE 等）选 `desktop` 即可；DMS 见 <https://github.com/AvengeMedia/DankMaterialShell>

## 后端与选卡

```toml
[backend]
mode = "auto"      # auto: cuda→vulkan→cpu 探测降级；也可显式 cuda/vulkan/cpu
device = "auto"    # 设备名子串（如 "RADV"、"RTX"；纯数字一律按序号解释）或序号；auto=多卡时取 0 号
```

- 探测判据：引擎二进制存在 + 驱动库可 dlopen + ldd 无缺库；spawn 后 60s 内未 READY 自动换下一候选并气泡通知
- 显式指定的后端只降级到 CPU，不会悄悄换卡（点名某张卡通常有理由）
- 设备枚举优先引擎 `--list-devices`（Vulkan 引擎 0.4.1 起原生支持，展示真实设备名）；无此参数的旧二进制回退 `vulkaninfo --summary` / `nvidia-smi -L`
- 选卡结果经 `GGML_VK_VISIBLE_DEVICES` / `CUDA_VISIBLE_DEVICES` 注入子进程

## 引擎

识别引擎基于 Fun-ASR 官方 llama.cpp fork（Fun-ASR-Nano，GGUF Q8_0），随包预编译、免自建：

- `capsasr-engine-vulkan`：主包携带，仅需 Vulkan 驱动；可移植构建（AVX2 档，2013 年后的 x86_64 通用）
- `capsasr-engine-cuda`：`capsasr-cuda` 子包携带，sm_75–120a 全档（RTX 20 系至 50 系）+ 各档 PTX；CUDA 运行库由 daemon 自动解析
- 对上游的修改（SIGUSR1 松手即出字、编码器/LLM GPU 化、热词注入、设备枚举）见 `tools/funasr-cli.patch`

## 回滚 / 卸载

```bash
capsasr uninstall --purge        # 清自启/服务/进程/数据（--purge 连模型；不加则保留 ~/.capsasr）
sudo capsasr setup-keyd --remove  # CapsLock 恢复原状（可选）
sudo pacman -R capsasr            # 卸载系统包（装了 capsasr-cuda 则 `pacman -R capsasr capsasr-cuda`）
```

## 依赖与许可

- Python ≥ 3.11 标准库（tomllib；**零 pip 依赖**）
- 系统依赖见包 `depends`（pipewire/parec、keyd、wl-clipboard、ydotool+ydotoold、Vulkan 加载器）；通知为**可选**：libnotify（notify-send 通道）与 dms（toast 通道）缺失时提示静默跳过/退回，`capsasr init` 与 `doctor` 会显示各通道可用性
- 模型权重 Apache-2.0（FunAudioLLM）；本仓库代码 MIT（见 LICENSE）
