# 构建、安装与测试原生技能

[English](../../../../../../docs/eos/develop/interfaces/skills/native/build-install-test.md)

[编写总览](README_cn.md) · [完整示例](example_cn.md) · [ABI 契约](abi_cn.md)

构建的 `.skill.so` 仅导出 `eos_skill_library_query_v2`。安装后需要重启 EOS；当前不支持热加载。

## 独立 CMake 项目

安装 EOS 和 nlohmann JSON 3.11，再导入 EOSSkill 包。`EOS::SkillCpp` 提供 C++17 门面，`EOS::SkillABI` 只提供 C 头；`eos_add_skill` 设置模块名称、隐藏可见性与安装位置。部署前用 `nm` 核对唯一导出符号。

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

技能目录依次由 `--skills-dir`、`EOS_SKILLS` 指定。加载器按词典顺序扫描顶层普通、非符号链接的 `*.skill.so`；无效 ABI、重复身份或不可用原语会拒绝整个配置。用假 host 测成功、取消、格式错误、可选 callback 缺失与恰好一次完成，再进行运行时准入测试。
