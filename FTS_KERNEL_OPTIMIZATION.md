# 全文索引内核优化与验证（2026-10-29）

## 参照与方法

按[赛题要求](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/za7cfo5cazcrihhq)阅读现有分词、索引构建、优化器及 DAS 检索/回表代码。参照为本地合并提交 `2a826d109fde3bf9c7c1668a5f12dfc55393574a`，已包含 rzx110 的 `cdcd9b08a` 词典查询优化和此前 AI 列实现。本轮没有修改官方基准、生成数据、测试 SQL、计时方式、评分脚本或基线。

正式平台提供的最近结果为综合改善 `0.1479113006245586`、raw score `29.58226012491172`。下面的对照来自本机，不能据此推算平台得分。

本地使用 Ubuntu 24.04 / WSL、Clang 17、Release `-O2`，服务固定 CPU 0–3、`cpu_count=4`、`memory_limit=4G`。两版独立数据目录和二进制；无编译或 perf 采样时运行原样官方脚本，参数为 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。每次使用新的报告文件。

## 热点与实现

### 只读行数的有限子查询

原版官方 LIMIT 查询的实际计划会按 MATCH 相关性排序、计算分数，并回表取得 `id`，外层却只使用 `COUNT(*)`。原版 LIMIT 采样可见 BM25 批量计算、倒排表解码和合并。这不等于所有返回 ID 的 LIMIT 查询都能省略排名。

`ObTransformPreProcess` 现在只在严格证明消费方只读取行数时，避免给子查询添加隐式相关性顺序：单个顶层父查询、单个派生表、唯一无参数 `COUNT(*)` 聚合、无父层过滤/分组/窗口/子查询/赋值，以及无 DISTINCT/聚合/窗口/副作用投影的子查询。只接受列或常量投影，显式排序照常保留。`COUNT(expr)`、`COUNT(DISTINCT expr)`、外层过滤、连接及可观察的 ID/分数等保留原路径。

父子查询加锁以及谓词中的随机/状态函数、UDF、用户变量、赋值等采用保守回退。回归测试检查随机谓词和函数投影仍保留评分计划。

证明依据是对子查询匹配集大小 `N`，`LIMIT L OFFSET O` 的行数为 `min(L, max(0, N-O))`，与内部行序无关。MATCH 数值阈值和其他确定性过滤仍按原有语义计算。它不依赖表名、搜索词、常数 20、数据分布或 SQL 文本。

第一轮实验发现，只省去排名仍会退化：DAS 缓存回表默认先取得 1000 行，再返回调用方所需的少量行。该实验的采样中 `ObDASCacheLookupIter::inner_get_next_rows` 累计约 87.62%，因此没有保留只消除排序的实验结果作为最终收益。

`ObDASCacheLookupIter` 进一步按调用方实际需求限制全文检索的小批量预取。带附加算子的表扫描有意在 TSC 层执行最终 LIMIT，底层 storage limit 为空，不能通过给每条倒排链设置 LIMIT 来截断候选。现在只缩小预取批次；回表过滤删除行时仍由原状态机继续补取，OFFSET 和最终输出计数保持原有位置。完整批次及普通索引回表维持原批量策略。

### IK 文档生命周期

IK 实际 TOKENIZE 采样中，`init_dict` 累计约 3.44%，可见独立 arena 分配、词典 cache handle 取得与包装构造。每篇文档的 `ObFTRangeDict` 原先忽略传入 allocator，额外创建自己的 arena。

现在词典范围包装使用调用方 allocator，常见的少量范围放在内联数组中；范围阅读器借用原有 cache block，销毁包装后再释放 pin。扩容失败会释放刚构造的阅读器，重复 reset 会清空已释放指针。已删除从未参与 IK 输出判断的停用词词典加载/包装；公共解析帮助器中的实际停用词过滤保持原有实现，并用完整 token 数组与频次比较验证。

### 构建阶段的剩余热点

本轮对原始 20000 行数据的三种索引构建另行采样，`ObSortOp::inner_get_next_batch` 累计约 37.08%，`write_ordered_slice_by_row` 累计约 56.05%，后者包含前者，不能相加。直接热点包含字符串比较、字符校验、排序比较器、解码/压缩和写入。

内部构建 SQL 显式禁用 `enable_newsort`；已有排序键编码器的类型检查也不直接接受一般 VARCHAR。因此没有通过强制开关绕过类型限制。此次 IK 资源优化也作用于构建路径，但不代表排序和 SSTable 写入瓶颈已被消除。

## 两轮完整计时

下表分别取原版 A/B、最终版 E/F 的均值；原版 B 在最终版测试之后运行，检查执行顺序影响。较早的实验 A/B/C 不混入最终版均值。

| 指标 | 原版均值 | 最终版均值 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 9.3590 s | 9.1965 s | 1.74% |
| build_ik_content_sec | 7.4090 s | 7.3180 s | 1.23% |
| build_beng_en_sec | 3.8465 s | 3.9935 s | **-3.82%** |
| tokenize_ik_avg_ms | 0.24090 ms | 0.23265 ms | 3.42% |
| tokenize_beng_avg_ms | 0.16420 ms | 0.15870 ms | 3.35% |
| query_cn_avg_ms | 1.47290 ms | 1.46325 ms | 0.66% |
| query_beng_avg_ms | 2.14060 ms | 2.04310 ms | 4.55% |
| query_mixed_avg_ms | 1.68045 ms | 1.66565 ms | 0.88% |
| query_limit_avg_ms | 2.79775 ms | 0.55085 ms | **80.31%** |

原版两次 LIMIT 分别为 2.7730/2.8225 ms，最终版为 0.5412/0.5605 ms，改善重复出现。四条查询命中数均保持 `8001 / 11000 / 7332 / 20`。

按赛题三类等权计算的**本地相对综合耗时下降为 8.23%**，包括负向项；它不是平台分数。构建总耗时仅从 20.6280 s 到 20.5195 s（0.53%），BENG 构建均值存在 3.82% 退化。SELECT 1 控制项从 0.15355 ms 到 0.13535 ms，显示仍有环境波动；其他小幅变化不宜归因于本轮代码，更不能声称构建或分词已获得稳定显著收益。

原始报告、两版 CPU 采样摘要、实际执行计划、二进制 SHA-256、编译源码校验值及结果快照校验值保存在 `unittest/storage/fts/results/local-20261029-kernel-*`。其中 `local-20261029-kernel-validation.json` 包含全部指标与计算结果。

## 正确性验证

- 5 项原生测试：分词热路径、词频分组、文档 ID 比较、词典字符查找，以及新增的范围包装 allocator 所有权/逐次分配失败测试。后者覆盖 0/1/4/5/12/80 个范围、内联容量边界、扩容失败和缓存数据不被释放。
- `compare_fts_results.py` 比较完整 token 数组、顺序、词频、文档长度、配置错误、完整匹配 ID 与数值评分，并覆盖 DML/回滚及并发。
- `compare_fts_membership.py` 的 154 组结果比较覆盖整数/隐藏文档 ID、320 个词触发的 TAAT、空值、排序、数值评分、预编译、事务及并发。
- 新增 `compare_fts_cardinality.py` 的 100 组结果覆盖 LIMIT 0、OFFSET、空/NULL/停用词搜索、`COUNT(expr)`、DISTINCT、外层过滤/连接/聚合、显式排名、数值阈值、额外过滤、预编译及 DML/回滚；另有 160 次并发查询。优化版本同时断言实际计划的评分消除及排名保留。
- 原生 mysqltest：`hot_path_equivalence`、`tokenize_function`、`simple_query`。

采样与计时分开，保留原始报告及二进制 SHA-256。源码复制构建目录没有 Git 元数据，因此官方脚本报告 `git_head=unknown, git_dirty=1`；参照提交和二进制校验值用于追溯。

在配置好依赖的 Linux Release 构建中可重跑原生测试：

```sh
cmake --build build_release --target seekdb test_fts_hot_path test_fts_word_grouping test_fts_docid_compare test_fts_dictionary_lookup test_fts_range_allocator -j4
ctest --test-dir build_release -R '^test_fts_(hot_path|word_grouping|docid_compare|dictionary_lookup|range_allocator)$' --output-on-failure
```

两版分别运行原样 `tools/benchmark/fts_large_bench.sh`，每次设置上述固定参数和独立 `OUTPUT`，再用三个 `unittest/storage/fts/compare_fts_*.py` 脚本在参照版 `--output before.json`、最终版 `--reference before.json --output after.json` 做结果比较，每个脚本各用独立的参照和输出文件。membership/cardinality 脚本的最终版同时传入 `--expect-elision`。所有比较均在隔离测试实例进行。

## 算法参考

[Lucene ScoreMode 源码](https://github.com/apache/lucene/blob/main/lucene/core/src/java/org/apache/lucene/search/ScoreMode.java)区分是否需要数值评分和是否需要穷举，支持按消费方需求避免无用工作这一设计方向。本轮的 SQL 等价性证明及实现针对 seekdb 自身的语义。

[Ding 与 Suel 的 Block-Max Indexes 论文（SIGIR 2011）](https://research.engineering.nyu.edu/~suel/papers/bmw.pdf)通过块分数上界实现安全 Top-K 跳过。seekdb 已有 `ObTextBMWIter` / `ObSRBMWIterImpl`，本轮没有把它冒充新增算法，也没有修改 BM25、跳过本应计入 COUNT 的命中文档或对可观察的排名使用近似剪枝。
