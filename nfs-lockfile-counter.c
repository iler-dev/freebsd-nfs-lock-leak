#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>

#include <kvm.h>

#define SYMBOL_LOCKHASH "_nfslockhash"
#define SYMBOL_LOCKHASH_SIZE "_nfsrv_lockhashsize"

// The nfslockfile struct is copied here as it is too messy to try and import it
// from <nfs/nfsrvstate.h>. The struct has been modified to convert all pointers
// to unsigned long, as they are all kernel addresses.
struct nfslockfile {
    struct { unsigned long lh_first; } lf_open;    /* Open list */
    struct { unsigned long lh_first; } lf_deleg;    /* Delegation list */
    struct { unsigned long lh_first; } lf_lock;    /* Lock list */
    struct { unsigned long lh_first; } lf_locallock;    /* Local lock list */
    struct { unsigned long lh_first; } lf_rollback;    /* Local lock rollback list */
    struct { unsigned long le_next; unsigned long le_prev; } lf_hash;    /* Hash list entry */
    fhandle_t lf_fh;        /* The file handle */
    struct nfsv4lock {
        u_int32_t nfslock_usecnt;
        u_int8_t nfslock_lock;
    } lf_locallock_lck; /* serialize local locking */
    int lf_usecount;    /* Ref count for locking */
};

// This program uses libkvm to read kernel memory and examine the nfslockhash
// table. "_nfslockhash" points to the array containing the buckets of the hash
// table. "_nfsrv_lockhashsize" points to the integer representing the number of
// buckets. The table can be iterated through and the number of entries counted.
// A lockfile is considered lost if it lacks a pointer to a session or a lock,
// which indicates the lock file is not associated with any currently opened
// files or locks.

int main(int argv, char *argc[]) {
    kvm_t *kd;

    int rc;
    char errbuf[_POSIX2_LINE_MAX];

    struct nlist symbols[3] = {
            {.n_name = SYMBOL_LOCKHASH},
            {.n_name = SYMBOL_LOCKHASH_SIZE},
            {.n_name = NULL},
    };

    int lockfilehashsize;
    unsigned long lockfilehashtable;

    kd = kvm_openfiles(NULL, NULL, NULL, O_RDONLY, &errbuf[0]);
    if (!kd) {
        fprintf(stderr, "Failed to open files for KVM: %s", kvm_geterr(kd));
        return 1;
    }

    rc = kvm_nlist(kd, symbols);
    if (rc < 0) {
        fprintf(stderr, "Failed to read symbols: %s", kvm_geterr(kd));
        return 1;
    }

    rc = kvm_read(kd, symbols[1].n_value, &lockfilehashsize, sizeof lockfilehashsize);
    if (rc < 0) {
        fprintf(stderr, "Failed to read lockfilehash size: %s", kvm_geterr(kd));
        return 1;
    }

    rc = kvm_read(kd, symbols[0].n_value, &lockfilehashtable, sizeof lockfilehashtable);
    if (rc < 0) {
        fprintf(stderr, "Failed to read lockfilehash pointer: %s", kvm_geterr(kd));
        return 1;
    }

    int total_lockfiles = 0;
    int leaked_lockfiles = 0;
    for (int bucket = 0; bucket < lockfilehashsize; bucket++) {
        unsigned long cur;

        rc = kvm_read(kd, lockfilehashtable + (bucket * sizeof(void *)), &cur, sizeof cur);
        if (rc < 0) {
            fprintf(stderr, "Failed to read bucket pointer: %s", kvm_geterr(kd));
            return 1;
        }

        while (cur) {
            struct nfslockfile lockfile;

            rc = kvm_read(kd, cur, &lockfile, sizeof lockfile);
            if (rc < 0) {
                fprintf(stderr, "Failed to read lockfile: %s", kvm_geterr(kd));
                return 1;
            }

            total_lockfiles++;

            // A file handle is considered lost if it has no reference to an open file or lock.
            if (!lockfile.lf_open.lh_first && !lockfile.lf_lock.lh_first) {
                leaked_lockfiles++;
            }

            cur = lockfile.lf_hash.le_next;
        }
    }

    printf("Total file handles: %d\n", total_lockfiles);
    printf("Lost file handles: %d\n", leaked_lockfiles);
}
