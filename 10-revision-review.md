# 修改版与我们提交版的逐节对比核查

> 这份文档是把您返回的修改版和我们提交前那一版逐节对了一遍的结果,交给您决定哪些改回、哪些保持。每一条都写成同一个形状:您那一版的原句、我们那一版的原句、我们查到的证据、以及一个尽量小的改法。表述层面的改动一律以您为准,只有确定的语法错误才列出来。
>
> 对比的两份文本是您返回的 PDF 和我们 camera-ready 编译出的 PDF(源文件 `abs.tex`、`bg2.tex`、`mot2.tex`、`design2.tex`、`eval2.tex`)。凡是说「代码里是怎样」的地方,证据都取自实现仓库,文中给出的是仓库内的相对路径和行号。
>
> **最要紧的一条在 2.2**:论文原来说 Éxitos 用 syscall tracepoint 拦截,这个提法不成立;实际拦截程序是交给第三方的用户态 eBPF 运行时 bpftime 执行的,而这一版把点明这件事的那句话和对应的参考文献一起删掉了。

## 一、先说结论

这一版把 9 页压到 8 页,压缩几乎全部发生在第四节 Design:我们那一版该节抽出的正文有 264 行,这一版只剩 153 行。第二节 Background 和第三节 Motivation 逐词相同,没有删掉任何测量、前提或交叉引用;第六节 Conclusion 逐词相同;所有数字(2.4×、49.2%、42.3%、24.8%、47.4%、2.1×、1.6×、63.1% 等)一个都没有被改动。所有图表都在,都被正文引用过,章节编号和 `Section` 交叉引用全部对得上;文献表本身自洽,没有一条引用指向不存在的条目。

要请您裁定的问题分四类:

1. **这一版新引入的正文问题,3 条**(第二节)。其中 2.2 那条最要紧:论文说 Éxitos 用 syscall tracepoint 拦截,这个提法不成立 —— tracepoint 只能观察不能改道,实现里也没有装任何内核 tracepoint;真实做法是把 eBPF 程序交给第三方的**用户态** eBPF 运行时 bpftime 执行。原来那句「The interception programs are executed by bpftime [23]」正是点明这件事的,这一版把它连同参考文献 [23] 一起删了,于是全文再没有出现 bpftime,而「eBPF」和「syscall tracepoint」两处还在,读者会读成内核态 eBPF,**这恰好是系统唯一没有做的事**。另外图 8 的标题被换成了图 9 的内容,审稿人一眼可见。
2. **被删掉、删掉之后审稿人的标准问题没人回答的内容,6 段**(第三节),集中在第四节 D 小节。第四节 A 小节仍然承诺 scalability / comprehensiveness / compatibility 三个维度,而 D 小节现在只对应得上两个。
3. **两版都有、这一版没有引入、但代码不支持或出处不对的说法**(第四节和 8.3),这些是我们自己的问题,列出来是因为第四节被压缩之后它们占比更大。
4. **文献层面的问题**(第五节和第八节):这一版新引入 7 条引用错误,其中 4 条建议必须改;另有 8 条「文献真实存在、但支撑不住它所在那句话」的问题,两版都有。

---

## 二、这一版新引入的问题(按审稿危害排序)

### 2.1 图 8 的标题被换成了图 9 的内容

这一版:`Fig. 8: Impact of bypassing different log types.`
我们版:`Fig. 8: Impact of preallocation size.`

图 8 本身在这一版里没有改动:横轴是 `Append I/O Size`(4K–128K),纵轴是 `Throughput (MB/s)`,四条曲线是 `No Prealloc / 64K-async / 256K-async / 1M-async`,全是预分配尺寸,不是日志类型。标题下面三行的正文还写着「Figure 8 shows the curves for three preallocation sizes」。「bypassing different log types」是图 9 的内容(Vanilla / BypassD / Éxitos-B / Éxitos-R / Éxitos)。

**这是一次回退,不是新想法**:论文仓库的提交 `d4c067a`(提交说明「sub title of figure 8」)已经把 `eval2.tex:379` 从 `\caption{Impact of bypassing different log types.}` 改成了 `\caption{Impact of preallocation size.}`,这一版用的是改之前的标题。

**最小改法**:改回 `Fig. 8: Impact of preallocation size.`



### 2.2 拦截机制:tracepoint 这个提法不成立,而点明真实运行时的那句话被删掉了

**这一条是整份文档里风险最高的一条,请特别看一下。**

这一版第四节 D 小节的完整段落是:「To intercept file operations, Éxitos employs syscall tracepoints that are static kernel instrumentation points characterized by minimal runtime overhead. Tracepoints avoid the cost of dynamic instrumentation, making them ideal for latency-sensitive operations [22].」我们那一版在同一段里还有四句,现在全部不在了,其中第一句是「The interception programs are executed by bpftime [23].」

**tracepoint 这个提法本身不成立,两版都错,原因有两层。** 第一层是机制上做不到:tracepoint 只能观察,不能改道,而这里要做的是把一次 `write` 从内核原路上拿走、送到别处去。实现仓库 `src/intercept.c:3-6` 的注释就是为这件事写的 ——「The paper describes redirecting a write from inside the kernel, at a syscall tracepoint. A tracepoint cannot do that: it observes, it does not divert. So the decision is made here, in user space」。第二层是实现上根本没有装:两个前端都在用户态,一个是 `src/preload.c`,用 `LD_PRELOAD` 做符号插入;另一个是 `src/bpftime_hook.c`,把进程自己代码段里的 `syscall` 指令改写掉、转到用户态的钩子(被钩住的调用列在 `bpftime_hook.c:361-367`)。全树没有任何地方安装内核 tracepoint。

**真实的情况是:拦截程序是 eBPF 程序,但由第三方的用户态 eBPF 运行时 bpftime 执行,不是内核里的 eBPF 子系统。** bpftime 是一个把 eBPF 程序放到用户态跑的运行时(Zheng 等,OSDI'25),我们是把它当作一个外部依赖引进来用的,论文里那句「The interception programs are executed by bpftime [23]」正是用来点明这件事的。

**为什么这一版比原来更危险。** 这句话连同参考文献 [23] 一起被删之后,全文再也没有出现 bpftime 这个词;剩下的是「syscall tracepoints ... static kernel instrumentation points」这一句,加上引言贡献列表里的「leveraging eBPF」和结论里的「utilizes eBPF」。审稿人把这三处连起来读,得到的是「内核态 eBPF 挂在 syscall tracepoint 上」——**这恰好是系统唯一没有做的事**。而开源脚注还在,审稿人打开仓库看到的是一个 `LD_PRELOAD` 的 `.so` 和一个改写 `syscall` 指令的 `.so`,没有 tracepoint,也没有内核模块。这会被读成对实现的虚假陈述,代价远高于本文档里其它任何一条。

**最小改法:按我们那一版恢复这一段,并且明确写出 bpftime 是一个用户态 eBPF 运行时。** 建议的英文写法:

> *Runtime interception.* Éxitos intercepts file operations entirely in user space. The interception programs are eBPF programs executed by bpftime [23], a third-party user-space eBPF runtime; Éxitos requires no kernel module and no patched kernel. Éxitos obtains a list of log files from a configuration file already used by the database and intercepts only `write` and `fdatasync` requests to the listed log files. All other requests continue through the conventional I/O path. This design requires no changes to the database source code. Before entering the shortcut path, Éxitos verifies that the request falls within a block range registered in the Maco structure; otherwise, it falls back to the conventional path.

配套要做的两件小事:

- **把参考文献 [23](bpftime,Zheng 等,OSDI'25,pp. 557–574)加回文献表**。这一条在 camera-ready 阶段核过,对过该项目仓库的 `CITATION.cff` 和 DBLP 条目 `conf/osdi/ZhengY0HL0Q25`。
- **凡是正文写「eBPF」的地方都补一个限定词**,让读者不会默认成内核 eBPF:引言贡献列表里的「Éxitos uses eBPF to intercept and redirect persistent writes」和结论里的「Éxitos utilizes eBPF to transparently redirect and transform file operations」,都建议写成「eBPF programs run in user space by bpftime [23]」这样的形式。这两处两版一字不差,是我们自己的问题,不是这一版引入的,但在 bpftime 那句被删之后,它们是仅存的关于机制的陈述,更容易被误读。

### 2.3 严格模式被描述成设计文档明确否决过的那个机制

这一版:「The strict mode follows MAC compliance for enhanced protection through inode attribute verification via ioctl interfaces at every operation.」

**代码做的是另一件事**:严格模式在每次接管之前,在被拦截的文件描述符上发一个长度为 0 的 `pwrite`,让内核自己的逐写权限门做判断。`src/intercept.c:1885-1889`:

```c
if (tx->ctx->strict &&
    exitos_internal_pwrite_call(tx->fd, buf, 0, off) != 0) {
    r->passed++; r->kernel_holds = 1; ...
    return EXITOS_PASS;
}
```

用 ioctl 查 inode 属性不只是「另一种实现」,它正是 `docs/exitos-s-design.md` 第 2 节(「Why not a user-space attribute check」)评估之后否决掉的那个方案,理由是 inode 属性属于自主访问控制那一侧,查不出 SELinux 策略被收紧。所以这句话是在用一个按构造就做不到的机制去声称 MAC 合规。同一句里还有两处细节不对:这个探测只在写上做,`fdatasync` 是故意不探测的(普通 `fdatasync` 路径上没有对应的安全模块文件钩子),所以不是「at every operation」;严格模式还额外开启了逐写的 `fstat` 描述符身份检查。

**最小改法**:「The strict mode follows MAC compliance for enhanced protection: before every taken-over write it issues a zero-length write on the same descriptor, which runs the kernel's own per-write permission gate — the security module's `file_permission` hook included — and the write falls back to the conventional path unless that gate allows it.」


---

## 三、被删掉、建议至少恢复一部分的内容

这些全部在第四节 D 小节。第四节 A 小节仍然承诺「we optimize Éxitos in the dimensions of scalability, comprehensiveness, and compatibility (Section IV-D)」,而 D 小节现在只剩 Optimizations、Permission checks、Compatibility 三个小标题,`comprehensiveness` 这一维没有任何内容对应。**要么恢复下面几段中的一段,要么把 A 小节承诺里的 `comprehensiveness` 去掉。**

| 被删的段落 | 现在没人回答的审稿人问题 | 恢复的话,代码上站得住吗 |
|---|---|---|
| **Crash consistency**(整段) | 「你在写路径上绕过了文件系统,崩溃之后 ext4 的元数据和你的裸写怎么对得上?」 | 站得住,而且是全文最好写的一段。Maco 每次注册时从 FIEMAP 重建(`src/extent.c` 的 `exitos_extent_load_maco`),是纯内存数组、不落盘(`src/maco.c:1-36`),运行期映射变化只经过写日志的 move-extent,异步准备器从不在写者已经越过的位置以下捐赠(`src/donor_async.c:68-84`)。诚实的答案就是「我们不引入任何自己的持久状态」。 |
| **Stale mappings**(整段) | 「文件在你缓存映射期间被截断、轮转、改名了怎么办?」 | 站得住。截断和日志轮转时 `maco_invalidate()`(`include/exitos_maco.h:26`),每种 `fallocate` 模式之前先失效映射(`frontend_fallocate.c:431-434`),逐写的描述符身份检查 `EXITOS_VERIFY_IDENTITY`,以及一个专门的 LBA 边界结构,其目的写在 `intercept.c:29-41`。 |
| **Error reporting**(整段) | 「数据库现在看到的错误会不会不一样?」 | **不要按原样恢复**。我们那段写的是把设备完成状态映射成同样的 errno,代码不是这么做的:任何裸写失败一律返回 `EXITOS_PASS`,由应用重新走内核路径产生 errno(`src/intercept.c:1893-1900`)。删掉它是对的。要写就写代码真实的行为。 |
| **Portability**(整段,连带参考文献 [26] XFS range-exchange) | 「这套东西只能在 ext4 + NVMe 上用吗?Btrfs、F2FS 上会怎样?」 | 站得住,而且便宜。异地更新的文件系统在覆写时会换块位置,前提就不成立;捐赠机制需要一个区间交换接口,XFS 也有。 |
| **技术新颖性那一段**(`design2.tex:51`,当初是回应 REVIEW 4 Detailed Comment 1 写的) | —— | 新颖性段落本属表述层面,规矩上以您为准;列在这里是因为它的第一点同时是「为什么写裸 LBA 不危险」的安全性论证:预分配日志文件的偏移到块映射在文件保持打开期间不变,Éxitos 发 I/O 用的块号就是 ext4 自己算出来的那些。上面几段也被删之后,这一版全文没有任何地方陈述这个前提。 |
| **捐赠池用尽 / 准备赶不上写** 的两句(第四节 C 小节) | 「准备是异步的,写超过准备进度时会怎样?池子用光了应用看到什么?」 | **不要按原样恢复**,我们那两句都不对。代码里:可用捐赠但准备失败时,`src/preload.c:1054-1058` 置 `errno` 返回 −1 给应用,不会偷偷改走原生 `fallocate`;写者从不等待准备,`donor_async_runway()` 是一次原子读,读到 0 就走普通内核路径(`src/donor_async.c:1-14`、`:68-84`)。 |

第六项的**正确写法**(一句,尽量贴合这一版的语气):「If the donor pool cannot satisfy an allocation request, if a move-extent operation fails, or if a write arrives before preparation has caught up with it, Éxitos falls back to Ext4's normal allocation path for that operation.」

另外,被删的那四句里还有一句是「Éxitos obtains a list of log files from a configuration file already used by the database and intercepts only `write` and `fdatasync` requests to the listed log files. All other requests continue through the conventional I/O path.」以及「Before entering the shortcut path, Éxitos verifies that the request falls within a block range registered in the Maco structure; otherwise it falls back to the conventional path.」删掉之后,`grep -in "falls back\|fall back\|fallback\|conventional path"` 在这一版全文只剩一处,而且在第五节的评测问题里。这是实现的整个安全论证:`src/intercept.c:8-27` 列出默认放行规则和九个具名条件,`EXITOS_PASS` 在这个文件里有二十多个返回点。加上拦截被描述成一个不限范围的内核 tracepoint,审稿人的默认读法会变成「机器上每一个 `write` 都进了捷径」。

**建议补一句**:「Éxitos acts only on descriptors it has registered for the log files named in the database's configuration, and only when the request falls entirely within a block range recorded in the Maco structure; every other request, and any raw write that fails, is handed back to the conventional path unchanged.」

---

## 四、其他次级错误

下面这些两版都有,不是这一版引入的,修改责任在我们。列出来是因为第四节被压缩之后它们在剩下的篇幅里占比更大,审稿人翻开开源仓库就会碰到。

1. **「we leverage the io_uring to asynchronously transfer extents」**(第四节 D 小节;第五节 E 小节还有一次「frequently call the move-extent and io uring to extend the binlog file」)。异步来自一个后台线程,不是 io_uring:`src/donor_async.c:174` 的 `pthread_create(&a->th, NULL, preparer, a)`,里面的传输是阻塞的 `ioctl(target_fd, EXT4_IOC_MOVE_EXT, &me)`(`src/donor.c:682`,经 `donor_async.c:110` 的 `donor_extend()`)。io_uring 只出现在设备提交后端 `src/iopath.c`。异步这个说法是对的,给它安的机制不对。改成「we perform the extent transfer on a background preparer thread, off the critical path」。另外,内核 6.6.5 的 io_uring 没有通用 ioctl 操作码,所以这句不只是「没实现」,按字面也做不到 —— 做 io_uring 的审稿人会直接问哪个操作码承载 MOVE_EXT。
2. **「60.2% ... on average」与第三节的「60.2% for the NVMe SSD」自相矛盾**。图 1b 里 HDD 的软件时间约 8%,不带掉电保护的 SATA SSD 约 17%,所以 60.2% 不是四个设备的平均值,是最大值。改引言:把 `on average` 换成 `on a low-latency NVMe SSD`。
3. **io_uring 被引到了 ioctl(2) 手册页**。「we leverage the io uring [16, 18]」里,[16] 是 `ioctl: System calls manual`,[18] 才是 Axboe 的 io_uring 论文。改成 `[18]`。

---

## 五、参考文献的三处变化

- **[22](tracepoint)从内核自己的文档退回到一篇厂商博客**:我们版本引的是 Desnoyers 的 `docs.kernel.org/trace/tracepoints.html`(`ioctl4.bib:198`),这一版引的是 Arges 在 Confluera 的 2020 年博客(`ioctl3.bib:478`)。对全文最容易被质疑的那个机制陈述来说,这是把证据换弱了。
- **[23](bpftime,OSDI'25)和 [26](`ioctl_xfs_exchange_range(2)`)随着被删的段落一起消失**。这两条在 camera-ready 阶段都核过:bpftime 对过仓库的 `CITATION.cff` 和 DBLP 条目 `conf/osdi/ZhengY0HL0Q25`,XFS 那条对过 man7.org 页面。这一版的文献表本身是自洽的(26 条,正文里 [1]–[26] 全部能解析到正确条目),所以不是断引用,是丢了两条。
- **Sysbench(这一版的 [26])作者写成了 P. Zaitsev**,我们版写的是 A. Kopytov,与 URL 指向的仓库 `github.com/akopytov/sysbench` 一致。

另有一条两版都有:[7] 列在文献表里但正文从未引用。

---

## 六、只按语法处理的表述项

表述层面一律以您为准,只有确定的语法错误才列出来,共三处:

1. **「Databases logging.」**(第二节第一个行内小标题)。我们版是「Database logging.」,也是全文其它地方一律使用的术语(摘要「Database logging relies on file write and fsync」、`mot2.tex:29`、`design2.tex:181`)。删掉那个 s。
2. **「The move-extent is a feature that Ext4 file system uses for online defragmentation」**,缺冠词,应为「that **the** Ext4 file system uses」。
3. **「Log files are hardly read except for recovery」**(我们版是 rarely read)。`hardly` 后面要跟 `ever`。这一条介于语法和习惯之间,如果您认为不算硬性语法错误,以您为准。

---

## 七、核查过、确认没有问题的部分

- **第二节和第三节逐词相同**,没有删掉任何测量、前提或交叉引用。我用两种办法验证:对指定区间做词集合差分,除下面报告的几项和排版换行外没有残留;以及把两份 PDF 的第 2、3 页按 150 dpi 渲染出来比对,图 1a、图 1b、图 2、表 I 完全一致。9 页压到 8 页发生在第四节。
- **四个观察 𝕆1–𝕆4 全部保留且与各自的图自洽**:35MB/s→166MB/s、60.2% 与 46.3%、2.6×/3.2×/3.4×、表 I 的二十五个格子逐格未变(包括对 BypassD 的刻画「specialized IOMMU」「more than 4,200 lines of code」「only supports NVMe SSD」)。
- **所有数字未被改动**:两版共有的每一个数字都相同,包括实验配置(16 核、64GB、1GB 文件、4KB+fdatasync、16KB、16 线程、10GB、4K–128K、64KB/256KB/1MB、一千万行表、30 分钟)。每个保留下来的数字都保留了它的条件(块大小、线程数、哪一个 arm、什么负载)。唯一消失的数字是第五节 D 小节里那组关键路径批量 `fallocate` 的对照值 2.6×、3.2×、3.4×,它们随所在的那句话一起被删掉了。
- **九张图全部在,全部被引用过**;图 6 的 (a)–(c) 和图 7 的 (a)–(f) 子图都在;表 I 在且被引用两次。没有「引用了但缺图」或「有图但没引用」的情况。
- **章节编号与交叉引用全部对得上**:I–VI 顺序正确,IV-A…IV-D 与 V-A…V-E 顺序正确,正文里每一处 `Section` 引用都能解析。评测路线图承诺四个问题,V-B…V-E 四个小节逐一回答;V-C 说「we derive five observations」,后面确实是五条。
- **第六节 Conclusion 与我们版逐词相同**。`ACKNOWLEDGMENT` 标题没了,但文字没丢,移到了首页脚注(基金号 2022YFB4401700、上科大启动经费、通讯作者),这是正常的 camera-ready 处理。
- **不打算报告的差异**:新增的第二单位、移到首页的基金脚注、压缩后的文献格式、图 3 / 图 4 浮动体位置的调整,都属编辑层面的决定,不在本文档的范围内。

---

## 八、引用真实性与支撑力核查

这一节分两半:书目本身对不对(文献存不存在、作者年份会场卷期页码对不对),以及引用支撑不支撑得住它所在的那句话。

**先说结论:没有虚构的文献。** [1]–[26] 全部真实存在,[1]–[13] 的作者、年份、会场、卷期、页码逐条对过原始出处,没有一条错;正文里 [1]–[26] 每一条都被引用过,没有超过 26 的引用号。重新编号之后有四处「句子没动、编号变了」,逐一核过都还指向同一件作品。被删的两条([23] bpftime、[26] XFS range-exchange)是随各自的句子和段落一起走的,没有留下悬空引用;不过 [23] 建议按 2.2 加回来。

### 8.1 这一版新引入的引用错误,七条

1. **[26] sysbench 的作者写成了 P. Zaitsev**,应为 A. Kopytov。`src/sysbench.c` 第 2 行是 `Copyright (C) 2004-2018 Alexey Kopytov`,ChangeLog 每一条都由 Kopytov 签名,仓库所有者是 `akopytov`(1197 次提交,第二名 14 次),Peter Zaitsev 在整个仓库的作者记录里没有出现。我们版本的 [28] 是对的。
2. **[17] ext4 move-extent 的地址改成了一个 404。** 这一版写的是 `https://www.kernel.org/doc/html/latest/filesystems/ext4.html`,返回 404;我们版本的 `https://docs.kernel.org/admin-guide/ext4.html` 返回 200,而且里面就有 `EXT4_IOC_MOVE_EXT` 的完整说明。这一版给的标题「Ext4 ioc move ext - online defragmentation for ext4」不是任何内核文档的标题,真实标题是「ext4 General Information」。**这一条影响五个正文句子**,全部是关于 move-extent 的,内容都对,但读者按印出来的地址点不进去。
3. **[21] Intel VTune 的地址被截短成了一个 404。** 这一版是 `https://www.intel.com/content/www/us/en/docs/vtune-profiler`,重定向到 Intel 自己的错误页;我们版本的完整地址 `.../get-started-guide/2026-0/overview.html` 返回 200。
4. **[22] 从内核文档换成了一个域名已消失的厂商博客。** 我们版本引的是 Desnoyers 的 `https://docs.kernel.org/trace/tracepoints.html`(今天仍是 200);这一版引的是 Arges 在 Confluera 的 2020 年博客,而 `confluera.com` 现在根本不解析(权威域名服务器返回 REFUSED),最后一次存档是 2026-02-18。这一换有一个真实的好处:这一版那句话是「静态插桩比动态插桩快」这个比较性主张,内核文档不做这个比较,博客做了(还给了 kprobes+bpf 慢 20% 的数字);但代价是引用点不进去、用厂商营销文章替代了内核官方文档,而且那篇文章把「静态」列为缺点、把速度另算,这一版的写法压缩了原意。
5. **[24] libATA 的作者被换掉了。** 页面上明写 `Author: Jeff Garzik`,我们版本的 [25] 写的就是 J. Garzik,这一版改成了「Linux Kernel Documentation」;年份也从 2026 改成 2025,与同一表里其它网页条目(**[17]** 写 2026、**[23]** 写 Jan 2026)不一致。
6. **[20] fio 加了一个与地址矛盾的版本号和日期。** 这一版写「rev. 3.36 ... August 2024」,但引的是 `https://fio.readthedocs.io/en/latest/fio_doc.html` 这个滚动地址,今天显示的是 rev. 3.42;而且 fio 3.36 的发布日期是 2023-10-20,2024 年 8 月的当期版本是 3.37。我们版本不带版本号,是自洽的。
7. **[25] 「SCAIL Reseach Group」把 Research 拼错了。** 有意思的是这个拼写错误是从源头抄来的 —— GitHub 那个组织自己的简介就写着「The SCAIL (formerly Multifacet) Reseach Group」;但我们版本当初把它改对了。另外仓库路径实际是 `multifacet/Bypassd`(结尾小写 d),不是 `BypassD`,GitHub 会重定向所以链接能用。

### 8.2 排版层面的回退

**[1]、[4]、[7]、[13] 的页码现在印成 `p.` 而不是 `pp.`。** 原因当场复现过:`IEEEtran.bst` 只认 ASCII 连字符,`pages` 字段里换成 Unicode en-dash 之后判定失败,退成单数的 `p.`。这四条恰好全是 ACM 和 IEEE 的条目,像是从数字图书馆重新粘贴过。仓库里的 `ioctl4.bib` 仍然是正确的 `--` 写法,说明这一版的 `.bib` 已经和仓库里的 `ioctl4.bib` 分叉。

有一处我们两次读数不一致,建议直接看 PDF 确认:**[7] 的页码**,一次读到的是 `p. 228–243`(只是 pp. 变 p.),另一次读到的是 `p. 228 243`(连字符本身也丢了)。

其余三处较轻:作者列表新做的「first author et al.」截断漏了 [4](同样六个作者的 [3] 被截断了);[7] 和 [13] 的会议城市被换成了 ACM 的公司地址(纽约);[14] 的会议录标题少了 ", Volume 1"。

**[2] MySQL 的年份从 2026 退回 2025**,而 `mysql.com` 的页脚现在写的是「© 2026 Oracle」。同一条的条目类型也从网页引用改成了出版物式条目,把 Oracle Corporation 同时写成作者和出版者。

### 8.3 引用支撑不住所在句子的地方,八条(两版都有,不是这一版引入的)

按危害排序。

1. **[14] BypassD「introduces more than 4,200 lines of code (LOC) in the Linux kernel」。** ASPLOS'24 那篇论文的表 2 分四项:Kernel 517 行、ext4 1303 行、Device driver 885 行、UserLib 1496 行。UserLib 是用户态库,内核侧合计 2705 行。4201 是项目总行数,不是内核占用。**这句话的用途正是论证 BypassD 侵入内核,方向对我们有利**,审稿人查到会很难看。改法:「introduces more than 4,200 lines of code, of which about 2,700 are in the Linux kernel.」
2. **[14] 又被引来支撑「frequent metadata operations ... mainly due to searching for target block numbers in the per-file mappings」。** 那篇论文说的是相反的话:「If the block mappings are cached in memory (e.g., ext4's extent status tree), obtaining LBAs is inexpensive.」而我们测的正是预分配文件的稳态追加,恰好是那棵树热的情况。它唯一的延迟分解只有一行合并数据「VFS + ext4 2,810 ns 36%」,没有把其中任何一部分归给块映射查找。
3. **[1] OceanBase 被引来支撑两句关于预分配的话** —— 引言的「a few databases preallocate log files using the fallocate syscall [1, 2]」和背景的「like the rising-star OceanBase [1], initialize log files by preallocating tens of gigabytes by default」。那篇 PVLDB 论文全文里 `fallocate` 零次、`preallocat` 零次、`log file` 零次。它确实支撑同一处引用的另一件事(日志保证一致性与持久性),但没有任何预分配内容。「几十 GB 是默认值」这个说法也没找到出处:OceanBase 自己的部署参数 `log_disk_size` 默认是 0(部署时按实际磁盘算),`log_disk_percentage` 也是 0,「20G」只出现在参数说明的格式示例里。
   
4. **[12] NVMeDirect 被引来支撑「demand extensive reprogramming efforts due to poor POSIX API compatibility」。** HotStorage'16 那篇 §4.3 报的是把 Redis 移植过去「added 6 LOC ... and modified 12 LOC」。十八行是「extensive reprogramming」的反面。真正撑得住的论点在同一篇里:它不提供文件系统,论文自己承认「it cannot provide enough protection normally enforced by the file system layer」。建议把这句话改挂在「缺文件系统」上。
5. **[6]–[10] 被归成「optimized fsync at the operating system (OS) level」,其中两篇需要改设备。** [7] OptFS:「requires a slight change in the disk interface to provide ... asynchronous durability notification」;[10] RFLUSH:RFLUSH 本身是一条新的主机-设备命令,§4 写着「We modified both the file system (F2FS) and the storage device (BlueDBM)」,BlueDBM 是一块 FPGA 闪存平台。**这一条格外要紧,因为「纯软件、不改硬件」正是表 I 和引言里 Éxitos 的卖点**,这句话把自己赖以区分的那条线抹掉了。改法:[6]、[8]、[9] 留在「at the OS level」,[7]、[10] 移到后面一个说「也改了设备接口」的从句里。
6. **表 I 里 Moneta-D 的「Compatibility with SATA SSD ✓」撑不住。** 那篇论文的原型「runs at 250 MHz on a BEE3 FPGA prototyping system」,用 DRAM 加改过的内存控制器模拟相变存储,全文没有出现 SATA。一行已经因为需要自己的板子而在「No hardware change」打了 ×,不可能同时兼容商用 SATA SSD。
7. **表 I 里 SPDK 的同一格也是 ✓。** CloudCom'17 那篇通篇讲 NVMe,SPDK 自己的文档说「The bedrock of SPDK is a user space, polled-mode, asynchronous, lockless NVMe driver」,没有 SATA/AHCI 驱动。SPDK 够到 SATA 设备只能走 `aio` bdev 模块,那是绕回内核的路,不是这一列讲的内核旁路。
8. **[25] BypassD 仓库被引来支撑「The latter provides source codes with specialized IOMMU by emulation」。** 那个 README 只有 75 行,`IOMMU`、`emulat`、`qemu`、`simulat` 全部零次,41 个路径里也没有相关文件。**这个事实本身是真的**,出处在 [14] 那篇论文:§6.2「we emulate the overheads of VBA translation by adding a delay while issuing requests from UserLib」,§7「a minimum delay of 550ns」。改引 [14] 即可。


还有一条属于轻微拉伸、可以接受:**[23]** 被引来支撑「This results in the semantic equivalence to original fdatasync for guaranteed persistence」。那个页面支撑「数据完整性操作要靠设备缓存刷盘」这个一般原则,但没有提到 `nvme-flush`,也没有关于 fdatasync 语义的陈述(fdatasync 还带一份元数据义务,页面不讨论)。地址和署名都对。

### 8.4 取不到的东西,分清是哪一种

- **IEEE Xplore** 对自动抓取返回 HTTP 418 且只走 JavaScript。[4] 的书目信息因此走 Crossref、OpenAlex、Semantic Scholar(三者都带 IEEE 自己提交的记录),全文用的是 KAIST OSLab 作者主页上挂的 PDF(文件名就是 IEEE 文章号 07172998,可据此确认同一篇)。要读 Xplore 页面本身,需要人在浏览器里取。
- **ACM 数字图书馆**返回 403。[14] 因此走 Crossref 加合作者主页的 PDF(`cgi.di.uoa.gr/~vkarakos/papers/asplos24_bypassd.pdf`),两者对得上(页码 35–51,正好 17 页,与 ACM Reference Format 里的「17 pages」一致)。
- **usenix.org** 对不带浏览器 User-Agent 的自动抓取返回 403,带上之后正常,所有 USENIX 条目都是从真实的会议录页面读的。
- **intel.com** 带浏览器 UA 时对新旧两个地址都返回 403(机器人过滤),所以判 [21] 是 404 用的是不带 UA 那一次,那一次返回的是 Intel 自己的错误页(`<title>Error: Page Not Found</title>`)。
- **两条结论用的是存档而不是实时页面,日期就写在结论旁边**:[22] 的内容取自 **2020-09-24** 的 Wayback 快照(`confluera.com` 今天,2026-09-02,不解析);[18] 的文档身份取自 **2026-06-24** 的快照(`kernel.dk/io_uring.pdf` 今天返回 404,最早一条 404 记录是 2026-07-22,也就是说这个地址是大约两个月前坏的;文档本身是真的,8 页,版本行写着 `Version: 0.4, 2019-10-15`)。
- **我们最初抽出来的参考文献文本是截断的**,只到 [23] 而且在 URL 中间断掉,[24]–[26] 是回到全文重新取的。这不影响上面的结论,记在这里是为了说明核查过程。
