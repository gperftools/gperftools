%define	ver	%VERSION
%define	RELEASE	1
%define rel     %{?CUSTOM_RELEASE} %{!?CUSTOM_RELEASE:%RELEASE}
%define	prefix	/usr

Name: %NAME
Summary: Performance tools for C++
Version: %ver
Release: %rel
Group: Development/Libraries
URL: http://google.sourceforge.net
Copyright: BSD
Packager: El Goog <opensource@google.com>
Source: http://google.sourceforge.net/%{NAME}-%{PACKAGE_VERSION}.tar.gz
Distribution: Redhat 7 and above.
Buildroot: %{_tmppath}/%{name}-root
Docdir: %prefix/doc
Prefix: %prefix

%description
The %name packages contains some utilities to improve and analyze the
performance of C++ programs.  This includes an optimized thread-caching
malloc() and cpu and heap profiling utilities.

%package devel
Summary: Performance tools for C++
Group: Development/Libraries

%description devel
The %name-devel package contains static and debug libraries and header
files for developing applications that use the %name package.

%changelog
    * Tue Feb 8 2005 <opensource@google.com>
    - First draft

%prep
%setup

%build
./configure
make prefix=%prefix

%install
rm -rf $RPM_BUILD_ROOT
make prefix=$RPM_BUILD_ROOT%{prefix} install

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)

%doc AUTHORS COPYING ChangeLog INSTALL NEWS README TODO doc/cpu_profiler.html doc/heap-example1.png doc/heap_profiler.html doc/overview.gif doc/pageheap.gif doc/pprof-test-big.gif doc/pprof-test.gif doc/pprof-vsnprintf-big.gif doc/pprof-vsnprintf.gif doc/spanmap.gif doc/tcmalloc.html doc/threadheap.gif

%{prefix}/lib/libstacktrace.so.0
%{prefix}/lib/libstacktrace.so.0.0.0
%{prefix}/lib/libtcmalloc.so.0
%{prefix}/lib/libtcmalloc.so.0.0.0
%{prefix}/lib/libprofiler.so.0
%{prefix}/lib/libprofiler.so.0.0.0
%{prefix}/lib/libheapprofiler.so.0
%{prefix}/lib/libheapprofiler.so.0.0.0
%{prefix}/lib/libheapchecker.so.0
%{prefix}/lib/libheapchecker.so.0.0.0
%{prefix}/bin/pprof
%{prefix}/man/man1/pprof.1.gz

%files devel
%defattr(-,root,root)

%{prefix}/include/google
%{prefix}/lib/debug
%{prefix}/lib/libstacktrace.a
%{prefix}/lib/libstacktrace.la
%{prefix}/lib/libstacktrace.so
%{prefix}/lib/libtcmalloc.a
%{prefix}/lib/libtcmalloc.la
%{prefix}/lib/libtcmalloc.so
%{prefix}/lib/libprofiler.a
%{prefix}/lib/libprofiler.la
%{prefix}/lib/libprofiler.so
%{prefix}/lib/libheapprofiler.a
%{prefix}/lib/libheapprofiler.la
%{prefix}/lib/libheapprofiler.so
%{prefix}/lib/libheapchecker.a
%{prefix}/lib/libheapchecker.la
%{prefix}/lib/libheapchecker.so

