## Process this file with automake to produce Makefile.in
#
# A simple Makefile to convert MarkDown documentation to HTML
# An easy way to browse gperftools documentation though is to browse it online at:
#   https://github.com/f18m/gperftools
# Still HTML can be useful for offline browsing. If you want HTML, please install pandoc from 
#   http://pandoc.org/
# and then call this Makefile.

if FOUND_CONVERTER_FOR_HTML
MARKDOWN = pandoc --from markdown_github --to html --standalone

all: $(patsubst %.md,%.html,$(wildcard *.md)) Makefile

clean:
	rm -f $(patsubst %.md,%.html,$(wildcard *.md))

# 1) convert markdown to HTML:
# 2) convert markdown references to HTML ones:
%.html: %.md
	$(MARKDOWN) $< --output $@
	sed -i 's@href="\(.*\).md"@href="\1.html"@g' $@
endif
