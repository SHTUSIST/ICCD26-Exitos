# arXiv source

The paper's LaTeX source with every comment removed, reduced to the files that
actually take part in the build. This is what gets uploaded to arXiv.

## What is here

```
ICCD26-main.tex          the document; the \iffalse/\else/\fi block is resolved,
                         only the branch that compiled is left
mot2.tex                 Section III
design2.tex              Section IV
eval2.tex                Section V
hotstorage/abs.tex       abstract
hotstorage/intro.tex     Section I
hotstorage/bg.tex        Section II
hotstorage/conclusion.tex Section VI
ioctl4.bib               the bibliography
ICCD26-main.bbl          the formatted bibliography, 28 entries
IEEEtran.cls             the class file
fig/                     21 figures
```

`ICCD26-main.bbl` is included deliberately: arXiv does not run BibTeX, it uses
the `.bbl` that comes with the submission.

Files that took no part in the build are not here — `abs.tex`, `bg2.tex`,
`design2-simple.tex`, `ioctl3.bib`, the build scripts and the build directory.

## This tree is no longer a verbatim copy

Two passages were corrected here after the source tree was copied, so the arXiv version and the
annotated source now differ in content on purpose:

* **Permission checks** (design2.tex, and the matching sentence in *Stale mappings*). The strict
  mode is now described as the zero-length write probe it is: it runs the kernel's own per-write
  permission gate and carries none of the file-system cost. The earlier description, an inode
  attribute check through an ioctl, misses the case it was there to cover -- a tightened SELinux or
  AppArmor policy changes no inode attribute, so an attribute check cannot see it.
* **Optimizations** (design2.tex, plus eval2.tex and hotstorage/intro.tex). The asynchronous extent
  transfer is now attributed to the background preparer thread that performs it, rather than to
  io_uring. io_uring is used on the device submission path and takes no part in moving extents.

Those changes add one page: this tree builds to 10 pages, the annotated source to 9. The extra page
carries the last two references.

## The mechanical part: what was changed, and what was not

Apart from the two corrections above, the rest is mechanical.

Comments were removed in two forms: a line that is entirely a comment is
deleted, and a comment at the end of a line has its text removed. Where the
original had no space before the `%`, the `%` is kept — there it joins the two
lines, and dropping it would insert a space in the middle of a word. No comment
text survives anywhere.

Two figures were renamed because their names contained a space, which arXiv's
compiler handles unreliably:

```
fig/eval/sysbench_ob/sysbench_ob_All Insert_throughput2.pdf
    -> sysbench_ob_All_Insert_throughput2.pdf
fig/eval/sysbench_ob/sysbench_ob_All Insert_latency2.pdf
    -> sysbench_ob_All_Insert_latency2.pdf
```

The two `\includegraphics` lines in `eval2.tex` were updated to match. Apart from
the two corrected passages named above, nothing else was touched: no other
sentence, no number, no citation, no figure content.

## How that was checked

The comment removal was verified before the two corrections above were applied: this tree was
compiled and compared against the PDF built from the annotated source, page by page, and all nine
pages were identical in extracted text and differed by zero pixels at 150 dpi. So the stripping
itself changes nothing in the output; what differs now is only the two corrected passages.

The packaged archive is checked the way it will actually be used: extracted into an empty directory
and compiled there, with no other file present.

## Rebuilding and packaging

```sh
latexmk -pdf -interaction=nonstopmode -outdir=build ICCD26-main.tex
tar czf arxiv-exitos.tar.gz ICCD26-main.tex ICCD26-main.bbl IEEEtran.cls \
    ioctl4.bib mot2.tex design2.tex eval2.tex hotstorage fig
```

Upload the tarball. The archive is built from this directory rather than kept
beside it, so there is only ever one copy of the source to keep current.
