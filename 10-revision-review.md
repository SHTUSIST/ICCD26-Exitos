# 修改版与我们提交版的逐节对比核查

> 这份文档是把您返回的修改版和我们提交前那一版逐节对了一遍的结果。按您邮件的要求,**第一节就是您点名要的三类具体位置**——一处图标题、七处引用、三处语法——每一条都写成同一个形状:论文的第几节第几小节、行内小标题、我们源文件的行号、原句、以及改成什么。
>
> **坐标怎么读。** 章节号是按 `ICCD26-main.tex` 里 `\else` 分支的 input 顺序数 `\section` 与 `\subsection` 推出来的,并用 `build/ICCD26-main.aux` 里的 `\newlabel` 对过,不是估的。行内小标题指 `{\bf ...}` 或 `\textbf{...}` 那种段首粗体。行号是**我们这一版**源文件的行号;您那一版行号不同,但章节号、小标题和原句三者合起来足以定位。凡是说「代码里是怎样」的地方,给的是实现仓库里的文件名加行号,每一个都开文件核过。
>
> 第二节是机制描述与实现对不上的地方——**这些不是新增内容,是把已经写错的句子改对**,多数在您那一版和我们那一版里一字不差。第三节是被删掉的段落,按您邮件不进 camera-ready,列在这里只为 arXiv 版备查。第四节是「文献真实存在、但支撑不住它所在那句话」的地方,两版都有。

---

## 一、您点名的三类,共 11 处

### 1.1 图标题:1 处

**位置** Section V-D 的正文 + 图 8 的标题(图 8 与图 9 是同一个 `figure` 环境里的两个 `minipage`,源文件 `eval2.tex:372-391`,浮动到跨 V-D 与 V-E 的那一页顶部)。

| | 内容 |
|---|---|
| 您那一版 | `Fig. 8: Impact of bypassing different log types.` |
| 我们这一版 | `eval2.tex:379` — `\caption{Impact of preallocation size.}`,标签 `\label{fig:eval:preallocation}` 在 380 行 |
| 为什么是错的 | 图 8 的横轴是 `Append I/O Size`(4K–128K),四条曲线是 `No Prealloc / 64K-async / 256K-async / 1M-async`,画的是预分配尺寸。而 Section V-D 的正文 `eval2.tex:334` 写着 `\autoref{fig:eval:preallocation} shows the curves for three preallocation sizes`,渲染出来就是「Figure 8 shows the curves for three preallocation sizes」——标题和它下面三行的正文互相矛盾。「bypassing different log types」是图 9(`eval2.tex:387`,`\label{fig:mysql_sysbench}`)的内容。 |
| **改成** | `Impact of preallocation size.` |

补一句来历:论文仓库的提交 `d4c067a`(提交说明「sub title of figure 8」)已经把 `eval2.tex:379` 从 `Impact of bypassing different log types.` 改成了 `Impact of preallocation size.`,您那一版用的是改之前的标题。

### 1.2 引用:7 处

前四处建议必须改——两处地址点开是 404,两处作者写错了。每一处都只动文献表一行,不占正文版面。**我们 `ioctl4.bib` 里的对应条目行号一并给出,正确数据都在那里。**

| # | 您那一版的编号 | 我们的编号 / bib 行 | 问题 | 改成 |
|---|---|---|---|---|
| 1 | **[26]** sysbench | 我们 [28],`ioctl4.bib:244` | 作者写成 `P. Zaitsev`。仓库 `src/sysbench.c` 第 2 行是 `Copyright (C) 2004-2018 Alexey Kopytov`,ChangeLog 每条都由 Kopytov 签名,仓库所有者 `akopytov` 提交 1197 次、第二名 14 次;Peter Zaitsev 在整个仓库的作者记录里没有出现。 | `A. Kopytov` |
| 2 | **[17]** ext4 move-extent | 我们 [17],`ioctl4.bib:159`,URL 在 `162` 行 | 地址改成了 `https://www.kernel.org/doc/html/latest/filesystems/ext4.html`,返回 **404**;标题「Ext4 ioc move ext - online defragmentation for ext4」不是任何内核文档的标题(真实标题是 `ext4 General Information`)。我们的 `https://docs.kernel.org/admin-guide/ext4.html` 返回 200 且含 `EXT4_IOC_MOVE_EXT` 的完整说明。 | 恢复我们 bib 里那个地址与标题 |
| 3 | **[21]** Intel VTune | 我们 [21],`ioctl4.bib:191` | 地址被截短成 `https://www.intel.com/content/www/us/en/docs/vtune-profiler`,重定向到 Intel 自己的错误页(`<title>Error: Page Not Found</title>`);我们的完整地址 `.../get-started-guide/2026-0/overview.html` 返回 200。 | 恢复完整地址 |
| 4 | **[24]** libATA | 我们 [25],`ioctl4.bib:223` | 作者被换成「Linux Kernel Documentation」,而页面上明写 `Author: Jeff Garzik`;年份也从 2026 改成 2025,与同表其它网页条目([17] 写 2026、[23] 写 Jan 2026)不一致。 | `J. Garzik`,年份与同表其它条目一致 |
| 5 | **[22]** tracepoint | 我们 [22],`ioctl4.bib:198` | 从 Desnoyers 的内核官方文档 `https://docs.kernel.org/trace/tracepoints.html`(今天仍返回 200)换成了 Arges 在 Confluera 的 2020 年博客,而 `confluera.com` 现在**根本不解析**(权威域名服务器返回 REFUSED),最后一次存档是 2026-02-18。换过去有一个真实的好处:那句话是「静态插桩比动态插桩快」这个比较性主张,内核文档不做这个比较,博客做了(给了 kprobes+bpf 慢 20% 的数字);代价是引用点不进去,而且那篇文章把「静态」列为缺点、把速度另算。 | 恢复内核文档;比较性主张若要保留,建议引那篇文章自己引的 LWN 补丁集 |
| 6 | **[20]** fio | 我们 [20],`ioctl4.bib:184` | 加了「rev. 3.36 ... August 2024」,但引的是 `https://fio.readthedocs.io/en/latest/fio_doc.html` 这个滚动地址,今天显示的是 rev. 3.42;而且 fio 3.36 发布于 2023-10-20,2024 年 8 月的当期版本是 3.37。我们那一版不带版本号,是自洽的。 | 去掉版本号,或换成版本固定的地址 |
| 7 | **[25]** BypassD 仓库 | 我们 [27],`ioctl4.bib:237` | `SCAIL Reseach Group` 把 Research 拼错了。这个错拼是从源头抄来的——GitHub 那个组织自己的简介就写着「The SCAIL (formerly Multifacet) Reseach Group」——但我们那一版当初改对了。另外仓库路径实际是 `multifacet/Bypassd`(结尾小写 d),GitHub 会重定向,链接能用。 | `Research` |

**另有一处排版回退,不是内容错。** [1]、[4]、[7]、[13] 的页码印成了 `p.` 而不是 `pp.`。原因当场复现过:`IEEEtran.bst` 只认 ASCII 连字符,`pages` 字段换成 Unicode en-dash 之后判定失败,退成单数的 `p.`。这四条恰好全是 ACM 和 IEEE 的条目,像是从数字图书馆重新粘贴过。我们 `ioctl4.bib` 里仍然是正确的 `--` 写法(例如 [4] 在 `ioctl4.bib:37` 是 `pages   = {1720--1733},`)。有一处我们两次读数不一致,建议直接看 PDF 确认:**[7] 的页码**,一次读到 `p. 228–243`,另一次读到 `p. 228 243`(连字符也丢了)。

其余三处更轻:作者列表新做的「first author et al.」截断漏了 [4](同样六个作者的 [3] 被截断了);[7] 和 [13] 的会议城市被换成了 ACM 的公司地址(纽约);[14] 的会议录标题少了 `, Volume 1`(我们 `ioctl4.bib:137` 有)。[2] MySQL 的年份从 2026 退回 2025,而 `mysql.com` 页脚现在写的是「© 2026 Oracle」;我们 `ioctl4.bib:13` 那条仍是 `@misc` 加 year 2026。

### 1.3 语法:3 处

| # | 论文位置 | 我们源文件 | 原句 | 改成 |
|---|---|---|---|---|
| 1 | **Section II**,行内小标题 `Database logging.` | `hotstorage/bg.tex:6` | 您那一版写的是 `Databases logging.` | `Database logging.` —— 删掉那个 s。这也是全文其它地方一律使用的术语(摘要「Database logging relies on file write and fsync」、`mot2.tex:29`、`design2.tex:181`) |
| 2 | **Section IV-C**,行内小标题 `Moving extents.` | `design2.tex:280` | `The move-extent is a feature that Ext4 file system uses for online defragmentation` —— 缺冠词 | `that **the** Ext4 file system uses` |
| 3 | **Section II**,行内小标题 `Database logging.` | `hotstorage/bg.tex:10` | 您那一版把 `rarely read` 改成了 `hardly read`。`hardly` 后面要跟 `ever` | `rarely read`,或 `hardly ever read`。这一条介于语法和习惯之间,若您认为不算硬性错误,以您为准 |

---

## 二、机制描述与实现对不上的地方

**这一节里的句子多数在两版里一字不差,不是您改出来的,也不是新增内容——是把已经写错的地方改对。** 每一条都给了实现仓库里的行号。

### 2.1 拦截机制:tracepoint 这个提法不成立,bpftime 是**第三方的用户态 eBPF 运行时**

**这是整份文档里风险最高的一条,请特别看一下。**

**论文位置** Section IV-B「The Shortcut of Éxitos」,行内小标题 `Runtime interception.`,我们源文件 `design2.tex:167-178`。

我们这一版该段的前两句是:

> \textbf{Runtime interception.} \odes intercepts file operations using syscall tracepoints, which provide low-overhead static kernel instrumentation suitable for latency-sensitive paths~\cite{TracingL22:online}. **The interception programs are executed by bpftime~\cite{bpftime}.**

您那一版把加粗那一句连同参考文献 `[23]`(bpftime)一起删了,后面四句(取哪些日志文件、只拦 `write` 与 `fdatasync`、其余走原路、进捷径前先验 Maco 区间否则退回)也一并删了。

**为什么 tracepoint 这个提法不成立,两层原因。**

第一层,机制上做不到:tracepoint 只能观察,不能改道,而这里要做的是把一次 `write` 从内核原路上拿走送到别处。实现仓库 `src/intercept.c:3-5` 的注释就是为这件事写的——

> The paper describes redirecting a write from inside the kernel, at a syscall tracepoint. A tracepoint cannot do that: it observes, it does not divert. So the decision is made here, in user space.

第二层,实现上根本没装:两个前端都在用户态。一个是 `src/preload.c`,用 `LD_PRELOAD` 做符号插入(`src/preload.c:279` 的 `dlsym(RTLD_NEXT, ...)`,`:688` 起是替换掉的 `write`);另一个是 `src/bpftime_hook.c`,把进程自己代码段里的 `syscall` 指令改写掉转到用户态钩子(`src/bpftime_hook.c:360-373` 是被钩住的调用清单,`:392` 的注释写着 bpftime 把每条 `syscall` 指令改写成 `call`)。全树没有任何一处安装内核 tracepoint——`grep -rn` 找 `tracepoint`、`bpf_program__attach`、`TRACEPOINT`、`perf_event_open` 全部无命中。

**真实情况:拦截程序是 eBPF 程序,但交给第三方的用户态 eBPF 运行时 bpftime 执行,不是内核里的 eBPF 子系统。** bpftime 是把 eBPF 程序放到用户态跑的运行时(Zheng 等,OSDI'25;我们 `ioctl4.bib:205`),**我们是把它当作一个外部依赖引进来用的**。

**为什么删掉那句话之后更危险。** 全文再没有出现 bpftime;剩下的是「syscall tracepoints ... static kernel instrumentation points」这一句,加上引言贡献列表里的「leveraging eBPF」和结论里的「utilizes eBPF」。审稿人把这三处连起来读,得到的是「内核态 eBPF 挂在 syscall tracepoint 上」——**这恰好是系统唯一没有做的事**。而开源脚注还在,审稿人打开仓库看到的是一个 `LD_PRELOAD` 的 `.so` 和一个改写 `syscall` 指令的 `.so`,没有 tracepoint,也没有内核模块。这会被读成对实现的虚假陈述。

**要补充一点:即使按我们那一版恢复,也还不够。** 我们那一版只写了「executed by bpftime」,没有说 bpftime 是用户态运行时,读者仍然会默认成内核 eBPF。建议的英文写法:

> \textbf{Runtime interception.} \odes intercepts file operations entirely in user space. The interception programs are eBPF programs executed by bpftime~\cite{bpftime}, **a third-party user-space eBPF runtime**; \odes requires no kernel module and no patched kernel. \odes obtains a list of log files from a configuration file already used by the database and intercepts only \texttt{write} and \texttt{fdatasync} requests to the listed log files. All other requests continue through the conventional I/O path. This design requires no changes to the database source code. Before entering the shortcut path, \odes verifies that the request falls within a block range registered in the Maco structure; otherwise, it falls back to the conventional path.

配套两件小事:**把参考文献 [23] bpftime 加回文献表**(我们 `ioctl4.bib:205`,camera-ready 阶段核过该项目仓库的 `CITATION.cff` 与 DBLP 条目 `conf/osdi/ZhengY0HL0Q25`);**凡是正文写「eBPF」的地方都补一个限定词**,写成「eBPF programs run in user space by bpftime~\cite{bpftime}」这样,否则读者仍会默认成内核 eBPF。要改的三处是:Section I 贡献列表 `hotstorage/intro.tex:106`(`establishes a new shortcut I/O path by leveraging eBPF~\cite{ebpf}`)与 `:108`(`\odes uses eBPF to intercept and redirect`)、Section I 末尾的路线图 `hotstorage/intro.tex:184`(`\odes enables a direct I/O path by using eBPF \cite{ebpf} and ioctl`)、以及 Section VI 结论 `hotstorage/conclusion.tex:14`(`utilizes eBPF to transparently redirect and transform file operations`)。

顺带说明退回原路那几句为什么值得留:`grep -in "falls back\|fallback\|conventional path"` 在您那一版全文只剩一处,而且在第五节的评测问题里。这是实现的整个安全论证——`src/intercept.c:8-27` 列出默认放行规则和九个具名条件,`EXITOS_PASS` 在这个文件里有二十多个返回点。加上拦截被描述成一个不限范围的内核 tracepoint,审稿人的默认读法会变成「机器上每一个 `write` 都进了捷径」。

### 2.2 严格模式被描述成设计文档明确否决过的那个机制

**论文位置** Section IV-D,行内小标题 `Permission checks.`,我们源文件 `design2.tex:613`。

您那一版:「The strict mode follows MAC compliance for enhanced protection **through inode attribute verification via ioctl interfaces at every operation**.」

**代码做的是另一件事**:每次接管之前,在被拦截的文件描述符上发一个长度为 0 的 `pwrite`,让内核自己的逐写权限门做判断。`src/intercept.c:1880-1889`:

```c
/* Strict mode: before bypassing the kernel, ask the kernel whether this
 * write would still be allowed.  The zero-length probe runs the per-write
 * permission gate (FMODE_WRITE plus the LSM file_permission hook) and
 * nothing else; on refusal the write goes the ordinary way and the kernel
 * reports the error itself. */
if (tx->ctx->strict &&
    exitos_internal_pwrite_call(tx->fd, buf, 0, off) != 0) {
    r->passed++; r->kernel_holds = 1; exitos_stat_inc(...);
    return EXITOS_PASS;
}
```

用 ioctl 查 inode 属性不只是「另一种实现」,它正是 `docs/exitos-s-design.md:54-66`(「Why not a user-space attribute check」)评估之后**否决掉**的那个方案,理由是 inode 属性属于自主访问控制那一侧,查不出 SELinux 策略被收紧。所以那句话是在用一个按构造就做不到的机制去声称 MAC 合规。同一句里还有两处细节:这个探测**只在写上做**,`fdatasync` 是故意不探测的(`src/intercept.c:188-189`:普通 `fdatasync` 路径上没有对应的安全模块文件钩子,探测它会比基线更严),所以不是「at every operation」;严格模式还会连带打开逐写的 `fstat` 描述符身份检查(`src/intercept.c:2234-2240`,`src/frontend_config.c:185`)。

**我们那一版那句也不够准**(`design2.tex:613` 现在写的是「a lightweight kernel-space permission check at the LSM hook of every operation」——Éxitos 并没有安装任何 LSM 钩子,它是触发内核已有的那个),所以不要直接改回我们的写法。建议:

> The {\em strict mode} follows MAC compliance for enhanced protection: before every taken-over write it issues a zero-length write on the same descriptor, which runs the kernel's own per-write permission gate --- the security module's \texttt{file\_permission} hook included --- and the write falls back to the conventional path unless that gate allows it.

### 2.3 io_uring 被说成是搬 extent 的机制,实际是后台异步线程加 ioctl

**论文位置** 一共六处,两版都有:

| 论文位置 | 我们源文件 | 原句片段 |
|---|---|---|
| Section IV-D,`Optimizations.` | `design2.tex:507` | `we leverage the io\_uring~\cite{iouring,ioctlWik84:online} to asynchronously transfer extents for non-preallocated log files` |
| Section IV-D,`Optimizations.` | `design2.tex:518` | 同一段的后续句 |
| Section IV-C,`Moving extents.` | `design2.tex:316` | 异步准备那一句 |
| Section V-D | `eval2.tex:325` | 讲 move-extent 与 io_uring 的那句 |
| Section V-E | `eval2.tex:421` | `frequently call the move-extent and io uring to extend the binlog file` |
| Section I(引言贡献列表) | `hotstorage/intro.tex:141` | `Second, we leverage io\_uring~\cite{iouring} to do asynchronous transfers outside of the critical path` |

**代码里异步来自一个后台线程,不是 io_uring**:`src/donor_async.c:174` 是 `pthread_create(&a->th, NULL, preparer, a)`,那个线程里调 `src/donor_async.c:110` 的 `donor_extend()`,里面是阻塞的 `ioctl(target_fd, EXT4_IOC_MOVE_EXT, &me)`(`src/donor.c:682`)。io_uring 只出现在设备提交后端 `src/iopath.c`(`:553` 的 `io_uring_setup_raw`)。头文件 `include/exitos_donor_async.h:12` 写得很直白:写路径只碰 `donor_async_runway()`,「a plain memory read: no ioctl, no io_uring」。

异步这个说法是对的,给它安的机制不对。另外,内核 6.6.5 的 io_uring 没有通用 ioctl 操作码,所以这句不只是「没实现」,按字面也做不到——做 io_uring 的审稿人会直接问哪个操作码承载 `MOVE_EXT`。**建议改成** `we perform the extent transfer on a background preparer thread, off the critical path`,io_uring 留在它真正所在的设备路径上。

### 2.4 同一句里 io_uring 被引到了 ioctl(2) 手册页

**论文位置** Section IV-D,行内小标题 `Optimizations.`,我们源文件 `design2.tex:507`。

`\cite{iouring,ioctlWik84:online}` 渲染成 `[16, 18]`。其中 `ioctlWik84:online`(我们 `ioctl4.bib:152`,编号 [16])是 `ioctl: System calls manual`,`iouring`(`ioctl4.bib:166`,编号 [18])才是 Axboe 的 io_uring 文档。**改成只留 `\cite{iouring}`。**

### 2.5 摘要把两个最好情况的数字当成一般结果,而且量级写反了

**论文位置** 摘要,我们源文件 `hotstorage/abs.tex:20`。

原句:`that \odes boosts throughput **by** 2.1$\times$ and 1.6$\times$ for OceanBase and MySQL, respectively.`

2.1× 是单客户端 OLTP-write-only 那一个点,同一节在 32 客户端下报的是 21.4%;1.6× 是 OLTP-all-insert 那一个点,另外两个 MySQL 负载是 53.3% 和 21.0%。引言写的是「up to 2.1×」,摘要把限定词丢了。另外 `boosts throughput **by** 2.1×` 把一个 +110% 的结果说成 +210%,实测是吞吐**达到** 2.1× 和 1.6×。**改法:把 `by` 改成 `to`,并考虑补回 `up to`。**

### 2.6 「60.2% ... on average」与第三节自相矛盾

**论文位置** Section I,我们源文件 `hotstorage/intro.tex:51`;与之矛盾的是 Section III,`mot2.tex:152-154`。

- 引言(`hotstorage/intro.tex:51`):`the software tax accounts for 60.2\% time for a logging write on average.`
- 第三节 𝕆2(`mot2.tex:152-154`):`the runtime software tax increasingly dominates I/O latency as devices become faster, reaching 60.2\% for the NVMe SSD.`

图 1b 里 HDD 的软件时间约 8%,不带掉电保护的 SATA SSD 约 17%,所以 60.2% 不是四个设备的平均值,是最大值。**改引言:把 `on average` 换成 `on a low-latency NVMe SSD`。** 这两行是全文仅有的两处 `60.2`。

---

## 三、被删掉的段落(按您邮件不进 camera-ready,列此备查)

按您邮件,camera-ready 按审稿版本提交,所以这一节里的内容**不建议放进 camera-ready**。列在这里有两个用处:一是若 arXiv 版愿意收,它们各自回答一个审稿人的标准问题;二是 Section IV-A 的一处承诺现在没有内容对应。

**那处承诺**:Section IV-A,我们源文件 `design2.tex:42` —— `we optimize \odes in the dimensions of scalability, comprehensiveness, and compatibility (Section \ref{sec:design:discussion})`。您那一版的 Section IV-D 现在只剩 `Optimizations`、`Permission checks`、`Compatibility` 三个小标题,`comprehensiveness` 这一维没有内容对应。**如果不恢复下面任何一段,建议把 `comprehensiveness` 从这句承诺里去掉**——这一处改动只动一个词,不算新增内容。

| 被删段落 | 论文位置 / 我们源文件 | 审稿人问题 | 代码上站得住吗 |
|---|---|---|---|
| `Crash consistency.` | Section IV-D,`design2.tex:566` | 「你在写路径上绕过了文件系统,崩溃之后 ext4 的元数据和你的裸写怎么对得上?」 | 站得住,而且最好写。Maco 每次注册时从 FIEMAP 重建(`src/extent.c:280` 的 `exitos_extent_load_maco`),是纯内存数组、不落盘,运行期映射变化只经过写日志的 move-extent。诚实的答案就是「我们不引入任何自己的持久状态」 |
| `Stale mappings.` | Section IV-D,`design2.tex:589` | 「文件在你缓存映射期间被截断、轮转、改名了怎么办?」 | 站得住。`maco_invalidate()`(`include/exitos_maco.h:26`)、逐写描述符身份检查(`src/intercept.c:1765-1782`) |
| `Error reporting.` | Section IV-D,`design2.tex:591` | 「数据库现在看到的错误会不会不一样?」 | **不要按原样恢复**。我们那段写的是把设备完成状态映射成同样的 errno,代码不是这么做的:任何裸写失败一律返回 `EXITOS_PASS`(`src/intercept.c:1899`),由应用重新走内核路径产生 errno。删掉是对的 |
| `Portability.` | Section IV-D,`design2.tex:640` | 「这套东西只能在 ext4 + NVMe 上用吗?」 | 站得住,而且便宜。异地更新的文件系统覆写时会换块位置,前提不成立;捐赠机制需要区间交换接口,XFS 也有(我们 [26],`ioctl4.bib:230`,随这一段一起从您那一版消失) |
| 技术新颖性那一段 | Section IV-A,`design2.tex:51` | —— | 它的第一点同时是「为什么写裸 LBA 不危险」的安全性论证:预分配日志文件的偏移到块映射在文件保持打开期间不变(`src/maco.c:46`,排序且不重叠的数组),发 I/O 用的块号就是 ext4 自己算出来的那些 |
| 捐赠池用尽 / 准备赶不上写 | Section IV-C,`Moving extents.`,`design2.tex:316` | 「准备是异步的,写超过准备进度会怎样?池子用光应用看到什么?」 | **不要按原样恢复**,我们那两句都不对。代码里:准备失败时 `src/preload.c:1054-1058` 置 `errno` 返回 −1 给应用,不会偷偷改走原生 `fallocate`;写者从不等待准备,`donor_async_runway()` 是一次原子读,读到 0 就走普通内核路径 |

最后一项的**正确写法**(一句):`If the donor pool cannot satisfy an allocation request, if a move-extent operation fails, or if a write arrives before preparation has caught up with it, \odes falls back to Ext4's normal allocation path for that operation.`

---

## 四、引用支撑不住所在句子的地方(两版都有)

这些是我们自己的问题,不是您改出来的。按危害排序,每一条给论文位置和我们的源文件行号。

1. **BypassD「introduces more than 4,200 lines of code (LOC) in the Linux kernel」** —— Section III,`mot2.tex:347`。ASPLOS'24 那篇的表 2 分四项:Kernel 517 行、ext4 1303 行、Device driver 885 行、UserLib 1496 行。UserLib 是用户态库,内核侧合计 **2705** 行。4201 是项目总行数。**这句话的用途正是论证 BypassD 侵入内核,方向对我们有利**,审稿人查到会很难看。建议:`introduces more than 4,200 lines of code, of which about 2,700 are in the Linux kernel.`
2. **「frequent metadata operations persist, mainly due to searching for target block numbers in the per-file mappings [14]」** —— Section III,`mot2.tex:174`。那篇论文说的是相反的话:「If the block mappings are cached in memory (e.g., ext4's extent status tree), obtaining LBAs is inexpensive.」而我们测的正是预分配文件的稳态追加,恰好是那棵树热的情况。它唯一的延迟分解只有一行合并数据「VFS + ext4 2,810 ns 36%」,没有把其中任何一部分归给块映射查找。
3. **OceanBase [1] 被引来支撑两句关于预分配的话** —— Section I,`hotstorage/intro.tex:35`(`a few databases preallocate log files using the fallocate system call`)和 Section II(`initialize log files by preallocating tens of gigabytes by default`)。那篇 PVLDB 论文全文里 `fallocate` 零次、`preallocat` 零次、`log file` 零次。「几十 GB 是默认值」也没找到出处:OceanBase 的部署参数 `log_disk_size` 默认是 0(部署时按实际磁盘算),`log_disk_percentage` 也是 0,「20G」只出现在参数说明的格式示例里。
4. **PLP 的机制被引到 [4]** —— Section I。[4](我们 `ioctl4.bib:31`)讲的是 MEW,一个利用压缩内部碎片存放元数据、加快掉电恢复的 FTL 方案,全文没有出现 `power loss protection` 或 `PLP`;它唯一实质提到超级电容的地方(§3.4)说的是自己的方案**可以减小**超级电容容量。[5] 那份三星应用说明(`ioctl4.bib:43`)确实支撑这句话,应该让它单独承担。
5. **NVMeDirect [12]** —— Section III,`mot2.tex:341-342`:`SPDK and NVMeDirect demand extensive reprogramming efforts due to poor POSIX API compatibility`。HotStorage'16 那篇 §4.3 报的是把 Redis 移植过去「added 6 LOC ... and modified 12 LOC」。十八行是「extensive reprogramming」的反面。真正撑得住的论点在同一篇里:它不提供文件系统,论文自己承认「it cannot provide enough protection normally enforced by the file system layer」。
6. **[6]–[10] 被归成「optimized fsync at the operating system (OS) level」,其中两篇需要改设备** —— Section I,`hotstorage/intro.tex:32-33`。[7] OptFS:「requires a slight change in the disk interface to provide ... asynchronous durability notification」;[10] RFLUSH:RFLUSH 本身是一条新的主机-设备命令,§4 写着「We modified both the file system (F2FS) and the storage device (BlueDBM)」,BlueDBM 是一块 FPGA 闪存平台。**这一条格外要紧,因为「纯软件、不改硬件」正是表 I 和引言里 Éxitos 的卖点**,这句话把自己赖以区分的那条线抹掉了。建议 [6]、[8]、[9] 留在「at the OS level」,[7]、[10] 移到后面一个说「也改了设备接口」的从句里。
7. **表 I 里 Moneta-D 的「Compatibility with SATA SSD ✓」撑不住** —— Section III,表 I 的 Moneta-D 行在 `mot2.tex:270`(第五列)。那篇论文的原型「runs at 250 MHz on a BEE3 FPGA prototyping system」,用 DRAM 加改过的内存控制器模拟相变存储,全文没有出现 SATA。同一行已经因为需要自己的板子而在「No hardware change」打了 ×。
8. **表 I 里 SPDK 的同一格也是 ✓** —— `mot2.tex:268`。CloudCom'17 那篇通篇讲 NVMe,SPDK 自己的文档说「The bedrock of SPDK is a user space, polled-mode, asynchronous, lockless NVMe driver」,没有 SATA/AHCI 驱动。SPDK 够到 SATA 设备只能走 `aio` bdev 模块,那是绕回内核的路。这一格对我们不利的程度低于第 7 条(它是对基线宽容)。

---

## 五、核查过、确认没有问题的部分

- **第二节和第三节逐词相同**,没有删掉任何测量、前提或交叉引用。两种办法验证过:对指定区间做词集合差分,除本文档报告的几项和排版换行外没有残留;以及把两份 PDF 的第 2、3 页按 150 dpi 渲染出来比对,图 1a、图 1b、图 2、表 I 完全一致。9 页压到 8 页发生在第四节(我们该节抽出的正文 264 行,您那一版 153 行)。
- **四个观察 𝕆1–𝕆4 全部保留且与各自的图自洽**:35MB/s→166MB/s、60.2% 与 46.3%、2.6×/3.2×/3.4×、表 I 的二十五个格子逐格未变。
- **所有数字未被改动**:两版共有的每一个数字都相同,包括实验配置(16 核、64GB、1GB 文件、4KB+fdatasync、16KB、16 线程、10GB、4K–128K、64KB/256KB/1MB、一千万行表、30 分钟)。每个保留下来的数字都保留了它的条件。唯一消失的数字是 Section V-D 里那组关键路径批量 `fallocate` 的对照值 2.6×、3.2×、3.4×,随所在的那句话一起被删掉了。
- **九张图全部在,全部被引用过**;图 6 的 (a)–(c) 和图 7 的 (a)–(f) 子图都在;表 I 在且被引用两次。没有「引用了但缺图」或「有图但没引用」的情况。
- **章节编号与交叉引用全部对得上**:I–VI 顺序正确,IV-A…IV-D 与 V-A…V-E 顺序正确,正文里每一处 `Section` 引用都能解析。评测路线图承诺四个问题,V-B…V-E 四个小节逐一回答;V-C 说「we derive five observations」,后面确实是五条。
- **第六节 Conclusion 与我们那一版逐词相同**。`ACKNOWLEDGMENT` 标题没了,但文字没丢,移到了首页脚注(基金号 2022YFB4401700、上科大启动经费、通讯作者),这是正常的 camera-ready 处理。
- **没有虚构的文献。** [1]–[26] 全部真实存在,[1]–[13] 的作者、年份、会场、卷期、页码逐条对过原始出处,没有一条错;正文里 [1]–[26] 每一条都被引用过,没有超过 26 的引用号。重新编号之后有四处「句子没动、编号变了」,逐一核过都还指向同一件作品。被删的两条([23] bpftime、[26] XFS range-exchange)是随各自的句子和段落一起走的,没有留下悬空引用;不过 [23] 建议按 2.1 加回来。
- **不打算报告的差异**:新增的第二单位、移到首页的基金脚注、压缩后的文献格式、图 3 / 图 4 浮动体位置的调整,都属编辑层面的决定。

另有一条两版都有:[7](`ioctl4.bib:60`,OptFS)列在文献表里但正文从未单独引用,只出现在 [6]–[10] 那个成组引用里。

---

## 六、取不到的东西,分清是哪一种

- **IEEE Xplore** 对自动抓取返回 HTTP 418 且只走 JavaScript。[4] 的书目信息因此走 Crossref、OpenAlex、Semantic Scholar(三者都带 IEEE 自己提交的记录),全文用的是 KAIST OSLab 作者主页上挂的 PDF(文件名就是 IEEE 文章号 07172998,可据此确认同一篇)。要读 Xplore 页面本身,需要人在浏览器里取。
- **ACM 数字图书馆**返回 403。[14] 因此走 Crossref 加合作者主页的 PDF(`cgi.di.uoa.gr/~vkarakos/papers/asplos24_bypassd.pdf`),两者对得上(页码 35–51,正好 17 页,与 ACM Reference Format 里的「17 pages」一致)。
- **usenix.org** 对不带浏览器 User-Agent 的自动抓取返回 403,带上之后正常,所有 USENIX 条目都是从真实的会议录页面读的。
- **intel.com** 带浏览器 User-Agent 时对新旧两个地址都返回 403(机器人过滤),所以判 [21] 是 404 用的是不带 UA 那一次,那一次返回的是 Intel 自己的错误页。
- **两条结论用的是存档而不是实时页面,日期就写在结论旁边**:[22] 的内容取自 **2020-09-24** 的 Wayback 快照(`confluera.com` 今天不解析);[18] 的文档身份取自 **2026-06-24** 的快照(`kernel.dk/io_uring.pdf` 今天返回 404,最早一条 404 记录是 2026-07-22,也就是这个地址大约两个月前才坏;文档本身是真的,8 页,版本行写着 `Version: 0.4, 2019-10-15`)。这一条影响我们 `ioctl4.bib:169` 那个地址,两版都受影响。
