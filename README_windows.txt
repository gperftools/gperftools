--- COMPILING

Windows builds use CMake (3.15 or later) and any generator/toolchain
supported by it, e.g. Visual Studio or Ninja with MSVC:

    cmake -S . -B build -Dgperftools_build_minimal=ON
    cmake --build build --config Release
    ctest --test-dir build -C Release

Only tcmalloc_minimal is supported at this time.  Unit tests are built
along with the library and can be run via ctest.

To use tcmalloc_minimal in your own project, link against the
tcmalloc_minimal library (CMake target) or, when linking by hand, add
the following to the linker line of your executable:
   "libtcmalloc_minimal.lib" /INCLUDE:"__tcmalloc"

The /INCLUDE is necessary so the linker doesn't "optimize out" the
libtcmalloc dependency.  In Visual Studio it can be set in Project
Properties -> Linker -> Input -> "Force Symbol References".

You can also link tcmalloc code in statically.  For this to work,
you'll need to add "/D PERFTOOLS_DLL_DECL=" to the compile line of
every perftools .cc file.  You do not need the __tcmalloc symbol
reference in this case.

--- OVERRIDE MODE

An alternative to all the above is "override" mode: statically link
your application with the CRT (/MT), and have tcmalloc replace the
CRT's malloc and friends at link time instead of patching them at
run time.  This is more reliable than run-time patching (which is
inherently fragile).  Enable it with:

    cmake -S . -B build -Dgperftools_build_minimal=ON \
          -Dgperftools_windows_override=ON -DBUILD_SHARED_LIBS=OFF \
          -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded

It requires a static build of tcmalloc and a static CRT.  Your
executable still needs to pull tcmalloc in at link time, e.g. with
/INCLUDE:__tcmalloc as described above.


--- ISSUES

NOTE ON _MSIZE and _RECALLOC: The tcmalloc version of _msize returns
the size of the region tcmalloc allocated for you -- which is at least
as many bytes you asked for, but may be more.  (btw, these *are* bytes
you own, even if you didn't ask for all of them, so it's correct code
to access all of them if you want.)  Unfortunately, the Windows CRT
_recalloc() routine assumes that _msize returns exactly as many bytes
as were requested.  As a result, _recalloc() may not zero out new
bytes correctly.  IT'S SAFEST NOT TO USE _RECALLOC WITH TCMALLOC.
_recalloc() is a tricky routine to use in any case (it's not safe to
use with realloc, for instance).


If you run across any problems, please report them at
   https://github.com/gperftools/gperftools/issues
