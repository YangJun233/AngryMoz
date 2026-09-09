# AngryMoz · 愤怒的蚊子

A tiny Windows 11 tray app that nags you with mosquitoes: sit too long by day and
one shows up; stay up past bedtime and they multiply. Native C++/Win32 + GDI+,
a single self-contained `.exe` (~500 KB, no runtime needed, ~2–4 MB RAM, ~0% CPU idle).

一个很小的 Windows 11 托盘小工具，用蚊子提醒你：白天久坐会冒出蚊子，晚上熬夜蚊子越来越多。
原生 C++/Win32 + GDI+，单个绿色 `.exe`（约 500 KB，免安装，内存 2–4 MB，空闲几乎不占 CPU）。

---

## English

### What it does
- **Daytime (anti–sitting):** while you use the PC your "energy" drains (tray ring
  goes green → red). After ~40 min of work, energy runs out and angry mosquitoes
  fly around your screen. Keep working and more appear. Get up and rest to clear them.
- **Night (anti–staying-up):** at bedtime (default 23:30) mosquitoes appear and grow
  more/bigger/louder to push you to bed. To dismiss them you transcribe a passage —
  the real goal is to make you too sleepy to bother, so you just go to sleep.
- **Bilingual (中文 / English)**, chosen on first run and switchable in Settings.

### Energy model (sedentary)
- Only **operating** (input) drains energy; being idle just holds it.
- **Start working:** at full energy, 10 s of continuous input starts a fixed 40-min countdown.
- **Away → full:** 15 min of continuous stillness refills energy instantly (configurable).
- Once empty, mosquitoes appear; **5 min of accumulated stillness** clears them
  (a stray touch only pauses the count; constant fidgeting resets it).

### Run
Double-click `AngryMoz.exe` (in `release/`). It lives in the system tray — **everything
is on the tray icon's right-click menu** (Dismiss / Pause / Settings / Buzz / Auto-start /
Help / Exit). No global hotkeys (to avoid conflicts).

### Build
Needs MinGW-w64 `g++` (WinLibs UCRT) on PATH.
```bat
build.bat
```
Produces `AngryMoz.exe`. The exe icon and tray face are the artwork in `icon.png` /
`app.ico` (regenerate from a source image with `make_icon.py`).

### Bedtime transcription text
`tengwang.txt` ships as a short placeholder. Paste the full text you want (only Han
characters are used for matching; punctuation is ignored).

---

## 中文

### 它做什么
- **白天（防久坐）：** 你用电脑时「精力」会消耗（托盘圆环从绿变红）。连续工作约 40 分钟精力耗尽，
  屏幕上就会飞出愤怒的蚊子；继续用蚊子越来越多。起身休息即可清空。
- **晚上（防熬夜）：** 到睡觉点（默认 23:30）蚊子陆续出现，越来越多、越来越大、越来越吵，逼你去睡。
  想驱散得默写一段——真实目的是让你困到放弃、乖乖去睡。
- **中英文双语**，首次运行选择，之后可在「设置」里切换。

### 久坐能量模型
- 只有**操作**（有键鼠输入）才掉电；空闲时保持不动。
- **开始工作：** 精力满时，连续操作 10 秒才判定开工，随后固定 40 分钟倒计时。
- **离开回满：** 连续静止 15 分钟精力瞬间回满（可设置）。
- 耗尽后出蚊子；**累计静止 5 分钟**清空（偶尔碰一下只是暂停计时，一直乱动则清零）。

### 运行
双击 `release/` 里的 `AngryMoz.exe`。程序常驻右下角托盘——**所有操作都在托盘图标的右键菜单里**
（驱散 / 暂停 / 设置 / 嗡嗡声 / 开机自启 / 使用说明 / 退出）。没有全局快捷键（避免冲突）。

### 编译
需要 PATH 上有 MinGW-w64 的 `g++`（WinLibs UCRT）。
```bat
build.bat
```
生成 `AngryMoz.exe`。exe 图标和托盘的蚊子脸就是 `icon.png` / `app.ico`（可用 `make_icon.py`
从源图重新生成）。

### 滕王阁序全文
`tengwang.txt` 目前是占位短文。把你想要的全文粘进去覆盖即可（程序只取其中的汉字、忽略标点）。

---

*Not affiliated with anyone; a personal well-being nudge tool. 个人健康提醒小工具。*
