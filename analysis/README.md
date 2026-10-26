# analysis 目录说明

本目录用于存放逆向分析过程中的提取物。**内容不随仓库分发**（见 `.gitignore`）。

## 原版安装包逆向（2026-10-23）

对第三方发布的 Revit 版本查看工具安装包做了格式分析，结论记录在
[docs/notes.md 第一章](../docs/notes.md)。

| 文件 | 大小 | 判定 |
|---|---|---|
| `setup.exe` | 566,272 B | Visual Studio Bootstrapper，只负责解包并启动 MSI |
| `安装包项目.msi` | 548,352 B | VS Installer Project 产物，x86 / 简体中文 |
| 内嵌 PE | 305,152 B | 文件描述 `Microsoft (R) Visual Studio UrlConvert custom action`，版本 `16.0.31603.11111` —— 即 `DPCA.DLL`（Visual Studio CustomActions），**不是** Revit 工具本体 |

**结论：该安装包是半成品或占位包，真正的功能 DLL 未随包提供。**
本项目核心逻辑全部自研，未使用上述任何二进制内容。

## 为什么不入库

内嵌 PE 是 Microsoft 的 `DPCA.DLL`，属于第三方版权作品，不适合再分发。
需要复现分析时，自行从原安装包提取即可 —— 方法见
[docs/notes.md](../docs/notes.md) 第一章。
