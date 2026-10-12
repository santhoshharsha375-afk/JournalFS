/*
 * fuse_ops.h -- FUSE 3 operations interface.
 */
#ifndef JFS_FUSE_OPS_H
#define JFS_FUSE_OPS_H

#include "filesystem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mount the image (argv[1]) through FUSE using the remaining arguments as
 * FUSE options.  Returns the fuse_main exit status.
 */
int fuse_main_entry(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* JFS_FUSE_OPS_H */
