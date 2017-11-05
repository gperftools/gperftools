function(check_omit_fp_by_default result)
  set(src "${CMAKE_CURRENT_BINARY_DIR}/fp.c")
  set(asm "${CMAKE_CURRENT_BINARY_DIR}/fp.s")
  file(WRITE ${src}
       "int f(int x) { return x; } int main() { return f(0); }")
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -O2 -S -o ${asm} ${src}
    RESULT_VARIABLE compiled)
  if(compiled EQUAL 0 AND EXISTS ${asm})
    file(STRINGS ${asm} asm_instructions)
    set(fp_instructions "mov" "rsp" "rbp")
    foreach(asm_instruction IN LISTS asm_instructions)
      list(GET fp_instructions 0 fp_instruction)
      if(asm_instruction MATCHES "${fp_instruction}")
        list(REMOVE_AT fp_instructions 0)
      endif()

      list(LENGTH fp_instructions len)
      if(len EQUAL 0)
        set(matched_all ON)
        break()
      endif()
    endforeach()

    file(REMOVE ${asm})
  endif()

  file(REMOVE ${src})

  if(NOT matched_all)
    set(${result} ON PARENT_SCOPE)
  endif()

  if(result)
    set(fp_omitted "Success")
  else()
    set(fp_omitted "Failed")
  endif()
  message(
    STATUS "Checking if frame pointers are omitted by default - ${fp_omitted}")
endfunction()
