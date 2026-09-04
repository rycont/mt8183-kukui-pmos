// SPDX-License-Identifier: GPL-2.0
/*
 * libcamera 가 뱉은 ABGR8888 프레임의 선명도를 출력한다.
 * 대비검출 AF 에서 초점 위치를 고르는 데 쓴다.
 *
 * 지표는 수평 방향 2차 차분(라플라시안)의 절대값 합이다. 초점이 맞으면
 * 에지가 가팔라져서 값이 커진다. 밝기에 비례해 커지므로 평균 휘도로
 * 나눠 정규화한다 — AF 스윕 도중 AE 가 움직여도 비교가 되도록.
 *
 *   sharpness <파일> <stride(px)> <width> <height>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s <file> <stride_px> <width> <height>\n",
			argv[0]);
		return 2;
	}

	const char *path = argv[1];
	long stride = atol(argv[2]);
	long w = atol(argv[3]);
	long h = atol(argv[4]);

	if (stride < w || w < 3 || h < 1) {
		fprintf(stderr, "bad geometry\n");
		return 2;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}

	uint8_t *row = malloc(stride * 4);
	if (!row) {
		fclose(f);
		return 1;
	}

	/* 중앙 40% 만 본다. 주변부는 비네팅과 초점면 곡률로 흔들린다. */
	long x0 = w * 3 / 10, x1 = w * 7 / 10;
	long y0 = h * 3 / 10, y1 = h * 7 / 10;

	double accum = 0.0, luma = 0.0;
	long n = 0;

	for (long y = 0; y < h; y++) {
		if (fread(row, 4, stride, f) != (size_t)stride)
			break;
		if (y < y0 || y >= y1)
			continue;

		for (long x = x0 + 1; x < x1 - 1; x++) {
			/* ABGR8888 은 메모리상 R,G,B,A 순. G 만 본다. */
			int gl = row[(x - 1) * 4 + 1];
			int gc = row[x * 4 + 1];
			int gr = row[(x + 1) * 4 + 1];
			int lap = gl - 2 * gc + gr;

			accum += lap < 0 ? -lap : lap;
			luma += gc;
			n++;
		}
	}

	free(row);
	fclose(f);

	if (!n) {
		fprintf(stderr, "no pixels read\n");
		return 1;
	}

	double mean = luma / n;
	/* 0 나눗셈과 새까만 프레임에서의 폭주를 막는다. */
	printf("%.4f\n", mean < 1.0 ? 0.0 : (accum / n) / mean);
	return 0;
}
