# 上游提交审查与移植记录

审查日期：2026-10-08（Asia/Taipei）。直接上游：`53453450/MGL-minecraft`。
本轮上游快照：`14b0453b4cfa5aa24ac7ae08a68a467b9b1d5364`（2026-10-07）。
本地起点：`351eb21c09e9a83e507f8cfdd6cde4655cbe4a8e`。

上游历史与本地没有共同祖先，且上游已经切换至自有 GLSL/AIR 编译器和 C/C++ Metal 渲染架构。本轮按当前 ObjC / glslang / SPIR-V 架构筛选能独立移植的修复；筛查近期提交目录与模块依赖，再对候选 diff、调用方和规范逐项检查。不引入完整架构迁移，不把提交标题或 CTS 成绩当作正确性证明。

原工作区有 24 个已修改文件及未跟踪诊断资料。本轮在独立工作树验证，原有修改不包含在这些提交中。

## 纳入的来源（19 个上游提交）

| 上游提交 | 纳入范围与审查修正 |
| --- | --- |
| `5524c30f9e9c` | MultiDrawElements / BaseVertex 先验证索引类型，包括零 drawcount。解决冲突时不带入上游额外 framebuffer helper。 |
| `932aaa7eede2` | 六类 generic buffer binding 查询；补齐本地 computed-value 类型转换。 |
| `4a10f6bf7033` | GetInteger64v 的 BLEND_COLOR、PATCH_DEFAULT_INNER/OUTER_LEVEL 分量数。压缩格式列表扩展依赖当前不存在的格式数组，未纳入。 |
| `302ae5a94cf0` | GLFW 动态库依赖重写使用实际 `$(mgl_lib)`，支持自定义 build_dir。 |
| `3467c9b19ebd` | MapBuffer / MapBufferRange 的 ACCESS 与 FLAGS 一致；**修正上游 Unmap 重置 ACCESS 的错误**，解除映射保留最后 ACCESS。 |
| `b152b0a68b4d` | 缺失 compute / tess 阶段查询返回 INVALID_OPERATION。使用成功链接时阶段快照，避免后续 attach / detach 改变结果。 |
| `bdccff06c405` | Framebuffer attachment、Renderbuffer target 和默认 framebuffer 参数错误码。 |
| `c452a89f4aa6` | NamedFramebuffer draw/read/invalidate 校验；**单 DrawBuffer 与 DrawBuffers 分开验证**，保留单缓冲命令合法 FRONT 等 token。 |
| `022b633eeaba` | Buffer / image multibind 按实际目标上限校验；允许 first==limit 且 count==0，越界 INVALID_OPERATION。 |
| `6091cffc29e3` | Immutable BufferSubData 报错后立即返回；TexBuffer 格式白名单。CPU-shadow 新字段属于上游架构，当前实现已通过 copy/unmap 路径发布数据，未引入。 |
| `9506783332e3` | Sampler 名称 / pname、integer texture getter、immutable 与 multisample 校验、border 四分量查询。**修正上游整数 border color 直接 cast**，使用归一化映射。 |
| `18c8cff93689` | 仅 compressed image 的 PBO 读回：零/非零偏移、写入范围、映射状态、CPU shadow dirty 标记。不采用丢弃上传像素的部分。 |
| `ac785c7890f5` | GetTextureSubImage 拒绝 buffer / multisample texture。 |
| `e8354f4d` | Texture IMAGE_FORMAT_COMPATIBILITY_TYPE 查询 BY_SIZE；MemoryBarrierByRegion 接受 ALL_BARRIER_BITS，非法位报错后返回。 |
| `571b6507` | BindImageTexture 不隐式创建不存在的纹理、拒绝负 layer。不导入其空 mutable texture 的 ES 限制。 |
| `6fa1051f` | BindImageTextures 逐项处理，非法项不阻止后续合法绑定，采用规范 READ_WRITE / layered 默认状态。 |
| `e755862cbb76` | CopyImageSubData 比较实际 sample count，**同时覆盖 renderbuffer**；不照搬只检查 MS texture target 的不完整实现。 |
| `f91dbd6a3c7b` | Core profile 拒绝兼容模式 point pnames。 |
| `b2c00f9c5c5d` | Packed identity 路径拒绝 BGR/BGRA；RGB10_A2 BGRA 非对称颜色验证。共享指数 / packed float 的 BGR 上传按规范本就非法，保持现有 validator。 |

前四项采用带 `cherry picked from` 来源的提交；其余按当前接口移植并在提交说明保留来源，避免把上游新架构代码或错误实现带入。

## 主要未纳入项

| 提交 / 类别 | 原因 |
| --- | --- |
| `4ce475cb8dfa` | GLSL initializer 全面禁止隐式转换与规范不符，会拒绝合法 int→float / float→double 初始化。 |
| `56852988af67` | 依赖缺失的 sema 编译器；修复 inout 后仍声称函数 in 参数不可写，该注释与检查不完整。 |
| `bd392e94897b`, `d78b55496d6e`, `ac564e6ca4ba`, `650a063590c7`, `70e90dace21d` 等 AIR / frontend / tess 变更 | 依赖整套新编译器、类型 / limit / 渲染基线；当前 glslang / SPIRV-Cross 已处理其中部分语义。不能独立 cherry-pick。 |
| `14b0453b4cfa`, `94bfdc1f3eda` 与 compressed TexImage workaround | 拒绝 / remap 格式不能解决压缩存储、查询、subimage 一致性；不采纳为 CTS probe 定制的规避实现。 |
| `9b46000ef9bf` | 纹理部分回退 multisample 参数校验。TF overflow 功能还需要查询计数与 renderer 数据流，不能只增加 target slot。 |
| `94d62ed3cafb` | 本地已有 RGB8 可渲染与 RT 读回 Y 坐标换算。 |
| `669ed49aa71b` | 原工作区已有更好的属性重绑定实现：先分配新名字再更新状态，避免分配失败丢失绑定，保留该修改。 |
| `33ac11feaa24`, `0e517214bea1` | 当前 robust uniform 接口仍为 stub；error 去重需要同步所有错误计数调用方，不能孤立改队列。 |
| 其他 renderer 迁移、AIR 特定功能、上游测试 / 文档 / 构建迁移 | 不适用于当前实现，或需要独立功能项目及更完整验证；本轮没有声称审查或纳入全部历史提交。 |

## 验证

- `make -j8 lib`：Core、ES、GLFW 构建通过。复用现有 external 依赖和本地 JNI include 配置；没有更改依赖源码或安装软件。
- `make test-upstream-api`：四个 headless suite 全部 PASS。覆盖 query 类型转换与写入分量 / 边界、buffer 映射状态、immutable 写入、链接阶段快照、DSA 校验、texture / sampler 校验、PBO 偏移读回、image multibind，以及实际 packed pixel 上传 / 读回。
- `test_mapped_vertex`：非 persistent map/unmap 与 persistent VBO fence 前后像素均 PASS。
- 现有 `multi_draw_elements` 黄金图测试 FAIL。对本轮修改前的 `351eb21` 单独编译运行也 FAIL；两者实际 TGA **逐字节相同**，SHA-256 均为 `19091f9116ffb2afe09fd81bc7c269916b0e84808334b4f3ac42d5244955da86`。这是已有黄金图差异，未更新黄金图掩盖结果。
- 提交范围 `git diff --check` 通过。
- 合入 main 后恢复原有 24 个未提交修改，再执行 `make -j8 lib test-upstream-api`，Core / ES / GLFW 及四组测试仍全部通过。原修改反向补丁检查和原工作区状态清单对比通过，暂存区为空。
- 未运行完整 GL46CTS 或 Minecraft / Iris 实机游戏流程；上述测试不能证明全部游戏场景兼容。

## 规范依据

- [OpenGL 4.6 Core specification](https://registry.khronos.org/OpenGL/specs/gl/glspec46.core.pdf)：buffer map/unmap、framebuffer、packed pixel matching formats、image copy 和 stage queries。
- [GLSL 4.60 specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)：函数参数与 initializer conversion。
- [glTexParameter](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glTexParameter.xhtml)、[glGetTexParameter](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glGetTexParameter.xhtml)：multisample 参数限制与归一化 integer border queries。
- [glBindImageTextures](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBindImageTextures.xhtml)、[glCopyImageSubData](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glCopyImageSubData.xhtml)：逐项绑定、采样数一致性。
