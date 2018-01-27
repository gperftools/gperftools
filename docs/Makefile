# A simple Makefile to convert MarkDown documentation to HTML
# An easy way to browse gperftools documentation though is to browse it online at:
#   https://github.com/f18m/gperftools
# Still HTML can be useful for offline browsing. If you want HTML, please install pandoc from 
#   http://pandoc.org/
# and then call this Makefile.

MARKDOWN = pandoc --from markdown_github --to html --standalone

all: $(patsubst %.md,%.html,$(wildcard *.md)) Makefile

clean:
    rm -f $(patsubst %.md,%.html,$(wildcard *.md))
    rm -f *.bak *~

%.html: %.md
    $(MARKDOWN) $< --output $@
