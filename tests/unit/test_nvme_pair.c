#include "tap.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "exitos_devguard.h"

int main(void)
{
    char path[] = "/tmp/exitos-not-nvme-XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);

    T_EQ(exitos_devguard_nvme_pair(NULL, "/dev/null"), 0,
         "NULL block path is never a verified NVMe pair");
    T_EQ(exitos_devguard_nvme_pair("/dev/null", NULL), 0,
         "NULL character path is never a verified NVMe pair");
    T_EQ(exitos_devguard_nvme_pair(path, "/dev/null"), 0,
         "regular file plus character device cannot impersonate an NVMe namespace");
    T_EQ(exitos_devguard_nvme_pair("/dev/exitos-missing", "/dev/null"), 0,
         "missing block namespace is refused");
    T_EQ(exitos_devguard_nvme_pair("/dev/null", "/dev/null"), 0,
         "matching path strings are not proof of an NVMe namespace pair");

    unlink(path);
    T_DONE();
}
