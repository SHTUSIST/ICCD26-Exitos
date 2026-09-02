/* Completion-side polling as a fourth backend, alongside the existing ones.
 *
 * Polled completion needs poll queues on the controller, which are enabled by
 * a PCI re-probe of that controller alone. The paper's design -- a blocking
 * ioctl that "synchronously waits" -- keeps the interrupt.
 *
 * The ioctl backend is NOT replaced: it needs no poll queues, which a stock
 * kernel does not have (poll_queues defaults to 0), so it stays the portable
 * choice, and a polled backend is selected explicitly through EXITOS_IOPATH. */
#include "tap.h"
#include "exitos_iopath.h"

int main(void)
{
    /* Every backend must be distinct so a caller cannot ask for one and
     * silently get another. */
    T_OK(IOPATH_PWRITE != IOPATH_NVME_IOCTL, "pwrite and ioctl are distinct");
    T_OK(IOPATH_NVME_IOCTL != IOPATH_URING_CMD, "ioctl and uring are distinct");
    T_OK(IOPATH_URING_CMD != IOPATH_URING_CMD_POLL, "uring and polled uring are distinct");
    T_OK(IOPATH_URING_CMD_POLL != IOPATH_URING_WRITE_POLL,
         "NVMe passthrough polling and block WRITE polling are distinct");

    /* Only the polled backend asks the kernel for a polled ring. Getting this
     * wrong either loses the speedup or makes a normal ring refuse to complete. */
    T_EQ(iopath_setup_flags(IOPATH_PWRITE) & IOPATH_SETUP_IOPOLL, 0,
         "pwrite backend does not request a polled ring");
    T_EQ(iopath_setup_flags(IOPATH_NVME_IOCTL) & IOPATH_SETUP_IOPOLL, 0,
         "ioctl backend does not request a polled ring (it uses no ring at all)");
    T_EQ(iopath_setup_flags(IOPATH_URING_CMD) & IOPATH_SETUP_IOPOLL, 0,
         "plain uring backend stays interrupt-driven");
    T_OK(iopath_setup_flags(IOPATH_URING_CMD_POLL) & IOPATH_SETUP_IOPOLL,
         "polled backend requests IORING_SETUP_IOPOLL");
    T_EQ(iopath_setup_flags(IOPATH_URING_WRITE_POLL), IOPATH_SETUP_IOPOLL,
         "block WRITE polling requests exactly IOPOLL with standard entries");

    /* Both uring backends need 128-byte SQEs; NVMe passthru does not fit in 64. */
    T_OK(iopath_setup_flags(IOPATH_URING_CMD) & IOPATH_SETUP_SQE128,
         "uring backend requests 128-byte SQEs");
    T_OK(iopath_setup_flags(IOPATH_URING_CMD_POLL) & IOPATH_SETUP_SQE128,
         "polled backend also requests 128-byte SQEs");
    T_EQ(iopath_setup_flags(IOPATH_URING_WRITE_POLL) & IOPATH_SETUP_SQE128, 0,
         "block WRITE polling keeps standard 64-byte SQEs");

    /* Polling requires the device to have poll queues; report rather than guess. */
    int have = iopath_poll_available("/dev/definitely-not-a-device");
    T_EQ(have, 0, "poll availability of a missing device reports 0, does not crash");
    /* NVMe passthru over io_uring_cmd needs BOTH big entries: the 128-byte SQE
     * carries the 80-byte NVMe command, and the 32-byte CQE carries the NVMe
     * completion result back. Asking for only the SQE half made every command
     * fail with EOPNOTSUPP on a real controller, while the unit test passed
     * because it only ever checked the SQE half. */
    T_OK(iopath_setup_flags(IOPATH_URING_CMD) & IOPATH_SETUP_CQE32,
         "uring_cmd backend also requests 32-byte CQEs, which NVMe passthru needs "
         "to return its completion result");
    T_OK(iopath_setup_flags(IOPATH_URING_CMD_POLL) & IOPATH_SETUP_CQE32,
         "polled uring_cmd backend requests 32-byte CQEs too");
    T_EQ(iopath_setup_flags(IOPATH_URING_WRITE_POLL) & IOPATH_SETUP_CQE32, 0,
         "block WRITE polling keeps standard 16-byte CQEs");

    T_EQ(iopath_poll_available_fd(-1), 0,
         "fd-bound poll probing fails closed for an invalid descriptor");

    /* A backend that iopath_open refuses can never be measured, however right
     * its setup flags are. The polled backend was missing from that function's
     * accept list while these very flag assertions passed. */
    T_OK(iopath_open(NULL, IOPATH_URING_CMD_POLL, 512) == NULL,
         "polled backend still refuses a NULL device");
    {
        struct iopath *h = iopath_open("/dev/null", IOPATH_URING_CMD_POLL, 512);
        T_OK(h == NULL, "polled backend refuses a device that is not an NVMe namespace");
        if (h) iopath_close(h);
    }
    T_OK(iopath_open("/dev/nvme_no_such_device_xyz", IOPATH_URING_CMD_POLL, 512) == NULL,
         "polled backend refuses a device that does not exist");
    T_OK(iopath_open(NULL, IOPATH_URING_WRITE_POLL, 512) == NULL,
         "block WRITE polling refuses a NULL device");
    T_OK(iopath_open("/dev/null", IOPATH_URING_WRITE_POLL, 512) == NULL,
         "block WRITE polling refuses a character device");

    T_DONE();
}
