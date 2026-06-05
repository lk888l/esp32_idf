# ESP-IDF C++ Template

这是一个最小的 ESP-IDF C++ 项目模板，包含模块化组件示例（`logger`）。

快速开始:

```bash
cd sample_project
idf.py set-target esp32c5
idf.py build
idf.py flash
```

约定:
- `main/` 包含 `main.cpp`。
- `components/logger/` 演示如何编写 C++ 组件（头文件放在 `include/`）。
- 使用 `clang-format` 保持代码风格一致。
