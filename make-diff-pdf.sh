#!/usr/bin/env bash
#
# latexdiff-pdf.sh -- typeset one PDF that shows what changed between two
# versions of a LaTeX document, the way a diff view shows it: deleted text
# struck through in red, added text wavy-underlined in blue. Those two colours
# are latexdiff's own convention, which co-authors and reviewers already read
# fluently, so this script does not invent its own palette.
#
# Both versions come from git, so the two sides can be any pair of commits,
# tags, or branches, and either side can be the current working tree.
#
#   ./latexdiff-pdf.sh                     # baseline tag or first commit -> working tree
#   ./latexdiff-pdf.sh v1                  # tag v1 -> working tree
#   ./latexdiff-pdf.sh v1 v2               # tag v1 -> tag v2
#   ./latexdiff-pdf.sh HEAD~5 HEAD         # five commits back -> HEAD
#   ./latexdiff-pdf.sh submitted worktree  # "worktree" names the working tree
#
# With no OLD given the script looks for a tag named accepted, submitted,
# camera-ready, or v1 (in that order) and falls back to the repository's first
# commit.
#
# Environment overrides:
#   MAIN=paper.tex     main .tex file; autodetected when unset
#   OUT=changes.pdf    output file; default <main>-diff.pdf
#   TYPE=CFONT         latexdiff markup style; default UNDERLINE (see --help)
#   NOBIB=1            skip the bibliography pass (references then carry no markup)
#   NOPOST=1           skip the ghostscript pass that embeds fonts and linearises
#   KEEP=1             keep the scratch directory and print its path
#
# Requirements: git, latexdiff, latexmk, pdflatex, python3.
#   Debian/Ubuntu: apt-get install -y latexdiff latexmk texlive-latex-extra ghostscript
#   macOS:         brew install latexdiff ghostscript   (MacTeX supplies latexmk)
# Optional but recommended: bibtex (bibliography markup), ghostscript (font
# embedding), poppler-utils (page count and render check).
#
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }
note() { echo "$*" >&2; }

for tool in git latexdiff latexmk pdflatex python3; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed.
       Debian/Ubuntu: apt-get install -y latexdiff latexmk texlive-latex-extra ghostscript"
done

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not inside a git repository."
REPO=$(git rev-parse --show-toplevel)
cd "$REPO"

# ---------------------------------------------------------------- pick the sides
OLD_REF=${1:-}
NEW_REF=${2:-worktree}

if [ -z "$OLD_REF" ]; then
    for candidate in accepted submitted camera-ready v1; do
        if git rev-parse -q --verify "refs/tags/$candidate" >/dev/null; then
            OLD_REF=$candidate; break
        fi
    done
    [ -n "$OLD_REF" ] || OLD_REF=$(git rev-list --max-parents=0 HEAD | tail -1)
fi

git rev-parse -q --verify "$OLD_REF^{commit}" >/dev/null \
    || die "'$OLD_REF' is not a commit, tag, or branch in this repository."
if [ "$NEW_REF" != "worktree" ]; then
    git rev-parse -q --verify "$NEW_REF^{commit}" >/dev/null \
        || die "'$NEW_REF' is not a commit, tag, or branch in this repository. Use 'worktree' for uncommitted work."
fi

WORK=$(mktemp -d)
if [ -n "${KEEP:-}" ]; then
    trap 'echo "scratch kept at $WORK" >&2' EXIT
else
    trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK/old" "$WORK/new" "$WORK/build"

git archive "$OLD_REF" | tar -x -C "$WORK/old"
if [ "$NEW_REF" = "worktree" ]; then
    # Tracked files as they stand right now, uncommitted edits included.
    git ls-files -z | while IFS= read -r -d '' f; do
        mkdir -p "$WORK/new/$(dirname "$f")"
        cp -a "$f" "$WORK/new/$f" 2>/dev/null || true
    done
else
    git archive "$NEW_REF" | tar -x -C "$WORK/new"
fi

# ------------------------------------------------------------ find the main file
if [ -z "${MAIN:-}" ]; then
    mapfile -t CANDIDATES < <(
        cd "$WORK/new" && grep -rl --include='*.tex' -e '\\begin{document}' . 2>/dev/null \
            | while read -r f; do grep -q '\\documentclass' "$f" && echo "${f#./}"; done | sort
    )
    case ${#CANDIDATES[@]} in
        0) die "no .tex file with \\documentclass and \\begin{document} found. Set MAIN=<file>." ;;
        1) MAIN=${CANDIDATES[0]} ;;
        *) die "several main files found; set MAIN=<file> to pick one:
$(printf '         %s\n' "${CANDIDATES[@]}")" ;;
    esac
fi
[ -f "$WORK/new/$MAIN" ] || die "$MAIN is not present in $NEW_REF."
[ -f "$WORK/old/$MAIN" ] || die "$MAIN is not present in $OLD_REF."

STEM=$(basename "$MAIN" .tex)
OUT=${OUT:-${STEM}-diff.pdf}
TYPE=${TYPE:-UNDERLINE}

note "old      : $OLD_REF"
note "new      : $NEW_REF"
note "main     : $MAIN"
note "markup   : $TYPE (latexdiff default: red strikethrough = removed, blue wavy underline = added)"

# ------------------------------------------------- bibliography, so that added
# references show up as changes too. latexdiff can only fold \bibliography into
# the document when a .bbl already exists beside it, so build one on each side.
if [ -z "${NOBIB:-}" ] && command -v bibtex >/dev/null 2>&1 \
   && grep -q '\\bibliography{' "$WORK/new/$MAIN"; then
    note "building bibliographies ..."
    for side in old new; do
        ( cd "$WORK/$side" \
          && pdflatex -interaction=batchmode -draftmode "$MAIN" >/dev/null 2>&1 \
          && bibtex "$STEM" >/dev/null 2>&1 ) || note "  note: bibliography pass failed for $side; its references will carry no markup"
    done
fi

# ------------------------------------------------------------------- the diff
# --flatten pulls every \input, \include, and .bbl into one document, so the
# whole paper is compared in a single pass rather than file by file.
note "running latexdiff ..."
latexdiff --encoding=utf8 --flatten --type="$TYPE" \
          "$WORK/old/$MAIN" "$WORK/new/$MAIN" \
          2> "$WORK/latexdiff.err" > "$WORK/build/$STEM-diff.tex" \
    || { note "latexdiff failed:"; grep -v 'Wide character' "$WORK/latexdiff.err" >&2 | head -20; exit 1; }

# A macro whose definition ends in \xspace swallows the space in front of the
# markup latexdiff inserts, so "\sys \DIFadd{scales ...}" would set as
# "Sysscales ...". Writing the macro as \sys{} stops \xspace from inspecting the
# markup, and the space already in the source then prints normally.
python3 - "$WORK/new" "$WORK/build/$STEM-diff.tex" <<'PYFIX'
import os, re, sys
srcdir, difffile = sys.argv[1], sys.argv[2]
text = open(difffile, encoding='utf-8', errors='replace').read()

# 1. Spacing after macros whose definition ends in \xspace.
defs = re.compile(r'\\(?:new|renew|provide)command\*?\s*\{?\\([A-Za-z]+)\}?\s*(?:\[\d+\])?\s*\{[^{}]*\\xspace')
macros = set()
for root, _dirs, files in os.walk(srcdir):
    for name in files:
        if name.endswith(('.tex', '.sty', '.cls')):
            try:
                macros.update(defs.findall(open(os.path.join(root, name), encoding='utf-8', errors='replace').read()))
            except OSError:
                pass
spaced = 0
for m in sorted(macros):
    pat = re.compile(r'\\' + m + r'(?![A-Za-z])(\s+)(?=\\DIF(?:add|del)(?:FL)?\{)')
    spaced += len(pat.findall(text))
    text = pat.sub(lambda mo, name=m: '\\' + name + '{}' + mo.group(1), text)

# 2. Markup wrapped around a length or glue value. TeX primitives such as
# \hskip want a number next, and a \DIFadd{...} in that position raises
# "Missing number, treated as zero" followed by "Illegal unit of measure".
# IEEEtran's .bst emits exactly this shape (\hskip 1em plus 0.5em minus 0.4em),
# so any paper in an IEEE class hits it as soon as a reference is added.
# The repair keeps the new value and drops the old one, which is what the
# typeset result should use, and leaves the surrounding markup alone.
LENGTH_CMDS = ('hskip|vskip|kern|hspace|vspace|raisebox|rule|setlength|addtolength|'
               'itemsep|parsep|topsep|partopsep|parskip|baselineskip|leftmargin|'
               'labelsep|labelwidth|columnsep|arraycolsep|tabcolsep|abovedisplayskip|'
               'belowdisplayskip|hsize|vsize|textwidth|linewidth|columnwidth')
GLUE = r'[-+]?[\d.]*\s*(?:em|ex|pt|bp|cm|mm|in|pc|dd|cc|sp|\\[A-Za-z]+)[^{}]*'

def keep_new(mo):
    return mo.group('cmd') + mo.group('star') + ' ' + mo.group('new')

# \cmd \DIFdel{old}\DIFadd{new}  ->  \cmd new
pat_pair = re.compile(
    r'(?P<cmd>\\(?:' + LENGTH_CMDS + r'))(?P<star>\*?)\s*'
    r'\\DIFdel(?:FL)?\{(?P<old>' + GLUE + r')\}\s*'
    r'(?:%\n)?\s*\\DIFadd(?:FL)?\{(?P<new>' + GLUE + r')\}')
text, n1 = pat_pair.subn(keep_new, text)

# \cmd \DIFadd{value} or \cmd \DIFdel{value}  ->  \cmd value
pat_single = re.compile(
    r'(?P<cmd>\\(?:' + LENGTH_CMDS + r'))(?P<star>\*?)\s*'
    r'\\DIF(?:add|del)(?:FL)?\{(?P<new>' + GLUE + r')\}')
text, n2 = pat_single.subn(keep_new, text)
lengths = n1 + n2

open(difffile, 'w', encoding='utf-8').write(text)
print(f"  repaired spacing after {len(macros)} \\xspace macro(s) in {spaced} place(s); "
      f"unwrapped markup around {lengths} length value(s)", file=sys.stderr)
PYFIX

# ---------------------------------------------------------------- compile it
# Everything the document loads by a path relative to the main file -- figures,
# class files, style files -- has to sit beside the diff source. The new side is
# linked first; the old side fills in anything the new version dropped.
MAINDIR=$(dirname "$MAIN")
for side in new old; do
    ( cd "$WORK/$side/$MAINDIR" 2>/dev/null && find . -mindepth 1 -maxdepth 1 \
        ! -name '*.tex' ! -name '*.aux' ! -name '*.log' ! -name '*.out' \
        ! -name '*.bbl' ! -name '*.blg' ! -name '*.fls' ! -name '*.fdb_latexmk' \
        ! -name '*.toc' ! -name '*.synctex.gz' -print0 ) \
        | while IFS= read -r -d '' entry; do
            target="$WORK/build/${entry#./}"
            [ -e "$target" ] || cp -a "$WORK/$side/$MAINDIR/${entry#./}" "$target"
        done
done

note "compiling ..."
( cd "$WORK/build" && latexmk -pdf -interaction=nonstopmode "$STEM-diff.tex" > compile.log 2>&1 ) || {
    note "the marked-up document did not compile. Last lines of the log:"
    tail -40 "$WORK/build/compile.log" >&2
    note ""
    note "This usually means latexdiff placed markup somewhere LaTeX will not take it."
    note "Re-run with TYPE=CFONT (no ulem, tolerates more contexts), or exclude the"
    note "offending command with latexdiff's --exclude-textcmd."
    exit 1
}
[ -f "$WORK/build/$STEM-diff.pdf" ] || die "no PDF was produced."

# ------------------------------------------------- make the PDF easy to render
# Figures often reference a font without embedding it, and a viewer that cannot
# find that font locally draws the page wrong or refuses it. Ghostscript rewrites
# the file with every font embedded. Linearisation is deliberately NOT requested
# here: ghostscript's -dFastWebView writes a hint table that poppler and several
# browser viewers reject, which is worse than not linearising at all. If anything
# about the rewrite looks wrong, the original is kept instead.
RESULT="$WORK/build/$STEM-diff.pdf"
if [ -z "${NOPOST:-}" ] && command -v gs >/dev/null 2>&1; then
    note "embedding fonts ..."
    if gs -q -o "$WORK/build/post.pdf" -sDEVICE=pdfwrite \
          -dCompatibilityLevel=1.5 -dPDFSETTINGS=/prepress \
          -dEmbedAllFonts=true -dSubsetFonts=true \
          -dAutoRotatePages=/None -dNOPAUSE -dBATCH "$RESULT" >/dev/null 2>&1 \
       && [ -s "$WORK/build/post.pdf" ]; then
        if command -v pdfinfo >/dev/null 2>&1; then
            before=$(pdfinfo "$RESULT" | awk '/^Pages:/{print $2}')
            after=$(pdfinfo "$WORK/build/post.pdf" | awk '/^Pages:/{print $2}')
            if [ "$before" = "$after" ]; then RESULT="$WORK/build/post.pdf"; else
                note "  note: page count changed ($before -> $after); keeping the unprocessed PDF"
            fi
        else
            RESULT="$WORK/build/post.pdf"
        fi
    else
        note "  note: ghostscript pass failed; keeping the unprocessed PDF"
    fi
fi

# -------------------------------------------------------------- prove it draws
# "It compiled" is not "it renders". Draw every page and fail loudly if any one
# of them errors, so a broken file is never handed over as finished.
if command -v pdftoppm >/dev/null 2>&1 && command -v pdfinfo >/dev/null 2>&1; then
    PAGES=$(pdfinfo "$RESULT" | awk '/^Pages:/{print $2}')
    note "checking that all $PAGES pages render ..."
    bad=0
    for p in $(seq 1 "$PAGES"); do
        pdftoppm -png -r 30 -f "$p" -l "$p" "$RESULT" "$WORK/build/probe" 2>"$WORK/build/probe.err" \
            || { note "  page $p failed to render:"; sed 's/^/    /' "$WORK/build/probe.err" >&2; bad=1; }
        rm -f "$WORK/build"/probe-*.png
    done
    [ "$bad" -eq 0 ] || die "the PDF does not render cleanly; not writing $OUT."
elif command -v gs >/dev/null 2>&1; then
    note "checking that the PDF parses ..."
    gs -o /dev/null -sDEVICE=nullpage -dNOPAUSE -dBATCH -dQUIET "$RESULT" >/dev/null 2>&1 \
        || die "the PDF does not parse cleanly; not writing $OUT."
fi

case "$OUT" in
    /*) DEST=$OUT ;;
    *)  DEST=$REPO/$OUT ;;
esac
cp "$RESULT" "$DEST"
PAGES=${PAGES:-?}
SIZE=$(du -h "$DEST" | cut -f1)
echo
echo "wrote $OUT ($PAGES pages, $SIZE)"
echo "  red, struck through   = removed between $OLD_REF and $NEW_REF"
echo "  blue, wavy underlined = added between $OLD_REF and $NEW_REF"
