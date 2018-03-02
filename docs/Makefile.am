## Process this file with automake to produce Makefile.in
#
# A simple Makefile to convert MarkDown documentation to HTML
# An easy way to browse gperftools Markdown documentation is to browse it online at:
#   https://github.com/gperftools/gperftools
# but HTML can still be useful for offline browsing. This makes generates HTML from Markdown.
# Please install pandoc from 
#   http://pandoc.org/
# and then call this Makefile.

MARKDOWN_DOCUMENTATION = \
	cpuprofile-fileformat.md \
	cpuprofile.md \
	heap_checker.md \
	heap_profiler.md \
	pprof_remote_servers.md \
	readme.md \
	tcmalloc.md
	
ADDITIONAL_IMAGES = \
	images/overview.gif \
	images/pageheap.gif \
	images/spanmap.gif \
	images/threadheap.gif \
	images/heap-example1.png \
	images/pprof-test-big.gif \
	images/pprof-test.gif \
	images/pprof-vsnprintf-big.gif \
	images/pprof-vsnprintf.gif \
	images/tcmalloc-opspercpusec.vs.threads.1024.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.128.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.131072.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.16384.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.2048.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.256.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.32768.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.4096.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.512.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.64.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.65536.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.8192.bytes.png 	\
	images/tcmalloc-opspersec.vs.size.1.threads.png 		\
	images/tcmalloc-opspersec.vs.size.12.threads.png 		\
	images/tcmalloc-opspersec.vs.size.16.threads.png 		\
	images/tcmalloc-opspersec.vs.size.2.threads.png 		\
	images/tcmalloc-opspersec.vs.size.20.threads.png 		\
	images/tcmalloc-opspersec.vs.size.3.threads.png 		\
	images/tcmalloc-opspersec.vs.size.4.threads.png 		\
	images/tcmalloc-opspersec.vs.size.5.threads.png 		\
	images/tcmalloc-opspersec.vs.size.8.threads.png
	
ADDITIONAL_SUPPORT_DOCS = \
	t-test1.times.txt \
	images/overview.dot \
	images/pageheap.dot \
	images/spanmap.dot \
	images/threadheap.dot

# Documentation always installed:
dist_doc_DATA = $(MARKDOWN_DOCUMENTATION) $(ADDITIONAL_IMAGES) $(ADDITIONAL_SUPPORT_DOCS)

# If we are not compiling with stacktrace support, pprof is worthless
if WITH_STACK_TRACE
dist_man_MANS = pprof.1
endif

if FOUND_CONVERTER_FOR_HTML
MARKDOWN = pandoc --from markdown_github --to html --standalone

# Rule to convert markdown to HTML in 2 steps:
# 1) convert markdown to HTML:
# 2) convert markdown references to HTML ones:
%.html: %.md
	$(MARKDOWN) $< --output $@
	sed -i 's@href="\(.*\).md"@href="\1.html"@g' $@

HTML_FILES = \
	cpuprofile-fileformat.html \
	cpuprofile.html \
	heap_checker.html \
	heap_profiler.html \
	pprof_remote_servers.html \
	readme.html \
	tcmalloc.html

dist_doc_DATA += $(HTML_FILES)
CLEANFILES = $(HTML_FILES)

endif
