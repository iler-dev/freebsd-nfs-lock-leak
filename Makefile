all: nfs-lockfile-counter nfs-trigger-lockfile-bug

nfs-lockfile-counter: nfs-lockfile-counter.c
	$(CC) -o $@ -lkvm $<

nfs-trigger-lockfile-bug: nfs-trigger-lockfile-bug.c
	$(CC) -o $@ -I/usr/local/include -L/usr/local/lib -lnfs $<