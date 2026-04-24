# Engine 动态化方案总结

## 问题
在 commit `34471fc` 中，`engine` 从 STATIC 改成 SHARED 库后，Windows Debug 模式下出现断言失败：
```
Assertion failed: ((has_single_bit(mod)) && ("Value must be a power of two"))
```

**根本原因**：EnTT 模板跨 DLL 边界实例化导致内存访问错误。

## 解决方案：显式模板实例化

### 修改的文件

1. **src/engine/CMakeLists.txt**
   - 保持 `add_library(engine SHARED ...)`
   - 添加 `GameAPI_ExplicitInstantiation.cpp` 到源文件列表
   - 添加 `QGAME_ENGINE_EXPORTS` 定义
   - 启用 `WINDOWS_EXPORT_ALL_SYMBOLS`

2. **src/engine/Export.h**
   - 恢复 DLL 导出/导入宏定义

3. **src/engine/api/GameAPI.h**
   - 保持模板实现（内联）
   - 在文件末尾包含 `GameAPI_ExternTemplates.h`

4. **src/engine/api/GameAPI_ExternTemplates.h** (新增)
   - 声明所有 `extern template` 实例化

5. **src/engine/api/GameAPI_ExplicitInstantiation.cpp** (新增)
   - 显式实例化所有组件类型的模板函数

### 工作原理

```
┌──────────────────────────────────────┐
│  game.exe                            │
│  ├─ main.cpp                         │
│  └─ api.addComponent<Transform>(...) │
│       ↓                              │
│       看到 extern template 声明       │
│       → 不实例化，生成未定义符号       │
└──────────────────────────────────────┘
         ↓ 链接时解析符号
┌──────────────────────────────────────┐
│  engine.so / engine.dll              │
│  └─ GameAPI_ExplicitInstantiation.cpp│
│       ↓                              │
│       显式实例化所有组件类型            │
│       → 导出符号供 game.exe 使用       │
└──────────────────────────────────────┘
```

### 如何添加新组件

**步骤1**：在 `GameAPI_ExternTemplates.h` 中添加：
```cpp
extern template QGAME_ENGINE_API MyComponent& 
    GameAPI::addComponent<MyComponent>(entt::entity, MyComponent);
extern template QGAME_ENGINE_API MyComponent& 
    GameAPI::getComponent<MyComponent>(entt::entity);
extern template QGAME_ENGINE_API bool 
    GameAPI::hasComponent<MyComponent>(entt::entity) const;
```

**步骤2**：在 `GameAPI_ExplicitInstantiation.cpp` 中添加：
```cpp
template MyComponent& GameAPI::addComponent<MyComponent>(entt::entity, MyComponent);
template MyComponent& GameAPI::getComponent<MyComponent>(entt::entity);
template bool GameAPI::hasComponent<MyComponent>(entt::entity) const;
```

### 验证

检查 DLL 中是否导出符号：
```bash
nm -C build/src/engine/libengine.so | grep addComponent
```

应看到类似输出：
```
0000000000054440 W engine::Transform& engine::GameAPI::addComponent<engine::Transform>(...)
```

检查可执行文件是否依赖 DLL：
```bash
ldd build/game/game | grep engine
```

应看到：
```
libengine.so => /path/to/libengine.so
```

### 优点
- engine 可以作为 SHARED 库使用
- 无需禁用 EnTT 断言
- 跨平台兼容（Linux/Windows）

### 缺点
- 必须预先知道所有组件类型
- 添加新组件需要重新编译 engine

### 替代方案

如果需要动态添加组件（不在编译时确定），可以考虑：
1. 使用静态库（STATIC）
2. 实现类型擦除的组件系统
3. 使用插件架构
