# Description

In the FreeBSD NFSv4 server implementation, it is possible for the server to
"lose" structs associated with file handles that cannot be removed without a
restart of the NFS server. The structs are not leaked like in a memory leak but
rather lack an association with an open file, lock, or client. As such, these
lost structs will accumulate over time, until the performance of NFSv4
operations become so degraded as to be unusable or the server is rebooted.

Specifically, when the NFSv4 operation `OPEN` fails in specific ways, it can
leave an`nfslockfile` struct in the `nfslockfilehash` table that is not
associated with any open file. Since these lock file structs are not connected
to anything, there is no file to close or lock to release that will result in
removing the struct from the hash table. Even revoking the client that created
them does not work. With successive failed operations, these lost lockfile
structs can accumulate in the hash table.

`nfslockfilehash` is a bucketed hash table, where each bucket is a linked list
of `nfslockfile` structs. The hash table is accessed by the function
`nfsrv_getlockfile()`, which retrieves an `nfslockfile` struct for a given file
handle. When looking up a file handle, the function hashes the handle to
identify the bucket (by default the hash table has 20 buckets)and searches the
linked list of the bucket until the correct struct is found. If no struct
matching the handle is found,`nfsrv_getlockfile()` creates a new one and adds
it to the bucket. This generally happens when a file is opened for the first
time. Conversely, when all references to a file handle are closed (open files,
locks, etc), the corresponding struct is removed from the hash table.

In FreeBSD, the NFSv4 state is protected by various mutexes, particularly a
global mutex called `nfs_state_mutex`, typically referenced with the macro's
`NFSLOCKSTATE()`and `NFSUNLOCKSTATE()`. Most NFSv4 operations will require this
mutex at some point before they complete. In particular, before
`nfsrv_getlockfile()` can be invoked, the state mutex must be held. For
example, when a new file is opened, the state mutex is held by the thread
performing the operation while `nfsrv_getlockfile()` searches the 
`nfslockfilehash` for the file handle being opened.

Therefore, as lost structs accumulate in the hash table, the time to search the
hash table for file handles that are not already present increases, as the
number of entries that must be iterated through increases. With small amounts
of lost structs, this isnt an issue, but with hundreds of thousands of structs,
the time become significant, potentially in the milliseconds depending on the
server. And since the global state mutex is held by a single thread while
search the hash table, it can create major contention for the mutex,
depending on the server load. And since the mutex uses spin waiting, any thread
waiting will waste significant amounts of CPU time.

As such, a client with read and write permission on a single mount on an NFSv4
server can effectively render the server unresponsive to all other clients,
even ones using different mounts. This could happen accidentally or
intentionally. The server will remain degraded even if the client performing
the problematic operation is revoked, as the lost structs are not removed. The
only resolutions are to either restart the NFSv4 server or move all clients off
of NFSv4 and onto an alternative like NFSv3.

# Fix

This issue was reported to FreeBSD maintainers and fixed by Rick Macklem in commit 
[1749465947a807caa53ce09b90a30b820eaab62e](https://lists.freebsd.org/archives/dev-commits-src-all/2025-June/055842.html).

# Observation

An NFSv4 server that is degraded by an accumulation of lost lockfile hashes
will show high CPU usage by kernel threads associated with the `nfsd` process
and NFSv4 operations will see high latency on all operations. For example, when
under load, a degraded NFSv4 server will report that most unhalted CPU cycles
are spent either waiting for a mutex or performing `memcmp()` in
`nfsrv_getlockfile()`.

```
root@freebsd-nfs[~]# pmcstat -TS unhalted-cycles
PMC: [cpu_clk_unhalted.thread] Samples: 143984 (100.0%) , 698 unresolved

%SAMP IMAGE      FUNCTION                       CALLERS
92.6 kernel     lock_delay                     __mtx_lock_sleep
  5.0 kernel     memcmp                         nfsrv_getlockfile
```

In addition, `dtrace` can be used to measure the amount of time spent in
`nfsrv_getlockfile()`. The histogram output should show a large gap between
normal, fast calls to `nfsrv_getlockfile()`, and degraded, slow calls
to`nfsrv_getlockfile()`. In the example below, half of the calls took a few
microseconds, while the other half took around 10 milliseconds.

```
#!/usr/sbin/dtrace -s

fbt::nfsrv_getlockfile:entry
{
        self->ts = timestamp;
}

fbt::nfsrv_getlockfile:return
/self->ts/
{
        @ = quantize(timestamp - self->ts);
        self->ts = 0;
}
```

```
root@freebsd-nfs[~]# dtrace -s trace_getlockfile.d
^C


           value  ------------- Distribution ------------- count
             256 |                                         0
             512 |                                         9
            1024 |@@@                                      51
            2048 |@@@@@@@@@                                182
            4096 |@@@@@@@                                  142
            8192 |                                         0
           16384 |                                         2
           32768 |                                         0
           65536 |                                         0
          131072 |                                         0
          262144 |                                         0
          524288 |                                         0
         1048576 |                                         0
         2097152 |                                         0
         4194304 |                                         0
         8388608 |@                                        11
        16777216 |@@@@@@@@@@@@@@@@@@@@                     390
        33554432 |                                         2
        67108864 |                                         0

```

# nfs-lockfile-counter

The tool `nfs-lockfile-counter` can be used to detect any lost lock files and
identify what mount's they are connected to. `nfs-lockfile-counter` uses libkvm
to find the lockfile hash table and count the number of entries in it that are
lost. It will consider any lockfile that lacks a valid pointer to an open file
or a lock as "lost".

The program can be built with `make nfs-lockfile-counter`. The program must run
with sufficient privileges to use libkvm.

### Example

```commandline
root@freebsd-nfs:~ $ ./nfs-lockfile-counter
Total file handles: 5703097
Lost file handles: 5703090
```

On this NFS server, there are 5703097 `nfslockfile` structs in the
`nfslockfilehash` table. Of those, all but 7 are lost.

# nfs-trigger-lockfile-bug

This program will repeatedly trigger the bug on a FreeBSD 14.2 NFSv4 server.
This will result in lockfile objects accumulating in the NFSv4 state until the
program is stopped. Given enough time, the performance of the server will
significantly degrade until the point that is largely unusable.

The program uses libnfs to interact with the NFS server. libnfs can be
installed with `pkg install libnfs`. This is to ensure that the specific NFS
operations are performed and that there is no client side caching.

The script takes two arguments, `URL` and `FILEPATH`.

- `URL`: A libnfs formatted URL of the NFSv4 share to mount. `?version=4`
  should be added to ensure that NFSv4 is used,
  and not NFSv3.

- `FILEPATH`: The name of a file that the script has permission to create and
  delete.

### Example

```commandline
root@freebsd-nfsclient:~ $ ./nfs-trigger-lockfile-bug nfs://10.0.1.243/?version=4 dir/file
Running. Press CTRL+C to exit
^C
Created 1129 lost lockfile structs
```

# Minimal Reproducable Example
The following steps provide a way to reproduce the issue in a single FreeBSD
14.2 VM acting as both server and client. These steps configure a simple NFSv4
server that allows clients on loopback.

1. Configure an NFS server
   1. Add the following to `/etc/rc.conf`:
      ```text
      nfs_server_enable="YES"
      nfsv4_server_enable="YES"
      nfsuserd_enable="YES"
      ```
   2. Add the following to `/etc/exports`:
      ```text
      V4: /mnt/nfs -network 127.0.0.0 -mask 255.0.0.0
      /mnt/nfs -maproot=root
      ```
   3. `mkdir /mnt/nfs`
   4. `service nfsd start`
2. Build the reproduction tools
   1. `pkg install libnfs`
   2. `make`
3. Run the `nfs-trigger-lockfile-bug` tool for a period of time to create lost lockfiles.
   1. `./nfs-trigger-lockfile-bug nfs://127.0.0.1/?version=4 testfile`
4. Run `./nfs-lockfile-counter` to observe the growth in the number of lost lockfile structs.