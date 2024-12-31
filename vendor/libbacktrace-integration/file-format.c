// For now we only support elf (most unix-y and ~all post-unixy
// systems) and mach-o thingy that osex does.
#if __ELF__
#include "../libbacktrace/elf.c"
#elif defined(_AIX)
#include "../libbacktrace/xcoff.c"
#else
#include "../libbacktrace/macho.c"
#endif
