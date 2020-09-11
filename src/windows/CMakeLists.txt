add_executable(addr2line-pdb addr2line-pdb.c)
target_link_libraries(addr2line-pdb dbghelp)

add_executable(nm-pdb nm-pdb.c)
target_link_libraries(nm-pdb dbghelp)

#enable_language(ASM)
#add_executable(preamble_patcher_test preamble_patcher_test.cc shortproc.asm)
#target_link_libraries(preamble_patcher_test tcmalloc_minimal)
#add_test(preamble_patcher_test preamble_patcher_test)