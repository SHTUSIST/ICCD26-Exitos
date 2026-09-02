> 分类：给用户参考，不需要回应。

# camera-ready 改动清单

本轮一共改了 11 处。其中 9 处直接对应审稿意见，另外 2 处不是审稿意见：第 3 条点名 bpftime 是用户指定要补的，第 11 条对新增文字做可读性与术语统一是用户的第三条要求。页数从 8 页变成 10 页。

## 改了哪些地方

**1. Figure 1 两个子图改成等高。** 改的是 `mot2.tex` 里 Figure 1 的 `figure` 环境：给第二个子图的 `\includegraphics` 指定 `width=0.8801\columnwidth`，两个子图各加一行 `\centering`，两张图本身一个像素都没动。改完之后两个子图的图例、坐标框上沿、子图题处在同一水平线上。对应 REVIEW 3 的 Detailed Comment 4，审稿人原话是 `The two subfigures in Figure 1 are not well aligned, which slightly affects readability.`

**2. 说明技术新颖性在哪里。** 在 `design2.tex` 的 §IV-A `An Overview of \odes` 小节末尾新增一段，第一句直接写出新颖点：`The primary technical novelty of \odes lies in two points.` 随后写出两点——预分配日志文件的偏移到块号映射在文件打开期间保持不变，这个不变量正是纯软件捷径成立的前提；把 move-extent 从碎片整理功能改用成在线供给空间的手段，让同一个不变量对增长中的日志文件也成立。对应 REVIEW 4 的 Detailed Comment 1。

**3. 点名用了 bpftime 并引它的 OSDI 论文。** 在 `design2.tex` 的 §IV-B `Runtime interception` 段落末尾新增一句 `\odes loads and runs these interception programs with bpftime~\cite{bpftime}, an eBPF runtime that is compatible with the standard eBPF toolchain.`，只说明 bpftime 兼容标准 eBPF 工具链这一个特性。不需要给内核打补丁这一点前面已经讲过三处，这里不重复：`mot2.tex` 的 Table I 有一列 `Non-intrusive kernel`，`mot2.tex` 正文写 BypassD 这类方案 `require kernel changes` 并且改了 `4,200 lines of code (LOC) in the Linux kernel`，`hotstorage/intro.tex` 写这些方案有 `intrusive kernel modifications`。`ioctl3.bib` 新增一条 BibTeX，引用键 `bpftime`。这一条不是审稿意见，是用户指定要补的，用户原话是「这里有一点论文没写好, 你要提我们是用了ebpf library bpftime, 你就在劫持那里说明, 简要一句话说明, 然后引一下bpftime对应的osdi论文」。引用信息核实过两处且互相一致（bpftime 仓库的 `README.md` 与 `CITATION.cff`，以及 DBLP 记录 `conf/osdi/ZhengY0HL0Q25`）：Extending Applications Safely and Efficiently，作者 Yusheng Zheng、Tong Yu、Yiwei Yang、Yanpeng Hu、Xiaozheng Lai、Dan Williams、Andi Quinn，OSDI '25，页码 557--574。本文第一作者 Yanpeng Hu 也是这篇 OSDI 论文的作者之一，属于自引；ICCD camera-ready 不是双盲，没有问题。

**4. 补上哪些文件走捷径路径、ioctl 发出前校验什么。** 在 `design2.tex` 的 §IV-B `Runtime interception` 段落末尾新增三句：Éxitos 从数据库本来就会读的配置文件里拿到日志文件清单，只拦截写往清单上那些文件的 write 与 fdatasync，别的文件照走原来的路径，数据库源码不用改；发 ioctl 之前先确认这次请求的文件内偏移与长度落在 Maco 为该 file descriptor 登记过的某个 block range 之内，落在外面的请求交回传统路径。对应 REVIEW 1 的 Weakness 4。REVIEW 1 的 Weakness 4 点名四项：日志文件识别、Maco 初始化与更新、ioctl 校验、防止过期映射；本条答的是第一项和第三项，第二项和第四项由下面第 7 条答。

**5. 补上 donor 池的失败路径。** 在 `design2.tex` 的 §IV-C `Donor-Based Online Space Allocation` 小节末尾新增一段：一次搬移多少空间由监控模块在线测出的日志 I/O 平均大小决定；donor 池空间降到阈值以下时，在关键路径之外预分配新的 donor 文件补上；日志文件关闭时把没用掉的块还给 donor 文件；donor 池满足不了请求或者 move-extent 失败时，退回 Ext4 原本的分配路径，日志文件仍然正确，丢掉的只是这一次的加速；异步搬移还没完成时到达的写请求会等搬移完成，不会看到改了一半的映射。其中「donor 池空间降到阈值以下时在关键路径之外预分配新的 donor 文件」这一句的写法已经过第一作者确认。对应 REVIEW 1 的 Weakness 3 与 Detailed Comment 4。

**6. 新增 `Crash consistency` 一条。** 位置在 `design2.tex` 的 §IV-E `Optimizations and Discussions` 之内、`Permission checks.` 之前，是本轮最长的一处新增。内容依次是：Éxitos 改的是数据往哪里送，不改写什么内容、也不改什么时候算持久；写入顺序靠同步等待 write 与 fdatasync 的返回信号来保持；fdatasync 的等价性靠 PLP 或显式 flush 命令；部分写返回与传统路径相同的错误，数据库自己的日志校验在恢复时丢弃末尾不完整的记录；崩溃之后 Maco 里没有 Ext4 推不出来的状态，Ext4 的盘上元数据是权威的，Maco 在文件下次打开时重建；Éxitos 只在 move-extent 完成之后才往搬过来的空间里写数据，move-extent 失败或 donor 池耗尽时退回 Ext4 的分配路径。这是全部意见里最重要的一条，两位审稿人独立提出——REVIEW 1 的 Weakness 1 与 Detailed Comment 1，REVIEW 3 的 Weakness 1 与 Detailed Comment 1。REVIEW 1 在 Paper Summary 末尾把这一点写成自己的主要顾虑，原话是 `My main concern is that the correctness and failure-mode analysis is not yet strong enough for a database logging path.`

**7. 新增 `Stale mappings` 一条。** 位置在 `design2.tex` 的 §IV-E，紧接在 `Crash consistency` 之后：预分配日志文件在打开期间映射不会变；增长中的日志文件只有 Éxitos 自己发的 move-extent 会改映射，而 Éxitos 在同一步更新 Maco；Maco 的生存期就是文件打开的这段时间，文件没打开时发生的操作（例如 Ext4 在线碎片整理）留不下过期表项；strict mode 每次操作都校验 inode 属性，也能看到元数据变化。对应 REVIEW 1 的 Detailed Comment 2，同时答掉 REVIEW 1 的 Weakness 4 的第四项。审稿人写明了后果，原话是 `an incorrect mapping could lead to writes being sent to the wrong blocks`。

**8. 新增 `Error reporting` 一条。** 位置在 `design2.tex` 的 §IV-E，紧接在 `Stale mappings` 之后：ioctl 把设备的完成状态返回给 Éxitos，Éxitos 映射成传统路径在 write 或 fdatasync 上会返回的同一个 errno，数据库看到的错误一样，数据库原有的错误处理逻辑不用改。这一条的写法已经过第一作者确认。对应 REVIEW 1 的 Detailed Comment 5。

**9. 新增 `Portability` 一条。** 位置在 `design2.tex` 的 §IV-E 最后一条、`Compatibility.` 之后，`ioctl3.bib` 同时新增一条 BibTeX。这一条把设计拆成四层说清楚：缓存映射并在运行时直接查映射，只要求文件的映射在打开期间稳定，任何就地分配块的文件系统都满足，Ext4 与 XFS 都是；donor 机制额外要求文件系统提供一个不复制数据就交换两个文件之间一段空间的接口，Ext4 提供 move-extent，XFS 提供作用相当的 range-exchange ioctl；Btrfs 与 F2FS 这类把数据写到别处的文件系统每次覆盖写都会改变映射，前提不成立，Éxitos 需要换一种办法拿块号；用 eBPF 拦截、用 ioctl 投递是 Linux 特有的，flush 命令是设备相关的，而 Éxitos 已经同时发 NVMe 与 SCSI 两种命令。对应 REVIEW 3 的 Weakness 2 与 Detailed Comment 2、REVIEW 4 的 Weakness 1 与 Detailed Comment 3，两位审稿人独立提出。XFS 那个 ioctl 的引用核实过：`ioctl_xfs_exchange_range(2)`，man7.org 手册页 NAME 行原文 `ioctl_xfs_exchange_range - exchange the contents of parts of two files`，CONFORMING TO 一节写明 `This API is XFS-specific.`，引用键 `xfs-exchange-range`。

**10. 把 Section III 那组 fallocate 测试点名为 batched preallocation 基线。** 改的是 `eval2.tex` 的 §V-D `The Effect of Online Preallocation`，把原来一句话改写成四句，被删掉的原句是 `They are all higher than corresponding improvements obtained by preallocation on the critical path.` 改写后先说清 Section III 测过另一种预留空间的办法（在关键路径上调 fallocate，在每次追加写之前成批预留块），点明这就是一条 batched preallocation 基线；再并排给出两组数字——4KB 写入下，这条基线在 64KB、256KB、1MB 三种预留尺寸下相对不预留分别提升 2.6×、3.2×、3.4×，而 Éxitos 在同样三种尺寸下是 3.3×、3.6×、3.9×；最后说明两组测试预留的空间尺寸相同、写请求尺寸也相同，所以差出来的部分不是「少做几次分配」带来的，而是把分配挪出关键路径再加上捷径 I/O 路径带来的。对应 REVIEW 1 的 Weakness 2 与 Detailed Comment 3。审稿人要的这条基线其实一直在论文里，只是没有被叫作基线，所以审稿人没认出来；这一处不需要新实验，六个倍数、三个尺寸一字未改。

**11. 新增文字的可读性与术语统一。** 改的是 `design2.tex` 与 `eval2.tex`，对上面第 2 到第 10 条新写的句子做了 13 处改写：把要读两遍才明白的长句断开、把压成名词短语的地方还原成从句、把与原有正文对不上的词统一回去（`all-software` 改回论文一直用的 `all-in-software`，`the conventional file system path` 与 `the original path of the file system` 统一成 `the conventional path`，`reservation sizes` 统一成 `preallocation sizes`）。13 处改写全部落在第 2 到第 10 条新增的段落里，录用版原有的文字一处都没有碰，所以这一条保留。这一条没有改动任何数字、任何引用、任何主张。对应用户的第三条要求，用户原话里的判断是「这次压缩后文字更短，但略微更难懂，生硬表达反而更多」「不建议继续追求每句最短」「这些修改会增加少量词数，但能显著减少"AI 为了短而造出来的词组"」。

## 没有处理的三条

这三条都需要补新的实测数据，camera-ready 阶段做不了。

**没处理 1：拆解各个部件各自贡献多少的消融实验。** 对应 REVIEW 3 的 Detailed Comment 3，审稿人点名要分开的五项是 Maco 免去查找、eBPF 拦截、ioctl 投递、异步 move-extent、donor 文件管理。做不了的原因是这五项要各跑一组新实验。论文里已有的部分分解有三处：Éxitos 与 Éxitos-S 的差值分离出权限检查的代价；Éxitos-B 与 Éxitos-R 分离出 MySQL 两种日志各自的影响；§V-D 那张图分离出 donor 加 move-extent 策略的效果。

**没处理 2：CPU 开销、更高并发下的扩展性、换文件系统或换设备的敏感性。** 对应 REVIEW 4 的 Detailed Comment 2，审稿人用的措辞是 `If possible`，语气是加分项而不是必须。做不了的原因是每一项都要重新搭测试环境跑数据。论文里已有的部分覆盖：Figure 1(b) 覆盖四种设备（机械硬盘、无 PLP 的 SATA SSD、有 PLP 的 SATA SSD、有 PLP 的 NVMe SSD）；Fio 测试有 16 线程一组；OceanBase 的 SysBench 测试从 1 个客户端线程扫到 32 个。

**没处理 3：实测对比的系统太少。** 对应 REVIEW 4 的 Weakness 2，论文实测对比的只有 Vanilla 与 BypassD 两条基线。做不了的原因写在论文 §V-A 的 Baselines 段：把一个复杂数据库改写到 SPDK 与 NVMeDirect 这两套接口上工作量过大。论文里已有的说明是 Table I 对 SPDK 与 NVMeDirect 做了五个维度的定性比较。

## 页数

改之前是 8 页，而且第 8 页两栏都排满、一行空隙都没有；改之后是 10 页，第 10 页只排到 39%，前 9 页每一页两栏都是排满的。录用邮件的规定原文是 `A regular paper is 8 pages in the IEEE conference format, with the option to purchase up to 2 additional pages.`，也就是 8 页免费、最多可以再买 2 页，10 页正好是上限，需要购买 2 页。

风险在于 10 页已经顶到上限：之后任何新增（例如 IEEE 要求在第 1 页加版权声明块）都会溢出到第 11 页，第 11 页是不允许的。想留出余量就得把本轮新增的文字压掉大约三分之一，回到 9 页。

## 验证结果

下面是从零重新编译（`rm -rf build && latexmk -pdf -interaction=nonstopmode -outdir=build ICCD26-main.tex`）之后的实测结果，编译退出码 0。

- 页数：10。
- Overfull box 7 个、Underfull box 4 个。录用版是 7 个 Overfull、5 个 Underfull，两者同源，本轮没有新增。
- 未定义的引用 0 个，未定义的交叉引用 0 个。参考文献从 26 条增加到 28 条，两条新加的 BibTeX 条目都进了参考文献表。
- 逐条核对：上面第 2 到第 10 条的新增文字全部在生成的 PDF 里找得到。
- 数字核对：论文里 28 个关键数值（2.6×、3.2×、3.4×、3.3×、3.6×、3.9×、2.1×、1.6×、2.4×、109.0%、60.2%、46.3%、63.1%、31.1%、42.3%、47.4%、21.4%、62.3%、53.3%、41.5%、5.5%、18.5%、25.8%、7.7%、34.1%、60.8%、12.8%、4,200）在改后的 PDF 里一个不少、一个没变。
- Figure 1 的对齐：把生成的 PDF 第 2 页渲染成图片看过，两个子图的图例、坐标框上沿、子图题在同一水平线上。

## 怎么自己生成对照 PDF

仓库根目录有一个脚本 `make-diff-pdf.sh`。在仓库根目录运行 `./make-diff-pdf.sh`，就会生成 `ICCD26-Exitos-diff.pdf`，把录用版与最终版排在同一份排版结果上：删掉的文字是红色、带一道删除线；新增的文字是蓝色、带波浪下划线。这两种颜色是 latexdiff 默认的 UNDERLINE 标记样式，合作者与审稿人已经读惯了，所以不再自造配色。

三种用法：

- `./make-diff-pdf.sh`：基线按 `accepted`、`submitted`、`camera-ready`、`v1` 的顺序取第一个存在的标签，都没有就取仓库的第一个提交；新的一侧取当前工作区，未提交的改动也算在内。
- `./make-diff-pdf.sh accepted`：指定基线，新的一侧仍取当前工作区。
- `./make-diff-pdf.sh accepted 0f2cf3a`：指定两个 git 版本互相对比，标签、提交号、分支名都可以写；`worktree` 这个词专指当前工作区。

参考文献表现在也带标记：脚本先在两个版本上各跑一遍 bibtex，再让 latexdiff 把参考文献表并进正文一起比，所以本轮新加的两条引用（bpftime 与 XFS 的 range-exchange ioctl）在对照 PDF 的参考文献表里也是蓝色带波浪下划线。生成之后脚本把每一页都画一遍，任何一页画不出来就报错退出、不产出文件；还会用 ghostscript 重写一遍 PDF 把字体全部嵌进去，因为图里有字体没嵌进去时，浏览器里的 PDF 阅读器会画错或者干脆拒绝显示。

可选项写在命令前面，例如 `NOBIB=1 ./make-diff-pdf.sh`：`TYPE=CFONT` 换一种标记样式，latexdiff 支持的样式名可以用 `latexdiff --help` 查；`NOBIB=1` 跳过参考文献表的标记；`NOPOST=1` 跳过逐页渲染检查与字体嵌入；`MAIN=paper.tex` 指定主文件，不写就自动找带 `\documentclass` 和 `\begin{document}` 的那个 `.tex`；`OUT=changes.pdf` 指定输出文件名；`KEEP=1` 保留中间目录并打印路径，便于排查。

脚本需要 git、latexdiff、latexmk、pdflatex、python3，ghostscript 与 poppler-utils 可选但建议装；在 Debian 或 Ubuntu 上装这些依赖的命令是 `apt-get install -y latexdiff latexmk texlive-latex-extra ghostscript poppler-utils`。

