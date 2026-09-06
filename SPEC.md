# 29. Reference Repositories

开发前建议将以下开源项目 clone 到项目根目录的：

```text id="k6kjs5"
references/
```

这些仓库全部视为：

```text id="77o5n8"
READ-ONLY REFERENCE
```

它们用于：

- API 使用方式参考
- Linux / Wayland 实现参考
- ASR streaming 实现参考
- NixOS packaging 参考
- AMD XDNA2 实验参考
- 避免重复造轮子

不得因为某个 reference 使用不同技术栈而修改本项目 `SPEC.md` 定义的总体架构。

本项目仍然坚持：

```text id="dqypcn"
C17 core
+ PipeWire
+ sherpa-onnx
+ Unix Domain Socket
+ Qt6/QML thin overlay
+ Fcitx5/libei input
+ Nix Flake
+ optional AMD XDNA2
```

---

## 29.1 sherpa-onnx

Repository:

```text id="63r1j8"
https://github.com/k2-fsa/sherpa-onnx
```

Local:

```text id="uyyqkg"
references/sherpa-onnx
```

优先级：

```text id="2sqtct"
★★★★★
PRIMARY ASR REFERENCE
```

重点研究：

```text id="3jd98w"
c-api-examples/streaming-zipformer-c-api.c

sherpa-onnx/c-api/

OnlineRecognizer

OnlineStream

AcceptWaveform

DecodeOnlineStream

GetOnlineStreamResult

endpoint detection

streaming Zipformer

partial result

final result

INT8 model
```

重点理解：

```text id="1yb7dv"
PCM
 ↓
OnlineStream
 ↓
AcceptWaveform
 ↓
Decode
 ↓
Partial
 ↓
Endpoint
 ↓
Final
 ↓
Reset
```

这是项目 M2 的主要实现参考。

优先使用：

```text id="8h1kby"
sherpa-onnx C API
```

不要为了调用 sherpa 引入：

```text id="6cf4vm"
Python
```

---

# 29.2 Nextalk

Repository:

```text id="pyyjxl"
https://github.com/gonewx/nextalk
```

Local:

```text id="pf49aj"
references/nextalk
```

优先级：

```text id="98xnbd"
★★★★★
PRIMARY PRODUCT REFERENCE
```

这是目前最接近本项目目标的现有 Linux voice input 项目之一。

重点研究：

```text id="07g1zg"
Streaming Zipformer

partial result workflow

final result workflow

Fcitx5 integration

commitString / text commit

Wayland behavior

KDE shortcut integration

floating overlay

model management

endpoint handling
```

尤其研究：

```text id="8if11h"
Fcitx5
 ↓
current input context
 ↓
commit text
```

以及：

```text id="gmzb0b"
Streaming ASR
 ↓
partial
 ↓
overlay

final
 ↓
Fcitx5
```

不要照搬：

```text id="m2dgrb"
Flutter

Dart core

PortAudio architecture

Flutter state management

runtime model downloader
```

本项目对应实现必须仍然使用：

```text id="ozxztk"
C17 daemon

PipeWire

Unix socket

Qt/QML thin UI
```

Nextalk 主要用于：

> 学习已经验证可行的 Linux / Wayland / Fcitx5 / Streaming ASR 产品路径。

不是本项目 architecture template。

---

# 29.3 whisper-overlay

Repository:

```text id="mxujng"
https://github.com/oddlama/whisper-overlay
```

Local:

```text id="62ng0x"
references/whisper-overlay
```

优先级：

```text id="lq0d2h"
★★★★★
PRIMARY NIXOS REFERENCE
```

重点研究：

```text id="hq2i3r"
flake.nix

NixOS module

Home Manager module

systemd integration

package structure

Wayland overlay

global shortcut integration
```

主要用途：

```text id="i1jljt"
学习如何把 Linux voice input
完整包装成 Nix-native 软件
```

不要照搬：

```text id="ujdwuv"
Python ASR server

RealtimeSTT

Docker

Whisper architecture

localhost network service
```

本项目禁止因为参考该项目而引入：

```text id="i3wmh0"
Python daemon
```

---

# 29.4 Fcitx5

Repository:

```text id="xcpkyb"
https://github.com/fcitx/fcitx5
```

Local:

```text id="x9h08k"
references/fcitx5
```

优先级：

```text id="0i5zgq"
★★★★☆
```

重点研究：

```text id="6wp12w"
InputContext

commitString

addon architecture

DBus

Wayland integration

frontend architecture
```

目标：

寻找最干净的方式：

```text id="7v8jb3"
voice-inputd
      ↓
Fcitx5
      ↓
currently focused input context
      ↓
commit UTF-8 text
```

优先使用官方支持机制。

不要优先使用：

```text id="7n6k42"
模拟逐键输入
```

---

# 29.5 ydotool

Repository:

```text id="j2jd36"
https://github.com/ReimuNotMoe/ydotool
```

Local:

```text id="ulddo3"
references/ydotool
```

优先级：

```text id="mgwz0j"
★★★☆☆
FALLBACK REFERENCE
```

重点研究：

```text id="p83ln4"
uinput

Linux input event

minimal daemon

keyboard event generation

C implementation
```

ydotool 只作为：

```text id="trv5ca"
fallback input backend
```

不要让它成为 mandatory dependency。

正常优先级：

```text id="2a6e87"
Fcitx5
 ↓
libei
 ↓
clipboard + paste
 ↓
ydotool
```

---

# 29.6 AMD XDNA Driver

Repository:

```text id="rrqudy"
https://github.com/amd/xdna-driver
```

Local:

```text id="d60zmw"
references/xdna-driver
```

Clone 时：

```bash id="92c0eg"
git clone --recursive \
    https://github.com/amd/xdna-driver.git
```

优先级：

```text id="r0d7iw"
★★★★☆
OFFICIAL AMD REFERENCE
```

重点研究：

```text id="5mwpwk"
XDNA device detection

XRT shim

/dev/accel

device initialization

buffer management

command submission

Linux XDNA runtime
```

目标硬件：

```text id="ly1zof"
AMD Ryzen AI 9 HX 370

Strix Point

XDNA2
```

不要：

```text id="yjw4vf"
vendor kernel driver into project

modify kernel driver

make XDNA mandatory
```

AMD NPU 始终属于：

```text id="pj2z4l"
optional acceleration backend
```

---

# 29.7 Ryzen NPU Linux

Repository:

```text id="cwlghg"
https://github.com/Jonas-Augustinus-Linus/ryzen-npu-linux
```

Local:

```text id="k2f0oc"
references/ryzen-npu-linux
```

优先级：

```text id="78cw5u"
★★★★☆
XDNA2 PRACTICAL REFERENCE
```

重点研究：

```text id="xvebh4"
Strix Point

XDNA2

Ryzen AI NPU4

HX 370

XRT

IREE AMD AIE

IRON

persistent runner

device detection

benchmark

NPU dispatch overhead
```

尤其关注：

```text id="p0k4ml"
small workload
vs
NPU dispatch overhead
```

不要预设：

```text id="ydry02"
NPU always faster than CPU
```

项目真正关心的是：

```text id="e4n92v"
Latency
+
CPU usage
+
Wakeups
+
Package power
```

而不是：

```text id="y6z95h"
必须使用 NPU
```

如果 CPU backend：

```text id="c9z7k3"
first partial = 120 ms
CPU = 4%
power = low
```

而 NPU：

```text id="8wxtrd"
first partial = 180 ms
CPU = 1%
```

则需要根据综合体验决定 `auto` backend。

---

# 29.8 Vocalinux

Repository:

```text id="7lr48a"
https://github.com/VocaHQ/vocalinux
```

Local:

```text id="l8m0dt"
references/vocalinux
```

优先级：

```text id="qbszft"
★★☆☆☆
PRODUCT REFERENCE
```

重点研究：

```text id="82kb4v"
Linux desktop compatibility

Wayland behavior

push-to-talk

toggle recording

tray behavior

user-facing model selection

fallback strategies
```

不要照搬：

```text id="l2whof"
large GUI

unnecessary settings

Whisper-first architecture
```

---

# 29.9 PipeWire

Official:

```text id="f3wrhl"
https://pipewire.org/
```

Documentation:

```text id="ecmrf7"
https://docs.pipewire.org/
```

重点研究官方 C examples：

```text id="mbcy28"
audio capture

pw_stream

process callback

spa buffer

ring buffer

thread loop
```

本项目 Audio backend：

```text id="fpjglw"
PipeWire native
```

不要为了简单而改成：

```text id="2ve6zq"
PortAudio
```

---

# 29.10 libei

Official documentation:

```text id="r1cmzq"
https://libinput.pages.freedesktop.org/libei/
```

重点：

```text id="tv7j43"
libei

liboeffis

XDG RemoteDesktop portal

Wayland emulated input
```

用于研究：

```text id="8ib64s"
Wayland-native input injection
```

但第一版如果：

```text id="u5jvpm"
Fcitx5 commit
```

已经足够稳定，则不要为了 libei 增加 MVP 复杂度。

---

# 30. Clone Reference Sources

项目目录：

```text id="x9z6la"
voice-input/
├── SPEC.md
├── references/
└── src/
```

初始化：

```bash id="0uy0zu"
mkdir -p references
cd references

# ============================================================
# ASR
# ============================================================

git clone \
    https://github.com/k2-fsa/sherpa-onnx.git

# ============================================================
# Existing Linux Voice Input Projects
# ============================================================

git clone \
    https://github.com/gonewx/nextalk.git

git clone \
    https://github.com/oddlama/whisper-overlay.git

git clone \
    https://github.com/VocaHQ/vocalinux.git

# ============================================================
# Linux Input
# ============================================================

git clone \
    https://github.com/fcitx/fcitx5.git

git clone \
    https://github.com/ReimuNotMoe/ydotool.git

# ============================================================
# AMD Ryzen AI / XDNA
# ============================================================

git clone --recursive \
    https://github.com/amd/xdna-driver.git

git clone \
    https://github.com/Jonas-Augustinus-Linus/ryzen-npu-linux.git
```

---

# 31. Freeze Reference Versions

references 不应该永远跟随 upstream main。

下载完成以后执行：

```bash id="9n7ysc"
cd references

for dir in */; do
    (
        cd "$dir" || exit

        printf "%-30s %s\n" \
            "${dir%/}" \
            "$(git rev-parse HEAD)"
    )
done
```

将结果保存：

```bash id="08iqdy"
cd references

for dir in */; do
    (
        cd "$dir" || exit

        printf "%-30s %s\n" \
            "${dir%/}" \
            "$(git rev-parse HEAD)"
    )
done > ../REFERENCE_VERSIONS.txt
```

得到：

```text id="c2szm3"
voice-input/
├── SPEC.md
├── REFERENCE_VERSIONS.txt
└── references/
```

这样未来可以明确知道 Codex 开发时参考的是哪一版本 upstream source。

---

# 32. Instructions for Codex When Reading References

Codex 可以：

```text id="ohrnri"
read

grep

rg

inspect API usage

compare implementations

study architecture

study packaging

study benchmark methodology
```

Codex 不应该：

```text id="ldo4nw"
copy entire project

silently change architecture

introduce Python because a reference uses Python

introduce Flutter because Nextalk uses Flutter

introduce PortAudio because another project uses PortAudio

replace PipeWire

replace C core

replace Unix socket with HTTP

replace streaming Zipformer with Whisper
```

当 reference implementation 与 `SPEC.md` 冲突：

```text id="qim92h"
SPEC.md wins.
```

---

# 33. Reference Priority

Codex 阅读顺序：

```text id="wb3xve"
1. SPEC.md

2. sherpa-onnx
   ↓
   Streaming ASR / C API

3. nextalk
   ↓
   Fcitx5 / Wayland / product behavior

4. whisper-overlay
   ↓
   Nix / NixOS / packaging

5. fcitx5
   ↓
   native text commit

6. PipeWire official API
   ↓
   native audio capture

7. xdna-driver
   +
   ryzen-npu-linux
   ↓
   AMD NPU

8. ydotool
   ↓
   fallback only

9. Vocalinux
   ↓
   UX reference only
```

---

# 34. Critical Rule

不要因为已经存在：

```text id="ept4w8"
Nextalk
whisper-overlay
Vocalinux
```

而尝试 fork 后修改。

本项目独立实现。

目标差异：

```text id="3m2pu5"
Existing projects

feature rich
general Linux
multiple stacks
runtime setup
sometimes Python / Flutter / Whisper


voice-input

tiny
C-first
PipeWire-native
Wayland-first
NixOS-first
true streaming
offline
low latency
low power
optional XDNA2
```

核心问题始终是：

> Can voice input feel as immediate and lightweight as a native Linux input method?

---

# 35. Final Engineering Rule

任何技术选择都按照：

```text id="0hw39a"
Does it reduce latency?

Does it reduce resource usage?

Does it reduce dependencies?

Does it improve reliability?

Does it fit Linux/Wayland architecture?
```

如果一个方案只是：

```text id="5mx0jn"
easier to implement
```

但会引入：

```text id="cuwrcb"
Python daemon
Web server
Docker
Electron
polling
large runtime
high idle CPU
```

则默认拒绝。

最终理想状态：

```text id="a2dhrv"
systemd --user
       │
       ▼
 voice-inputd
       │
       │ idle
       ▼
     0% CPU


Meta+O
       │
       ▼
   PipeWire
       │
       ▼
 Streaming ASR
       │
       ├── partial ──→ tiny overlay
       │
       └── final ────→ Fcitx5
                            │
                            ▼
                     current application


Meta+O
       │
       ▼
      IDLE
```

安装：

```bash id="1yr1qn"
nix build
```

部署：

```bash id="8imvld"
sudo nixos-rebuild switch --flake .
```

使用：

```text id="cvbjzj"
Meta+O
↓
说话
↓
实时显示
↓
立即输入
```

That's it.