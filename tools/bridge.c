#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <math.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

#define CK(x) do { if ((x) < 0) { fprintf(stderr, #x ": %s\n", strerror(errno)); return 1; } } while (0)

static volatile int run = 1;
static void onint(int s) { (void)s; run = 0; }

/* MTISP 10bit = 연속 비트스트림(LE). row 시작은 바이트 정렬. */
static inline unsigned get10(const unsigned char *row, int i)
{
	int bit = i * 10, by = bit >> 3, sh = bit & 7;
	unsigned v = row[by] | (row[by + 1] << 8) | (row[by + 2] << 16);
	return (v >> sh) & 0x3FF;
}

int main(int argc, char **argv)
{
	const char *src = argc > 1 ? argv[1] : "/dev/video5";
	const char *dst = argc > 2 ? argv[2] : "/dev/video10";
	const char *bp  = argc > 3 ? argv[3] : "RGGB";
	int mfd, vfd, ofd, req_fd = -1;
	struct v4l2_format fmt = {0}, ofmt = {0};
	struct v4l2_requestbuffers rb = {0};
	struct v4l2_buffer buf = {0};
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
	void *mem; unsigned len, W, H, OW, OH, stride;
	unsigned char *yuyv;

	signal(SIGINT, onint); signal(SIGTERM, onint);
	CK(mfd = open("/dev/media1", O_RDWR));
	CK(vfd = open(src, O_RDWR));
	CK(ofd = open(dst, O_RDWR));

	W = argc > 4 ? (unsigned)atoi(argv[4]) : 1600;
	H = argc > 5 ? (unsigned)atoi(argv[5]) : 1200;
	const char *fcc = argc > 6 ? argv[6] : "MBRA";
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = W; fmt.fmt.pix_mp.height = H;
	fmt.fmt.pix_mp.pixelformat = v4l2_fourcc(fcc[0], fcc[1], fcc[2], fcc[3]);
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	CK(ioctl(vfd, VIDIOC_S_FMT, &fmt));
	W = fmt.fmt.pix_mp.width; H = fmt.fmt.pix_mp.height;
	stride = W * 10 / 8; OW = W / 2; OH = H / 2;
	printf("src %ux%u -> loopback %ux%u YUYV (%s)\n", W, H, OW, OH, bp);

	ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ofmt.fmt.pix.width = OW; ofmt.fmt.pix.height = OH;
	ofmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	ofmt.fmt.pix.field = V4L2_FIELD_NONE;
	ofmt.fmt.pix.bytesperline = OW * 2;
	ofmt.fmt.pix.sizeimage = OW * OH * 2;
	CK(ioctl(ofd, VIDIOC_S_FMT, &ofmt));
	yuyv = malloc(OW * OH * 2);

	rb.count = 2; rb.type = fmt.type; rb.memory = V4L2_MEMORY_MMAP;
	CK(ioctl(vfd, VIDIOC_REQBUFS, &rb));
	buf.type = rb.type; buf.memory = rb.memory; buf.index = 0;
	buf.m.planes = planes; buf.length = fmt.fmt.pix_mp.num_planes;
	CK(ioctl(vfd, VIDIOC_QUERYBUF, &buf));
	len = planes[0].length;
	mem = mmap(NULL, len, PROT_READ, MAP_SHARED, vfd, planes[0].m.mem_offset);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }

	int type = rb.type;
	CK(ioctl(vfd, VIDIOC_STREAMON, &type));

	/* 베이어 오프셋: 2x2 블록에서 R 위치 */
	int rx = (bp[0]=='R') ? 0 : (bp[1]=='R' ? 1 : 0);
	int ry = (bp[0]=='R'||bp[1]=='R') ? 0 : 1;
	if (!strcmp(bp,"GRBG")) { rx=1; ry=0; }
	else if (!strcmp(bp,"BGGR")) { rx=1; ry=1; }
	else if (!strcmp(bp,"GBRG")) { rx=0; ry=1; }
	else { rx=0; ry=0; }   /* RGGB */

	/* 10bit -> 8bit 감마 LUT, 이득은 매 프레임 갱신 */
	unsigned char lut[1024];
	float gain = 2.0f, wb[3] = {1.f, 1.f, 1.f};
	unsigned long frames = 0;
	while (run) {
		double t0 = now();
		CK(ioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &req_fd));
		memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
		buf.type = rb.type; buf.memory = rb.memory; buf.index = 0;
		buf.m.planes = planes; buf.length = fmt.fmt.pix_mp.num_planes;
		buf.flags = V4L2_BUF_FLAG_REQUEST_FD; buf.request_fd = req_fd;
		if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); close(req_fd); break; }
		if (ioctl(req_fd, MEDIA_REQUEST_IOC_QUEUE, NULL) < 0) { perror("REQ_QUEUE"); close(req_fd); break; }

		struct pollfd pfd = { .fd = vfd, .events = POLLIN };
		if (poll(&pfd, 1, 3000) <= 0) { close(req_fd); continue; }

		memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
		buf.type = rb.type; buf.memory = rb.memory;
		buf.m.planes = planes; buf.length = fmt.fmt.pix_mp.num_planes;
		if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF"); close(req_fd); break; }

		for (int i = 0; i < 1024; i++) {
			float v = i * gain / 1023.0f;
			if (v > 1.f) v = 1.f;
			lut[i] = (unsigned char)(255.0f * powf(v, 1.0f/2.2f));
		}
		double sum[3] = {0,0,0};
		double t1 = now();
		const unsigned char *base = mem;
		for (unsigned y = 0; y < OH; y++) {
			const unsigned char *r0 = base + (size_t)(2*y + ry) * stride;
			const unsigned char *r1 = base + (size_t)(2*y + 1 - ry) * stride;
			unsigned char *o = yuyv + (size_t)y * OW * 2;
			/* 5바이트 = 4픽셀 정렬. 출력 2픽셀(입력 4픽셀)씩 처리 */
			for (unsigned x = 0; x < OW; x += 2) {
				unsigned g = (x >> 1) * 5;
				const unsigned char *a = r0 + g, *b2 = r1 + g;
				unsigned a0 =  a[0]        | ((a[1] & 0x03) << 8);
				unsigned a1 = (a[1] >> 2)  | ((a[2] & 0x0F) << 6);
				unsigned a2 = (a[2] >> 4)  | ((a[3] & 0x3F) << 4);
				unsigned a3 = (a[3] >> 6)  |  (a[4] << 2);
				unsigned b0 =  b2[0]       | ((b2[1] & 0x03) << 8);
				unsigned b1 = (b2[1] >> 2) | ((b2[2] & 0x0F) << 6);
				unsigned b_2= (b2[2] >> 4) | ((b2[3] & 0x3F) << 4);
				unsigned b3 = (b2[3] >> 6) |  (b2[4] << 2);
				unsigned px0[4] = { a0, a1, b0, b1 };
				unsigned px1[4] = { a2, a3, b_2, b3 };
				for (int half = 0; half < 2 && x + half < OW; half++) {
					unsigned *q = half ? px1 : px0;
					unsigned R = q[rx];
					unsigned B = q[2 + (1 - rx)];
					unsigned G = (q[1 - rx] + q[2 + rx]) >> 1;
				sum[0] += R; sum[1] += G; sum[2] += B;
				unsigned Ri = (unsigned)(R * wb[0]), Gi = (unsigned)(G * wb[1]), Bi = (unsigned)(B * wb[2]);
				if (Ri > 1023) Ri = 1023;
				if (Gi > 1023) Gi = 1023;
				if (Bi > 1023) Bi = 1023;
				int Rv = lut[Ri], Gv = lut[Gi], Bv = lut[Bi];
				int Y = (77*Rv + 150*Gv + 29*Bv) >> 8;
				int U = ((-43*Rv - 85*Gv + 128*Bv) >> 8) + 128;
				int V = ((128*Rv - 107*Gv - 21*Bv) >> 8) + 128;
				if (Y<0)Y=0; if (Y>255)Y=255;
				if (U<0)U=0; if (U>255)U=255;
				if (V<0)V=0; if (V>255)V=255;
				unsigned xx = x + half;
				o[2*xx] = Y;
				o[2*xx+1] = (xx & 1) ? V : U;
				}
			}
		}
		{
			double n = (double)OW * OH;
			double mr = sum[0]/n, mg = sum[1]/n, mb = sum[2]/n;
			double avg = (mr + mg + mb) / 3.0;
			if (mr > 1 && mg > 1 && mb > 1) {
				wb[0] = 0.7f*wb[0] + 0.3f*(float)(avg/mr);
				wb[1] = 0.7f*wb[1] + 0.3f*(float)(avg/mg);
				wb[2] = 0.7f*wb[2] + 0.3f*(float)(avg/mb);
			}
			/* 목표: 감마 후 평균이 대략 0.45 -> 선형 0.17 */
			double target = 0.17 * 1023.0 / (avg > 1 ? avg : 1);
			gain = (float)(0.7*gain + 0.3*target);
			if (gain < 0.5f) gain = 0.5f;
			if (gain > 16.f) gain = 16.f;
		}
		if (write(ofd, yuyv, OW*OH*2) < 0) perror("write loopback");
		double t2 = now();
		if (frames < 5 || frames % 30 == 0)
			printf("capture=%.0fms process=%.0fms\n", (t1-t0)*1000, (t2-t1)*1000);
		close(req_fd); req_fd = -1;
		if (++frames % 30 == 0) { printf("frames=%lu\n", frames); fflush(stdout); }
	}
	ioctl(vfd, VIDIOC_STREAMOFF, &type);
	printf("stopped after %lu frames\n", frames);
	return 0;
}
