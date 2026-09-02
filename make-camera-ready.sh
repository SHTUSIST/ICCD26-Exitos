#!/usr/bin/env bash
#
# make-camera-ready.sh -- build the submission PDF with every font embedded.
#
#     ./make-camera-ready.sh
#
# Why this exists rather than a bare latexmk run: two of the figures were
# produced by tools that reference a font without embedding it, or embed a font
# file whose format does not match what the font dictionary declares. IEEE PDF
# eXpress rejects a submission that carries a non-embedded font, and a viewer
# that cannot find the font locally draws the page wrong or refuses it. Passing
# the finished document through ghostscript once rewrites every font reference
# into an embedded subset and normalises the font dictionaries.
#
# Run ghostscript over the FINISHED DOCUMENT, never over a figure on its own.
# fig/design/structure.pdf embeds a math font that ghostscript cannot parse; on
# the standalone figure it substitutes a fallback face and the circled step
# markers turn into unrelated CJK glyphs. Once pdfTeX has embedded and subset
# that font into the document, ghostscript reads it correctly and the markers
# survive -- verified by rendering page 4 and comparing.
#
# Linearisation is deliberately not requested: ghostscript's -dFastWebView
# writes a hint table that poppler and several browser viewers reject.
#
set -euo pipefail

MAIN=${MAIN:-ICCD26-main.tex}
OUT=${OUT:-ICCD26-Exitos-camera-ready.pdf}
STEM=$(basename "$MAIN" .tex)

command -v latexmk >/dev/null || { echo "error: latexmk is not installed." >&2; exit 1; }

echo "compiling ..."
latexmk -pdf -interaction=nonstopmode -outdir=build "$MAIN" > build.log 2>&1 || {
    echo "error: the paper did not compile. Last lines of build.log:" >&2
    tail -30 build.log >&2
    exit 1
}
RESULT=build/$STEM.pdf

if command -v gs >/dev/null 2>&1; then
    echo "embedding fonts ..."
    if gs -q -o build/embedded.pdf -sDEVICE=pdfwrite \
          -dCompatibilityLevel=1.5 -dPDFSETTINGS=/prepress \
          -dEmbedAllFonts=true -dSubsetFonts=true \
          -dAutoRotatePages=/None -dNOPAUSE -dBATCH "$RESULT" >/dev/null 2>&1 \
       && [ -s build/embedded.pdf ]; then
        before=$(pdfinfo "$RESULT" 2>/dev/null | awk '/^Pages:/{print $2}')
        after=$(pdfinfo build/embedded.pdf 2>/dev/null | awk '/^Pages:/{print $2}')
        if [ "$before" = "$after" ]; then RESULT=build/embedded.pdf; else
            echo "  note: page count changed ($before -> $after); keeping the unprocessed PDF" >&2
        fi
    else
        echo "  note: ghostscript pass failed; keeping the unprocessed PDF" >&2
    fi
else
    echo "  note: ghostscript not installed; fonts are NOT embedded and PDF eXpress will reject this" >&2
fi

# Compiling is not rendering. Draw every page and refuse to hand over a file
# that any viewer chokes on, and report any font the result still leaves out.
if command -v pdftoppm >/dev/null 2>&1 && command -v pdfinfo >/dev/null 2>&1; then
    PAGES=$(pdfinfo "$RESULT" | awk '/^Pages:/{print $2}')
    echo "checking that all $PAGES pages render ..."
    bad=0
    for p in $(seq 1 "$PAGES"); do
        pdftoppm -png -r 30 -f "$p" -l "$p" "$RESULT" build/probe 2>build/probe.err \
            || { echo "  page $p failed to render" >&2; bad=1; }
        [ -s build/probe.err ] && { echo "  page $p: $(cat build/probe.err)" >&2; bad=1; }
        rm -f build/probe-*.png build/probe.err
    done
    [ "$bad" -eq 0 ] || { echo "error: the PDF does not render cleanly; not writing $OUT." >&2; exit 1; }
fi
if command -v pdffonts >/dev/null 2>&1; then
    missing=$(pdffonts "$RESULT" 2>/dev/null | awk 'NR>2 && $(NF-3)=="no"{print $1}')
    [ -z "$missing" ] || { echo "error: fonts still not embedded: $missing" >&2; exit 1; }
fi

cp "$RESULT" "$OUT"
echo
echo "wrote $OUT (${PAGES:-?} pages, $(du -h "$OUT" | cut -f1)); every font embedded"
