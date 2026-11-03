# 构建、安装与测试原生技能

[English](../../../../../docs/eos/interfaces/skills/native/build-install-test.md)

[编写总览](README_cn.md) · [完整示例](example_cn.md) · [ABI 契约](abi_cn.md)

构建的 `.skill.so` 仅导出 `eos_skill_library_query_v2`。安装后需要重启 EOS；当前不支持热加载。

## 独立 CMake 项目

安装 EOS 和 nlohmann JSON 3.11，再导入 EOSSkill 包。`EOS::SkillCpp` 提供 C++17 门面，`EOS::SkillABI` 只提供 C 头；`eos_add_skill` 设置模块名称、隐藏可见性与安装位置。可选的 `EOS::SkillSDK` 共享目标和 `<eos/skill/support.h>` 可在工具加载软件包前检查固定 ABI 布局；技能是否准入仍由 EOS 加载器决定。部署前用 `nm` 核对唯一导出符号。

```cmake
cmake_minimum_required(VERSION 3.22)
project(example_skill LANGUAGES CXX)

find_package(EOSSkill CONFIG REQUIRED)

eos_add_skill(
  TARGET example_skill
  OUTPUT_NAME example
  SOURCES example.cpp
)
```

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/island
cmake --build build
nm -D --defined-only build/skills/example.skill.so
```

## 安装与加载

在可选 C++ 侧链中，启用的 package module 从 `isos.lock` 验证的 release 选择技能目录，并由 `eos-skill-worker` 加载；源码构建可用 `--skills-dir` 或 `EOS_SKILLS`，该 C++ 加载器递归发现 `*.skill.so`。**R04 目标 Rust 路径：** IsOS 通过 `settings.skill_libraries` 与清单列出的 `isos/skill-selection.json` 选择精确的 P01 已验证库；默认 Rust runtime 不把 `--skills-dir`/`EOS_SKILLS` 当作权限，也不扫描目录。无效 ABI、描述符、schema、声明、重复身份或缺失已声明 callback 所有者时，整个目录拒绝。仅安装 package 不会启用技能，改变选择后需要重启。用假 host 测成功、取消、格式错误、可选 callback 缺失与恰好一次完成，再进行运行时准入测试。
