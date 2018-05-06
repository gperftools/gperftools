# Process this file with automake to produce Makefile.in
#
# A simple Makefile to convert MarkDown documentation to HTML
# An easy way to browse gperftools Markdown documentation is to browse it online at:
#   https://github.com/gperftools/gperftools
# but HTML can still be useful for offline browsing. This makefile generates HTML from Markdown
# using Pandoc converter. Please install pandoc from
#   http://pandoc.org/
# and then call this Makefile.

MARKDOWN_DOCUMENTATION = \
	cpuprofile-fileformat.md \
	cpuprofile.md \
	heap_checker.md \
	heap_profiler.md \
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
	images/tcmalloc-opspercpusec.vs.threads.1024.bytes.png 		\
	images/tcmalloc-opspercpusec.vs.threads.128.bytes.png 		\
	images/tcmalloc-opspercpusec.vs.threads.131072.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.16384.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.2048.bytes.png 		\
	images/tcmalloc-opspercpusec.vs.threads.256.bytes.png 		\
	images/tcmalloc-opspercpusec.vs.threads.32768.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.4096.bytes.png 		\
	images/tcmalloc-opspercpusec.vs.threads.512.bytes.png 		\
	images/tcmalloc-opspercpusec.vs.threads.64.bytes.png 		\
	images/tcmalloc-opspercpusec.vs.threads.65536.bytes.png 	\
	images/tcmalloc-opspercpusec.vs.threads.8192.bytes.png 		\
	images/tcmalloc-opspersec.vs.size.1.threads.png 			\
	images/tcmalloc-opspersec.vs.size.12.threads.png 			\
	images/tcmalloc-opspersec.vs.size.16.threads.png 			\
	images/tcmalloc-opspersec.vs.size.2.threads.png 			\
	images/tcmalloc-opspersec.vs.size.20.threads.png 			\
	images/tcmalloc-opspersec.vs.size.3.threads.png 			\
	images/tcmalloc-opspersec.vs.size.4.threads.png 			\
	images/tcmalloc-opspersec.vs.size.5.threads.png 			\
	images/tcmalloc-opspersec.vs.size.8.threads.png

ADDITIONAL_IMAGE_DOTFILES = \
	images/overview.dot \
	images/pageheap.dot \
	images/spanmap.dot \
	images/threadheap.dot

# By listing markdown documentation into dist_doc_DATA we get it installed into the
# default folder for Automake projects' data, like e.g. /usr/local/share/doc/gperftools
dist_doc_DATA = $(MARKDOWN_DOCUMENTATION)

# We also need to list the image files. Actually we don't want them to go into
#  /usr/local/share/doc/gperftools
# but rather in /usr/local/share/doc/gperftools/images (see install-data-hook below)
# but if we don't list them in dist_doc_DATA they will not be packaged when running
# "make dist" !
dist_doc_DATA += $(ADDITIONAL_IMAGES) $(ADDITIONAL_IMAGE_DOTFILES)

# If we are not compiling with stacktrace support, pprof is worthless
if WITH_STACK_TRACE
dist_man_MANS = pprof.1
endif

if FOUND_CONVERTER_FOR_HTML
MARKDOWN = pandoc --from markdown_github+pandoc_title_block --to html --standalone

# Rule to convert markdown to HTML in 2 steps:
# 1) convert markdown to HTML:
# 2) convert markdown references to HTML ones:
.md.html:
	$(MARKDOWN) $< --output $@ ; sed -i 's@href="\(.*\).md"@href="\1.html"@g' $@

HTML_FILES = \
	cpuprofile-fileformat.html \
	cpuprofile.html \
	heap_checker.html \
	heap_profiler.html \
	readme.html \
	tcmalloc.html

# By listing HTML documentation into dist_doc_DATA we get it installed into the
# default folder for Automake projects' data, like e.g. /usr/local/share/doc/gperftools
dist_doc_DATA += $(HTML_FILES)
CLEANFILES = $(HTML_FILES)
endif

# Images must go into a SUBDIRECTORY of the default folder for Automake projects' data,
# like e.g. /usr/local/share/doc/gperftools/images
install-data-hook:
	mkdir -p ${docdir}/images
	cp -fv $(ADDITIONAL_IMAGES) $(ADDITIONAL_IMAGE_DOTFILES) ${docdir}/images

