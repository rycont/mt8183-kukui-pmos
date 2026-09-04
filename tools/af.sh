#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# 대비검출 자동초점.
#
# libcamera 의 소프트웨어 ISP 에는 AF 알고리즘이 없고 simple 파이프라인
# 핸들러는 렌즈 서브디바이스를 바인딩조차 하지 않는다. 그래서 초점은
# 커널 쪽 V4L2_CID_FOCUS_ABSOLUTE 로 직접 움직인다.
#
# 초점을 성기게 훑어 선명도 피크를 찾고, 그 주변을 다시 촘촘히 훑는다.
#
#   ./af.sh [rear|front]
#
# 환경변수: STEPS(성긴 스윕 횟수), FRAMES(위치당 프레임), W/H(스윕 해상도),
#           KEEP=1(중간 파일 유지)
#
# 두 가지 한계를 알고 쓸 것.
#
# 화면 중앙에 무늬가 있어야 한다. 대비검출이므로 중앙이 민무늬면 잴 것이
# 없어서 곡선이 평평해지고 노이즈를 피크로 고른다.
#
# 피사체가 렌즈의 최단 초점거리보다 가까우면 어느 위치도 맞지 않는다.
# 이때 선명도는 정점 없이 최대값까지 단조 증가한다 — 결과가 FMAX 에
# 붙어 있으면 뒤로 물러서야 한다는 뜻이다.

set -e

case "${1:-rear}" in
rear)	CAM_ID=/base/soc/i2c@11009000/camera@10 ;;
front)	CAM_ID=/base/soc/i2c@11008000/camera@3d ;;
*)	CAM_ID="$1" ;;
esac

STEPS=${STEPS:-10}
FRAMES=${FRAMES:-20}
W=${W:-1632}
H=${H:-1224}

SHARP=$(command -v sharpness || echo ./sharpness)
[ -x "$SHARP" ] || { echo "sharpness 바이너리가 없다 (tools/sharpness.c)" >&2; exit 1; }

TMP=$(mktemp -d)
[ -n "$KEEP" ] || trap 'rm -rf "$TMP"' EXIT

# focus_absolute 를 가진 서브디바이스를 찾는다. 노드 번호는 부팅마다 바뀐다.
LENS=
for s in /dev/v4l-subdev*; do
	if v4l2-ctl -d "$s" -L 2>/dev/null | grep -q focus_absolute; then
		LENS=$s
		break
	fi
done
[ -n "$LENS" ] || {
	echo "focus_absolute 를 노출하는 서브디바이스가 없다." >&2
	echo "dw9768 이 DT 에 있고 CONFIG_VIDEO_DW9768 이 켜져 있는지 확인할 것." >&2
	exit 1
}

RANGE=$(v4l2-ctl -d "$LENS" -L | awk '/focus_absolute/ {
	for (i = 1; i <= NF; i++) {
		if ($i ~ /^min=/) m = substr($i, 5)
		if ($i ~ /^max=/) M = substr($i, 5)
	}
	print m, M
}')
FMIN=${RANGE% *}
FMAX=${RANGE#* }
echo "렌즈 $LENS  초점범위 $FMIN..$FMAX"

# 한 위치에서 선명도를 잰다.
#
# 프레임을 여러 장 받는 이유는 AE 수렴 때문이다. cam 은 매 호출마다
# 파이프라인을 재설정하므로 앞쪽 프레임은 노출이 엉망이고, 소프트웨어
# AGC 는 수렴에 20 프레임 가까이 쓴다. 6 장만 받아 보면 위치별 차이보다
# AE 잔여 변동이 커서 엉뚱한 위치를 고른다. 마지막 프레임만 쓴다.
measure() {
	pos=$1
	v4l2-ctl -d "$LENS" -c focus_absolute="$pos" >/dev/null 2>&1
	rm -f "$TMP"/f_*.bin
	timeout 60 cam -c "$CAM_ID" --capture="$FRAMES" \
		--stream "width=$W,height=$H" \
		--file="$TMP/f_#.bin" >/dev/null 2>&1 || return 1

	last=$(ls "$TMP"/f_*.bin 2>/dev/null | tail -1)
	[ -n "$last" ] || return 1

	# libcamera 는 행을 패딩한다. 실제 stride 를 파일 크기에서 되짚는다.
	bytes=$(stat -c %s "$last")
	stride=$((bytes / H / 4))
	"$SHARP" "$last" "$stride" "$stride" "$H"
}

best_pos=$FMIN
best_val=-1

sweep() {
	lo=$1; hi=$2; n=$3
	step=$(( (hi - lo) / n ))
	[ "$step" -gt 0 ] || step=1
	pos=$lo
	while [ "$pos" -le "$hi" ]; do
		val=$(measure "$pos") || { echo "  $pos  캡처 실패"; pos=$((pos + step)); continue; }
		printf '  %4d  %s\n' "$pos" "$val"
		# busybox awk 로 실수 비교
		if [ "$(awk -v a="$val" -v b="$best_val" 'BEGIN{print (a>b)?1:0}')" = 1 ]; then
			best_val=$val
			best_pos=$pos
		fi
		pos=$((pos + step))
	done
}

echo "성긴 스윕 ($STEPS 단계)"
sweep "$FMIN" "$FMAX" "$STEPS"

coarse_step=$(( (FMAX - FMIN) / STEPS ))
lo=$((best_pos - coarse_step)); [ "$lo" -lt "$FMIN" ] && lo=$FMIN
hi=$((best_pos + coarse_step)); [ "$hi" -gt "$FMAX" ] && hi=$FMAX

echo "정밀 스윕 ($lo..$hi)"
sweep "$lo" "$hi" 6

echo "최적 초점 $best_pos (선명도 $best_val)"
v4l2-ctl -d "$LENS" -c focus_absolute="$best_pos"
