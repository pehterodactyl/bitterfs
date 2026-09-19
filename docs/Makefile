# ===========================================================================
# bitterfs — documentation build
# ===========================================================================
#
#   make            build build/bitterfs.pdf
#   make watch      rebuild continuously as sources change
#   make only DOC=ondisk
#                   build a single chapter while drafting
#   make view       build and open the PDF
#   make clean      remove build products
#
# latexmk runs the engine as many times as needed to settle cross-references
# and the table of contents.  Doing that by hand is the single most annoying
# thing about LaTeX, and it is the entire reason this Makefile does not call
# lualatex directly.
# ===========================================================================

LATEXMK ?= latexmk
MAIN    := bitterfs
OUTDIR  := build

# -lualatex          engine: native UTF-8 + system fonts (see preamble.tex)
# -interaction=nonstopmode
#                    do not stop at the ? prompt on an error; in a Makefile an
#                    interactive prompt just hangs
# -halt-on-error     but DO stop the run, rather than cascading one real error
#                    into fifty confusing follow-ons
# -file-line-error   report errors as file:line:message, so an editor can jump
#                    straight to them
LATEXMKFLAGS := -lualatex -interaction=nonstopmode -halt-on-error \
                -file-line-error -outdir=$(OUTDIR)

SOURCES := $(wildcard *.tex) $(wildcard *.bib)

.PHONY: all watch only view clean help highlight

.DEFAULT_GOAL := all

help:
	@echo 'bitterfs documentation targets:'
	@echo '  make                build $(OUTDIR)/$(MAIN).pdf'
	@echo '  make watch          rebuild continuously (latexmk -pvc)'
	@echo '  make only DOC=name  build a single chapter, e.g. DOC=ondisk'
	@echo '  make view           build and open the PDF'
	@echo '  make clean          remove build products'

all: $(OUTDIR)/$(MAIN).pdf

$(OUTDIR)/$(MAIN).pdf: $(SOURCES)
	$(LATEXMK) $(LATEXMKFLAGS) $(MAIN).tex

# Continuous preview: latexmk watches every dependency and rebuilds on save.
# Ctrl-C to stop.
watch:
	$(LATEXMK) $(LATEXMKFLAGS) -pvc $(MAIN).tex

# Draft a single chapter without recompiling the rest.
#
# \includeonly makes LaTeX skip the other \include files while still reading
# their .aux from the previous full build, so cross-references into them keep
# resolving.  Run a full `make` before trusting the numbering.
only:
	@test -n "$(DOC)" || { echo 'usage: make only DOC=ondisk'; exit 1; }
	@test -f "$(DOC).tex" || { echo "no such chapter: $(DOC).tex"; exit 1; }
	$(LATEXMK) $(LATEXMKFLAGS) -usepretex='\includeonly{$(DOC)}' $(MAIN).tex

# --- Regenerating converted / generated sources ----------------------------
# pandoc-highlight.tex holds the token macros pandoc's output depends on.  It is
# generated rather than hand-written so it stays in step with the installed
# pandoc: a version bump can add token classes, and a missing \SomethingTok is
# an undefined-control-sequence error in a file you did not write.
highlight:
	@printf '```c\nint x;\n```\n' > $(OUTDIR)/.hl.md
	@mkdir -p $(OUTDIR)
	pandoc --standalone $(OUTDIR)/.hl.md -o $(OUTDIR)/.hl.tex
	@echo 'Extract from $(OUTDIR)/.hl.tex into pandoc-highlight.tex by hand —'
	@echo 'the file carries a header and a fvextra block that must be preserved.'

view: all
	xdg-open $(OUTDIR)/$(MAIN).pdf >/dev/null 2>&1 &

# latexmk -C removes the PDF as well as the intermediates; -c would keep it.
clean:
	$(LATEXMK) -C -outdir=$(OUTDIR) $(MAIN).tex
	@rm -rf $(OUTDIR)
