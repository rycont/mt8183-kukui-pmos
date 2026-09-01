#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/media.h>

#define MEDIA_IOC_REQUEST_ALLOC _IOR('|', 0x05, int)
#define MEDIA_REQUEST_IOC_QUEUE _IO('|', 0x80)

#define CK(x) do { if ((x) < 0) { fprintf(stderr, #x " failed: %s\n", strerror(errno)); return 1; } } while (0)

int main(int argc, char **argv)
{
	const char *mdev = "/dev/media1", *vdev = "/dev/video5";
	const char *out = argc > 1 ? argv[1] : "/tmp/frame.raw";
	int mfd, vfd, req_fd = -1;
	struct v4l2_format fmt = {0};
	struct v4l2_requestbuffers rb = {0};
	struct v4l2_buffer buf = {0};
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
	void *mem;
	unsigned len;

	CK(mfd = open(mdev, O_RDWR));
	CK(vfd = open(vdev, O_RDWR));

	CK(ioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &req_fd));
	printf("request fd=%d\n", req_fd);

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	CK(ioctl(vfd, VIDIOC_G_FMT, &fmt));
	printf("fmt %ux%u fourcc=%.4s planes=%u size=%u\n",
	       fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	       (char *)&fmt.fmt.pix_mp.pixelformat,
	       fmt.fmt.pix_mp.num_planes, fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	CK(ioctl(vfd, VIDIOC_REQBUFS, &rb));

	buf.type = rb.type; buf.memory = rb.memory; buf.index = 0;
	buf.m.planes = planes; buf.length = fmt.fmt.pix_mp.num_planes;
	CK(ioctl(vfd, VIDIOC_QUERYBUF, &buf));
	len = planes[0].length;
	mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, planes[0].m.mem_offset);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }

	/* queue buffer into the request */
	memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
	buf.type = rb.type; buf.memory = rb.memory; buf.index = 0;
	buf.m.planes = planes; buf.length = fmt.fmt.pix_mp.num_planes;
	buf.flags = V4L2_BUF_FLAG_REQUEST_FD;
	buf.request_fd = req_fd;
	CK(ioctl(vfd, VIDIOC_QBUF, &buf));

	int type = rb.type;
	CK(ioctl(vfd, VIDIOC_STREAMON, &type));
	CK(ioctl(req_fd, MEDIA_REQUEST_IOC_QUEUE, NULL));
	printf("request queued, waiting...\n");

	struct pollfd pfd = { .fd = vfd, .events = POLLIN };
	int pr = poll(&pfd, 1, 10000);
	printf("poll=%d revents=0x%x\n", pr, pfd.revents);
	if (pr <= 0) { fprintf(stderr, "timeout\n"); return 2; }

	memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
	buf.type = rb.type; buf.memory = rb.memory;
	buf.m.planes = planes; buf.length = fmt.fmt.pix_mp.num_planes;
	CK(ioctl(vfd, VIDIOC_DQBUF, &buf));
	printf("DQBUF ok: idx=%u bytesused=%u seq=%u\n",
	       buf.index, planes[0].bytesused, buf.sequence);

	FILE *f = fopen(out, "wb");
	fwrite(mem, 1, planes[0].bytesused ? planes[0].bytesused : len, f);
	fclose(f);
	printf("wrote %s\n", out);

	ioctl(vfd, VIDIOC_STREAMOFF, &type);
	return 0;
}
