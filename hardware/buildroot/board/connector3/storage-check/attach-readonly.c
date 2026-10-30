// SPDX-License-Identifier: GPL-2.0-only
/* Qualification only: refuse attaching the original rootfs unless MTD is RO. */
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <mtd/ubi-user.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    struct mtd_info_user info;
    int fd = open("/dev/mtd5", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open mtd5"); return 1; }
    int ret = ioctl(fd, MEMGETINFO, &info);
    close(fd);
    if (ret < 0) { perror("MEMGETINFO"); return 1; }
    if ((info.flags & MTD_WRITEABLE) || info.type != MTD_NANDFLASH ||
        info.size != 380633088 || info.writesize != 4096 ||
        info.erasesize != 262144 || info.oobsize != 256) {
        fputs("Refusing: expected original rootfs NAND geometry and read-only MTD\n", stderr);
        return 1;
    }
    fd = open("/dev/ubi_ctrl", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open ubi_ctrl"); return 1; }
    struct ubi_attach_req request = { .ubi_num = UBI_DEV_NUM_AUTO,
        .mtd_num = 5, .vid_hdr_offset = 4096 };
    ret = ioctl(fd, UBI_IOCATT, &request);
    close(fd);
    if (ret < 0) { perror("UBI_IOCATT"); return 1; }
    printf("Attached read-only mtd5 as ubi%d\n", request.ubi_num);
    return 0;
}
