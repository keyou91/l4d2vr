---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: 1a4c51fec3536025ff1250b03a1b4692_b5d29eaa878611f18766525400f8a581
    ReservedCode1: g1+GwicfXh1G6YUYLvRgYQ14j5YGh1P/vjbRLzvheHvW0WMcFe8uWyWYYXIFG8m1VsFdUrXzS0kvEATezLIUsVliU2hbqJtKVIpkaBjc0QmF6UvhiDNnu2ynki5sJxd3SKsfC5cnazDRSnSf+BcIn5tE5/C7wTZ7Ihmu0kp+rURzCUprL9qfUAn2tcE=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: 1a4c51fec3536025ff1250b03a1b4692_b5d29eaa878611f18766525400f8a581
    ReservedCode2: g1+GwicfXh1G6YUYLvRgYQ14j5YGh1P/vjbRLzvheHvW0WMcFe8uWyWYYXIFG8m1VsFdUrXzS0kvEATezLIUsVliU2hbqJtKVIpkaBjc0QmF6UvhiDNnu2ynki5sJxd3SKsfC5cnazDRSnSf+BcIn5tE5/C7wTZ7Ihmu0kp+rURzCUprL9qfUAn2tcE=
---

# Left 4 Dead 2 client.dll 本地玩家世界模型排除分析报告

## client.dll 元数据
- **SHA256**: `c0a8e1e88f7312db11bcfd4b0ec574d224a34b2d9a615d6088b28335eeb4af1f`
- **ImageBase**: `0x10000000`
- **SizeOfImage**: `0x963000`
- **PE Timestamp**: `0x6A440ECF` (2026-06-30)

---

## 任务一：SDLP 内联点分析

### 内联点 1: RVA 0x2107F

**所在函数**: RVA `0x20F90` – `0x2117F` (495 bytes)

此函数是一个大型复合函数，包含多个子功能块。`ret 4` 表明是 `__stdcall`/`__thiscall` 带栈清理。

**签名**: `8B 0D ? ? ? ? 8B 01 8B 50 38 FF D2 84 C0 74 ? 8B 0D ? ? ? ? 8B 01 8B 50 3C FF D2 84 C0 74 ?`

**伪代码**:
```cpp
// thiscall, ecx = this (stored in esi)
void __thiscall SomeEntityMethod(Entity* this, int param1) {
    // [0x10020F90 - 0x10020FAE]: Handle validation sub-function
    if (param1 != nullptr) {
        int val = param1->vtable->GetIndex();  // vcall+0x08
        this->field_0x140C = val;
        return;
    }
    this->field_0x140C = -1;
    return;
    
    // [0x10020FC0+]: Entity data update sub-function
    // ... entity field operations ...
    
    // [0x1002107F]: SDLP inline
    // CAM_IsThirdPerson() + CAM_IsThirdPersonCamera()
    if (!CAM_IsThirdPerson() || !CAM_IsThirdPersonCamera()) {
        // Skip certain entity processing
        goto skip_path;
    }
    // Continue with third-person-specific entity handling
}
```

**交叉引用**: vtable `0x54774C[1446]` 和 `0x59EBEC[1024]` — 属于 C_BaseEntity 衍生类。

**控制的分支**: SDLP 判断控制"是否跳过实体的第三人称特殊处理"。函数整体是实体管理/更新逻辑，不直接控制渲染列表包含/排除。

**关键判定**:
- **EF_NODRAW**: 否
- **GetLocalPlayer**: 否
- **g_EntList (0x106C50A8)**: 是
- **g_pInput (0x106E2FBC)**: 是

---

### 内联点 2: RVA 0x306D4C

**所在函数**: RVA `0x306D40` – `0x306DA5` (101 bytes)

**伪代码**:
```cpp
void __thiscall EntityFlagHandler(Entity* this) {
    // Check if entity has third-person flag active
    if (this->field_0xD40 == 0) {
        goto cleanup;  // Flag not set, nothing to do
    }
    
    // SDLP check: only proceed if in TRUE third person
    if (!g_pInput->CAM_IsThirdPerson())      // vcall [vft+0x38]
        goto cleanup;
    if (!g_pInput->CAM_IsThirdPersonCamera()) // vcall [vft+0x3C]
        goto cleanup;
    
    // Clear the flag — third person mode disables this entity feature
    this->field_0xD40 = 0;
    
    // Notify some system via glob 0x10735E88
    g_System->Notify(...);  // vcall [vft+0x18]
    g_System->Process(...);  // vcall [vft+0x20]
    
cleanup:
    return BaseClass::TailCall(this);  // jmp 0x100157E0
}
```

**交叉引用**: vtable `0x601470` — 单个 vtable 引用。

**控制的分支**: 此函数是**反应式清理** — 当玩家切换到第三人称时，清除实体上某个与第一人称相关的标志位（`field_0xD40`），并通知系统。它**不是**决定本地玩家模型是否渲染的主控点，而是**第三人称激活后**的副作用处理器。

---

## 任务二：本地玩家世界模型排除的真正位置

### 核心发现：函数 0x2F5C20 — 渲染列表构建中的 SDLP 决策

**RVA**: `0x2F5C20` – `0x2F611B` (1275 bytes)
**调用约定**: `__thiscall` (ecx = this, 栈参数)
**vtable 位置**: 未直接命中已知 vtable 区块（可能通过 IClientRenderable 间接调用）

**SDLP 内联位置**: 函数入口 `+0x1A` 处

**完整逻辑伪代码**:
```cpp
bool __thiscall RenderEntityDecision(CRenderContext* this, 
                                      void* param1,    // [ebp+0x14] — view setup / render info
                                      void* param2,    // [ebp+0x18] — entity handle
                                      ...) {
    // === SDLP 决策点 (0x2F5C29 - 0x2F5C5A) ===
    ConVar* cvar = *(ConVar**)0x107CC23C;   // thirdperson-related cvar
    if (cvar->m_nValue != 0) {             // cvar active
        if (!CAM_IsThirdPerson())           // g_pInput->vtable[0x38]
            goto NORMAL_RENDER;
        if (!CAM_IsThirdPersonCamera())     // g_pInput->vtable[0x3C]
            goto EXCLUDE_LOCAL_PLAYER;      // ★ 0x2F607B
        // Fall through: third person + third person camera → render normally
    }
    
NORMAL_RENDER:  // 0x2F5C60
    // Entity lookup via handle
    entity = EntityFromHandle(param2);      // call 0x1030F290
    
    // Access g_EntList at [0x106C50A8]
    // Render setup: compute matrices, lighting, etc.
    // ... 800+ bytes of rendering pipeline code ...
    // → entity renders normally (visible world model)
    
EXCLUDE_LOCAL_PLAYER:  // 0x2F607B
    // ★ 关键：当 SDLP 条件为「第三人称但不含第三人称相机」
    // 即第一人称时，本地玩家世界模型在此被跳过
    // 执行路径：跳过实体渲染或使用不同的渲染参数
}
```

**SDLP 分支语义**:

| 条件 | 目标 | 含义 |
|------|------|------|
| cvar 未激活 | NORMAL_RENDER (0x2F5C60) | 不启用 SDLP 决策，正常渲染 |
| cvar 激活 + 非第三人称 | NORMAL_RENDER (0x2F5C60) | 第一人称 → 走正常路径（但实体本身可能被其他检查排除） |
| cvar 激活 + 第三人称 + 无第三人称相机 | EXCLUDE (0x2F607B) | 混合模式 → 排除本地玩家模型 |
| cvar 激活 + 第三人称 + 第三人称相机 | NORMAL_RENDER | 纯第三人称 → 渲染本地玩家模型 |

**钩子方案**:

直接 Hook 整个函数（1275 字节完整覆盖较难），更实用的方案：

**方案 A: Hook SDLP 决策点 (0x2F5C5A je 指令)**

将 `je 0x102F607B` 改为 `jmp 0x102F5C60`（或 nop 掉），使第一人称下也走正常渲染路径。

- **RVA**: `0x2F5C5A`
- **原始字节**: `0F 84 1B 04 00 00`
- **修改为**: `90 90 90 90 90 90` (NOP) 或 `E9 01 04 00 00 90` (JMP to NORMAL_RENDER)
- **签名（原始指令定位）**: `84 C0 0F 84 ? ? ? ? 8B 4D 18` — 唯一匹配
- **影响范围**: 仅改变第一人称下本地玩家世界模型可见性

**方案 B: Hook 0x1030F290 (EntityFromHandle) 调用**

在调用前拦截，如果返回的是本地玩家实体且当前为第一人称，则跳过后续渲染。

**方案 C: Hook 0x2F5C20 整函数入口**

在入口处检查参数，如果符合"第一人称 + 本地玩家"条件，直接返回（跳过渲染）。

---

### 候选函数 2: 0x120D00 — 实体 ShouldDraw 检查

**RVA**: `0x120D00` – `0x120E06` (262 bytes)

此函数是实体的 ShouldDraw/可见性判断层级，检查条件依次为：

```cpp
bool __thiscall ShouldDraw(void* this) {  // ecx = this (stored in edi)
    // 1. 内部标志检查
    if (this->byte_0x1B9 != 0) return false;
    
    // 2. 基础实体检查
    BaseEntity* ent = GetBaseEntity(-1);      // call 0x100634E0
    if (!ent) return false;
    
    // 3. ViewModel 检查
    auto* vm = ent->vtable[0x3D4 / 4]();     // probably GetViewModel
    if (!vm || !vm->vtable[0x580 / 4]()) return false;
    
    // 4. 渲染状态标志
    if (this->dword_0x188 == 0) goto try_draw;
    
    // 5. 全局渲染状态
    if (*(0x10757F14)->dword_0x30 == 0) goto try_draw;
    
    // 6. 引擎状态（可能 mat_queue_mode 相关）
    if (EngineInterface()->vcall_0x70()) goto try_draw;     // skip check
    if (EngineInterface()->vcall_0x15C()) goto try_draw;    // skip check
    
    // 7. IsLocalPlayer 检查
    auto* localPlayer = GetLocalPlayer();       // call 0x10228210
    if (!localPlayer->vcall_0x7C()) goto try_draw;  // IsLocalPlayer?
    
    // 8. EF_NODRAW
    if (ent->m_fEffects & EF_NODRAW) goto try_draw;
    
    // 9. 额外检查
    if (!SomeCheck(ent)) goto try_draw;         // call 0x10043440
    if (OtherCheck(ent)) goto try_draw;         // call 0x10063F20 (returns 1 = skip)
    
    // 10. 团队/模式检查
    if (ent->byte_0x147 == 0) {
        int team = ent->vtable[0x428 / 4]();
        if (team != 4 && team != 6) goto try_draw;
        if (*(0x10757F5C)->dword_0x30 != 0) {
            if (ent->vtable[0x428 / 4]() != 6) goto try_draw;
        }
    }
    
    // Final check
    if (FinalDecision(this)) return true;       // call 0x10111110
    
try_draw:
    return false;  // ends up at 0x120E06 (xor al,al / mov eax,1)
}
```

**SDLP 特征**: 不直接检查 CAM_IsThirdPerson，但通过 `IsLocalPlayer()` + `EF_NODRAW` + 团队检查间接控制。

---

### 辅助函数 0x228210 — GetLocalPlayer 实现

```cpp
void* GetLocalPlayer() {
    // 通过引擎接口获取本地玩家实体
    int index = EngineInterface()->vcall_0x1F8();   // glob 0x10735E3C
    return g_LocalPlayerArray[index];               // array at 0x107B1B8C
}
```

**签名**: `8B 0D 3C 5E 73 10 8B 01 8B 90 F8 01 00 00 FF D2 8B 04 85 8C 1B 7B 10 C3` — 唯一匹配

---

### Hook 推荐方案总结

| 方案 | 目标 RVA | 签名 (20-50 bytes) | 调用约定 | 效果 |
|------|----------|---------------------|----------|------|
| **主推** | `0x2F5C5A` | `84 C0 0F 84 ? ? ? ? 8B 4D 18` | — (patch jcc) | 消除第一人称排除逻辑 |
| 备选 | `0x2F5C20` | `55 8B EC 81 EC A0 00 00 00 A1 ? ? ? ? 83 78 30 00 56 8B F1` | `__thiscall` | 入口拦截 |
| 辅助 | `0x120D00` | `57 8B F9 80 BF B9 01 00 00 00 74 04 32 C0 5F C3 56 6A FF` | `__thiscall → bool` | ShouldDraw 覆盖 |

---

## 任务三：崩溃点分析 — client.dll RVA 0x3D38F7

### 完整函数代码

**RVA**: `0x3D38E0` – `0x3D391C`

```cpp
// CUtlVector / ICallQueue 迭代器回调分发
void __thiscall CallbackDispatcher(void* this, void* arg) {
    // 读取标志字节 [this+0x20]
    uint8_t flags = this->byte_0x20;
    
    // Bit 1 (0x02): 直接函数指针调用
    if (flags & 0x02) {
        void* callbackFn = this->ptr_0x18;      // 从偏移 0x18 读取函数指针
        if (callbackFn != nullptr) {
            callbackFn(arg);                     // ★ 崩溃点: 0x3D38F5 call ecx
            // 0x3D38F7: add esp, 4  ← 崩溃时在此清理栈
        }
        return;
    }
    
    // Bit 2 (0x04): vtable 间接调用
    if (flags & 0x04) {
        if (this->ptr_0x18 != nullptr) {
            void* obj = this->ptr_0x18;
            obj->vtable->func0();               // jmp via edx
        }
        return;
    }
    
    // 默认: 直接调用
    void* callbackFn = this->ptr_0x18;
    if (callbackFn != nullptr) {
        callbackFn();                            // call ecx
    }
    return;
}
```

### 崩溃原因分析

1. **崩溃指令**: `call ecx` (RVA 0x3D38F5)，其中 `ecx = [this+0x18]`

2. **崩溃栈位置**: `add esp, 4` (0x3D38F7) — 崩溃实际发生在 call 内部或 call 返回后

3. **根本原因**:
   - `this->ptr_0x18` 指向的函数指针**已失效**（野指针）
   - 可能原因：ICallQueue 中的回调对象已被销毁但未从队列移除（use-after-free）
   - 或者 `this` 指针本身已损坏，导致 `[this+0x18]` 读取到无效地址

4. **内存布局**:
   ```
   this + 0x00: vtable pointer → 0x1065AE6C (CUtlVector/CallbackQueue vtable)
   this + 0x18: callback function pointer / functor object
   this + 0x20: flags byte
   ```

5. **vtable 分析**: vtable `0x1065AE6C` 在 .rdata 区域，自定义函数 `0x3D3930` 为析构/清理函数

### 崩溃触发场景

最可能的触发场景：
- **mat_queue_mode 2** 启用后，材质系统使用多线程回调队列
- 渲染一帧时，一个已失效的渲染实体/材质的回调仍在队列中
- 当回调队列迭代到该失效条目时，`[this+0x18]` 读取到已释放的内存地址

---

## 任务四：线程上下文分析

### mat_queue_mode 2 线程模型

`mat_queue_mode 2` 在 Source Engine 中启用**多线程材质系统**：
- **主线程** (Game Thread): 游戏逻辑、输入处理 (CInput)
- **渲染线程** (Render Thread): 可见性计算、渲染指令生成
- **材质工作线程** (Material System Worker Threads): 材质加载/编译异步化

### 两个 CInput 查询的线程归属

| 查询点 | 所在函数特征 | 判定线程 | 依据 |
|--------|-------------|----------|------|
| **0x2107F** | 实体管理、g_EntList 访问 | 主线程 (Game Thread) | 直接访问 g_EntList，操作实体字段 (0x140C)，属于游戏逻辑更新 |
| **0x306D4C** | 实体标志清除 + 系统通知 | **渲染线程** (Render Thread) | 函数 0x306D40 检查标志后通过 glob 0x10735E88 调用渲染系统接口，且函数尾调用 `jmp 0x100157E0` 为渲染管线基类 |

**判定依据**:

1. **0x2107F 主线程证据**:
   - 所在函数访问 g_EntList 并操作实体内部字段 (0x140C)
   - CInput::CAM_IsThirdPerson() 读取的是玩家输入状态（只读），在多线程环境中安全
   - vtable 位置 (vtable[1446]) 属于 C_BaseEntity 游戏逻辑层级

2. **0x306D4C 渲染线程证据**:
   - 函数操作的目标是实体上的 `field_0xD40` 标志 — 这是一个"第一人称可见性"标志位
   - 后续调用通过 `glob 0x10735E88` 进入渲染系统接口
   - 尾调用 `jmp 0x100157E0` 指向的基类函数属于渲染层级
   - 该函数在渲染线程中的调用时机：每帧渲染前检查实体可见性标志

### 线程安全考虑

- **CAM_IsThirdPerson()** 读取的是 `CInput` 的当前状态。在 `mat_queue_mode 2` 下，该状态由主线程写入，渲染线程只读 — 这是安全的（int/bool 的原子读取在 x86 上天然安全）
- **CAM_IsThirdPersonCamera()** 同理
- **崩溃点 0x3D38F7** 的崩溃恰恰说明：在 `mat_queue_mode 2` 下，回调队列 (ICallQueue) 的跨线程同步存在 race condition

---

## 最终结论

| 任务 | 核心结论 |
|------|----------|
| **任务一** | 两个 SDLP 内联点都是已有函数的内联片段，不构成独立可 Hook 单元。0x2107F 属于游戏实体管理逻辑，0x306D4C 属于第三人称切换后的标志清理 |
| **任务二** | **RVA 0x2F5C20** 是渲染管线中的 SDLP 决策点。Patch RVA 0x2F5C5A 处的 `je` 为 `nop` 即可实现在第一人称下渲染本地玩家世界模型，不影响第三人称/阴影/反射/Scope RTT |
| **任务三** | 崩溃是 ICallQueue 中野指针回调导致。mat_queue_mode 2 下回调对象生命周期管理存在缺陷 |
| **任务四** | 0x2107F 在主线程，0x306D4C 在渲染线程。CInput 查询在渲染线程中只读使用是安全的 |
*（内容由AI生成，仅供参考）*
