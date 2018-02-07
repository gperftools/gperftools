# `pprof` and Remote Servers

In mid-2006, we added an experimental facility to [pprof](cpu_profiler.md), the tool that analyzes CPU and heap profiles. This facility allows you to collect profile information from running applications. It makes it easy to collect profile information without having to stop the program first, and without having to log into the machine where the application is running. This is meant to be used on webservers, but will work on any application that can be modified to accept TCP connections on a port of its choosing, and to respond to HTTP requests on that port.

We do not currently have infrastructure, such as apache modules, that you can pop into a webserver or other application to get the necessary functionality "for free." However, it's easy to generate the necessary data, which should allow the interested developer to add the necessary support into his or her applications.

To use `pprof` in this experimental "server" mode, you give the script a host and port it should query, replacing the normal commandline arguments of application + profile file:

<pre>   % pprof internalweb.mycompany.com:80
</pre>

The host must be listening on that port, and be able to accept HTTP/1.0 requests -- sent via `wget` and `curl` -- for several urls. The following sections list the urls that `pprof` can send, and the responses it expects in return.

Here are examples that pprof will recognize, when you give them on the commandline, are urls. In general, you specify the host and a port (the port-number is required), and put the service-name at the end of the url.:

```
    http://myhost:80/pprof/heap            # retrieves a heap profile
    http://myhost:8008/pprof/profile       # retrieves a CPU profile
    http://myhost:80                       # retrieves a CPU profile (the default)
    http://myhost:8080/                    # retrieves a CPU profile (the default)
    myhost:8088/pprof/growth               # "http://" is optional, but port is not
    http://myhost:80/myservice/pprof/heap  # /pprof/heap just has to come at the end
    http://myhost:80/pprof/pmuprofile      # CPU profile using performance counters
```

## `**/pprof/heap**`

`pprof` asks for the url `/pprof/heap` to get heap information. The actual url is controlled via the variable `HEAP_PAGE` in the `pprof` script, so you can change it if you'd like.

There are two ways to get this data. The first is to call

<pre>    MallocExtension::instance()->GetHeapSample(&output);
</pre>

and have the server send `output` back as an HTTP response to `pprof`. `MallocExtension` is defined in the header file `gperftools/malloc_extension.h`.

Note this will only only work if the binary is being run with sampling turned on (which is not the default). To do this, set the environment variable `TCMALLOC_SAMPLE_PARAMETER` to a positive value, such as 524288, before running.

The other way is to call `HeapProfileStart(filename)` (from `heap-profiler.h`), continue to do work, and then, some number of seconds later, call `GetHeapProfile()` (followed by `HeapProfilerStop()`). The server can send the output of `GetHeapProfile` back as the HTTP response to pprof. (Note you must `free()` this data after using it.) This is similar to how profiles are handled, below. This technique does not require the application to run with sampling turned on.

Here's an example of what the output should look like:

```
heap profile:   1923: 127923432 [  1923: 127923432] @ heap_v2/524288
     1:      312 [     1:      312] @ 0x2aaaabaf5ccc 0x2aaaaba4cd2c 0x2aaaac08c09a
   928: 122586016 [   928: 122586016] @ 0x2aaaabaf682c 0x400680 0x400bdd 0x2aaaab1c368a 0x2aaaab1c8f77 0x2aaaab1c0396 0x2aaaab1c86ed 0x4007ff 0x2aaaaca62afa
     1:       16 [     1:       16] @ 0x2aaaabaf5ccc 0x2aaaabb04bac 0x2aaaabc1b262 0x2aaaabc21496 0x2aaaabc214bb
[...]
```

Older code may produce "version 1" heap profiles which look like this:

```
heap profile:  14933: 791700132 [ 14933: 791700132] @ heap
     1:   848688 [     1:   848688] @ 0xa4b142 0x7f5bfc 0x87065e 0x4056e9 0x4125f8 0x42b4f1 0x45b1ba 0x463248 0x460871 0x45cb7c 0x5f1744 0x607cee 0x5f4a5e 0x40080f 0x2aaaabad7afa
     1:  1048576 [     1:  1048576] @ 0xa4a9b2 0x7fd025 0x4ca6d8 0x4ca814 0x4caa88 0x2aaaab104cf0 0x404e20 0x4125f8 0x42b4f1 0x45b1ba 0x463248 0x460871 0x45cb7c 0x5f1744 0x607cee 0x5f4a5e 0x40080f 0x2aaaabad7afa
  2942: 388629374 [  2942: 388629374] @ 0xa4b142 0x4006a0 0x400bed 0x5f0cfa 0x5f1744 0x607cee 0x5f4a5e 0x40080f 0x2aaaabad7afa
[...]
```

pprof accepts both old and new heap profiles and automatically detects which one you are using.

## `**/pprof/growth**`

`pprof` asks for the url `/pprof/growth` to get heap-profiling delta (growth) information. The actual url is controlled via the variable `GROWTH_PAGE` in the `pprof` script, so you can change it if you'd like.

The server should respond by calling

<pre>    MallocExtension::instance()->GetHeapGrowthStacks(&output);
</pre>

and sending `output` back as an HTTP response to `pprof`. `MallocExtension` is defined in the header file `gperftools/malloc_extension.h`.

Here's an example, from an actual Google webserver, of what the output should look like:

```
heap profile:    741: 812122112 [   741: 812122112] @ growth
     1:  1572864 [     1:  1572864] @ 0x87da564 0x87db8a3 0x84787a4 0x846e851 0x836d12f 0x834cd1c 0x8349ba5 0x10a3177 0x8349961
     1:  1048576 [     1:  1048576] @ 0x87d92e8 0x87d9213 0x87d9178 0x87d94d3 0x87da9da 0x8a364ff 0x8a437e7 0x8ab7d23 0x8ab7da9 0x8ac7454 0x8348465 0x10a3161 0x8349961
[...]
```

## `**/pprof/profile**`

`pprof` asks for the url `/pprof/profile?seconds=XX` to get cpu-profiling information. The actual url is controlled via the variable `PROFILE_PAGE` in the `pprof` script, so you can change it if you'd like.

The server should respond by calling `ProfilerStart(filename)`, continuing to do its work, and then, XX seconds later, calling `ProfilerStop()`. (These functions are declared in `gperftools/profiler.h`.) The application is responsible for picking a unique filename for `ProfilerStart()`. After calling `ProfilerStop()`, the server should read the contents of `filename` and send them back as an HTTP response to `pprof`.

Obviously, to get useful profile information the application must continue to run in the XX seconds that the profiler is running. Thus, the profile start-stop calls should be done in a separate thread, or be otherwise non-blocking.

The profiler output file is binary, but near the end of it, it should have lines of text somewhat like this:

<pre>01016000-01017000 rw-p 00015000 03:01 59314      /lib/ld-2.2.2.so
</pre>

## `**/pprof/pmuprofile**`

`pprof` asks for a url of the form `/pprof/pmuprofile?event=hw_event:unit_mask&period=nnn&seconds=xxx` to get cpu-profiling information. The actual url is controlled via the variable `PMUPROFILE_PAGE` in the `pprof` script, so you can change it if you'd like.

This is similar to pprof, but is meant to be used with your CPU's hardware performance counters. The server could be implemented on top of a library such as [`libpfm`](http://perfmon2.sourceforge.net/). It should collect a sample every nnn occurrences of the event and stop the sampling after xxx seconds. Much of the code for `/pprof/profile` can be reused for this purpose.

The server side routines (the equivalent of ProfilerStart/ProfilerStart) are not available as part of perftools, so this URL is unlikely to be that useful.

## `**/pprof/contention**`

This is intended to be able to profile (thread) lock contention in addition to CPU and memory use. It's not yet usable.

## `**/pprof/cmdline**`

`pprof` asks for the url `/pprof/cmdline` to figure out what application it's profiling. The actual url is controlled via the variable `PROGRAM_NAME_PAGE` in the `pprof` script, so you can change it if you'd like.

The server should respond by reading the contents of `/proc/self/cmdline`, converting all internal NUL (\0) characters to newlines, and sending the result back as an HTTP response to `pprof`.

Here's an example return value:

<pre>/root/server/custom_webserver
80
--configfile=/root/server/ws.config
</pre>

## `**/pprof/symbol**`

`pprof` asks for the url `/pprof/symbol` to map from hex addresses to variable names. The actual url is controlled via the variable `SYMBOL_PAGE` in the `pprof` script, so you can change it if you'd like.

When the server receives a GET request for `/pprof/symbol`, it should return a line formatted like so:

<pre>   num_symbols: ###
</pre>

where `###` is the number of symbols found in the binary. (For now, the only important distinction is whether the value is 0, which it is for executables that lack debug information, or not-0).

This is perhaps the hardest request to write code for, because in addition to the GET request for this url, the server must accept POST requests. This means that after the HTTP headers, pprof will pass in a list of hex addresses connected by `+`, like so:

<pre>   curl -d '0x0824d061+0x0824d1cf' http://remote_host:80/pprof/symbol
</pre>

The server should read the POST data, which will be in one line, and for each hex value, should write one line of output to the output stream, like so:

<pre><hex address><tab><function name>
</pre>

For instance:

<pre>0x08b2dabd    _Update
</pre>

The other reason this is the most difficult request to implement, is that the application will have to figure out for itself how to map from address to function name. One possibility is to run `nm -C -n <program name>` to get the mappings at program-compile-time. Another, at least on Linux, is to call out to addr2line for every `pprof/symbol` call, for instance `addr2line -Cfse /proc/<getpid>/exe 0x12345678 0x876543210</getpid>` (presumably with some caching!)

`pprof` itself does just this for local profiles (not ones that talk to remote servers); look at the subroutine `GetProcedureBoundaries`.

----

<address>
Last modified: Feb 2018
</address>

[Link to main documentation readme](readme.md)