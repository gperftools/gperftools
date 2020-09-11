function(find_objcopy_with_weaken)
  find_program(OBJCOPY_EXECUTABLE "objcopy")
  message(STATUS "Looking for objcopy that supports weaken - ${OBJCOPY_EXECUTABLE}")
  if(NOT OBJCOPY_EXECUTABLE)
    return()
  endif()
  set(objcopy_test_src "${CMAKE_CURRENT_BINARY_DIR}/objcopy_test.c")
  set(objcopy_test_exe "${CMAKE_CURRENT_BINARY_DIR}/objcopy_test")
  file(WRITE ${objcopy_test_src} "void foo() {} int main() { return 0; }")
  try_compile(objcopy_test_compiled
          ${CMAKE_CURRENT_BINARY_DIR} ${objcopy_test_src}
          COPY_FILE ${objcopy_test_exe})
  if(objcopy_test_compiled AND EXISTS ${objcopy_test_exe})
    execute_process(
            COMMAND ${OBJCOPY_EXECUTABLE} -W foo ${objcopy_test_exe}
            RESULT_VARIABLE objcopy_result)
    file(REMOVE ${objcopy_test_exe})
  endif()
  if(objcopy_result EQUAL 0)
    set(objcopy_weaken ON)
  endif()
  file(REMOVE ${objcopy_test_src})
  if(objcopy_weaken)
    set(objcopy_has_weaken "Success")
    set(HAVE_OBJCOPY_WEAKEN TRUE PARENT_SCOPE)
    set(OBJCOPY_EXECUTABLE "${OBJCOPY_EXECUTABLE}" PARENT_SCOPE)
  else()
    set(objcopy_has_weaken "Failed")
  endif()
  message(STATUS "objcopy has weaken support - ${objcopy_has_weaken}")
endfunction(find_objcopy_with_weaken)

function(weaken_object target)
  if(NOT HAVE_OBJCOPY_WEAKEN)
      return()
  endif()
  # If we have objcopy, make malloc/free/etc weak symbols.  That way folks
  # can override our malloc if they want to (they can still use tc_malloc).
  # Note: the weird-looking symbols are the c++ memory functions:
  # (in order) new, new(nothrow), new[], new[](nothrow), delete, delete[]
  # In theory this will break if mangling changes, but that seems pretty
  # unlikely at this point.  Just in case, I throw in versions with an
  # extra underscore as well, which may help on OS X.
  add_custom_command(TARGET ${target} POST_BUILD
          COMMAND "${OBJCOPY_EXECUTABLE}"
          -W malloc -W free -W realloc -W calloc -W cfree
          -W memalign -W posix_memalign -W valloc -W pvalloc
          -W aligned_alloc
          -W malloc_stats -W mallopt -W mallinfo -W nallocx
          -W _Znwm -W _ZnwmRKSt9nothrow_t -W _Znam -W _ZnamRKSt9nothrow_t
          -W _ZdlPv -W _ZdaPv
          -W __Znwm -W __ZnwmRKSt9nothrow_t -W __Znam -W __ZnamRKSt9nothrow_t
          -W __ZdlPv -W __ZdaPv "$<TARGET_FILE:${target}>")
endfunction()