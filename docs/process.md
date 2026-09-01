# 과정

막힌 지점과 그걸 어떻게 판별했는지의 기록. 결과보다 이쪽이 재현에 쓸모 있다.

## 디스플레이 (IT6505)

### 출발점이 틀렸다

처음엔 "Duet 은 USB-C 영상 출력을 지원하지 않는다"고 판단했다. 근거는
기억이었고, 확인하지 않았다. 사용자가 "ChromeOS 에서는 됐다"고 지적해서
다시 봤다.

기기 펌웨어가 답을 갖고 있었다:

```
$ sudo cat /sys/kernel/debug/gpio
 gpio-9   (IT6505_HPD_L)
 gpio-152 (IT6505_INT_ODL)
 gpio-13~28 (DPI_D0 ... DPI_CK)
```

coreboot 이 붙인 이름이라 반박의 여지가 없다. 전용 DP 송신 칩이 박혀 있었다.

**교훈**: 하드웨어 유무는 기억이 아니라 `/sys/kernel/debug/gpio` 와
벤더 DT 로 확인한다.

### 원인

메인라인 `mt8183-kukui.dtsi` 에 `dpi0` 가 disabled 이고 `ite,it6505`
노드가 없다. 드라이버(`CONFIG_DRM_ITE_IT6505=y`)와 전원 레귤레이터는
이미 있었다. **연결만 없었다.**

커널 재빌드 없이 DTB 만 고쳐서 해결됐다.

### 리셋 극성

`reset-gpios` 를 mainline mt8186-corsola 를 따라 `GPIO_ACTIVE_LOW` 로 했다가
칩이 응답하지 않았다. ChromeOS 의 kukui DT 는 `GPIO_ACTIVE_HIGH` 였다.

**교훈**: 드라이버가 같아도 보드별 배선은 보드 파일이 이긴다.

## 카메라

### 왜 가능했나

메인라인에 MediaTek 카메라 드라이버는 없다. 2020년 RFC v7 에서 멈췄다.
하지만 **ChromeOS 는 6.6 까지 포워드포트해뒀다**:

| 브랜치 | isp_50 |
|---|---|
| chromeos-5.10 | ✓ |
| chromeos-6.1 | ✗ (isp_7x 만) |
| **chromeos-6.6** | **✓** |
| chromeos-6.12 | ✗ |

5.10 이 아니라 6.6 을 출발점으로 잡으면 격차가 5년에서 6개 릴리스로 준다.
이게 이 작업이 하루에 끝난 가장 큰 이유다.

### 실제 포팅량

| 파일 | 원본 | 수정 |
|---|---|---|
| `mtk_seninf.c` | 1,026줄 | **4줄** |
| `mtk_cam.c` + `mtk_cam-hw.c` | 2,900줄 | 5군데 |

대부분은 API 이름 변경(`v4l2_subdev_get_try_format` → `v4l2_subdev_state_get_format`,
`.remove` 반환형 `int` → `void`)이었다.

### 시간을 먹은 것들

포팅 자체가 아니라 아래가 시간을 먹었다. 순서대로:

1. **CFI 크래시** — GCC 로 빌드해서 커널 사망. `LLVM=1` 로 해결
2. **클럭 13MHz** — `CLK_SET_RATE_PARENT` 때문에 `clk_set_rate` 가 무력
3. **센서 무응답** — `CMMCLK0` 핀mux 누락
4. **pad flags 순서** — 최근 커널은 `media_entity_pads_init` 에서 방향 플래그 검증
5. **`sd_fmt.pad` 미초기화** — 스택 쓰레기값이 EINVAL
6. **프레임 미도착** — 이미지 버퍼가 Request API 로만 큐잉됨

6번은 드라이버 주석이 예고하고 있었다:

```c
/* TODO(b/140397121): Remove non-request mode support when the HAL
 * is fixed to use the Request API only. */
```

비요청 경로는 메타 버퍼만 처리하고 이미지는 버린다. `tools/cap.c` 가
Request API 로 직접 큐잉하는 최소 구현이다.

### 화질 오진

프레임이 나온 뒤 "노이즈가 심하다"고 몇 시간을 오해했다. 실제로는
언패킹 버그였다 (`docs/findings.md` 1번). 원본 픽셀을 가공 없이 보면
순수 노이즈인데 평균을 내면 형체가 보이는 게 결정적 단서였다.

**교훈**: 노이즈처럼 보이면 먼저 **인접 픽셀 상관**을 재본다. 진짜 노이즈는
저주파에도 구조가 없다.

## Snapshot (libcamera)

libcamera 가 단계마다 정확히 한 겹씩 짚어줬다. 각 에러가 다음 할 일이었다:

| 에러 | 원인 | 해결 |
|---|---|---|
| (매칭 자체 안 됨) | 미디어 디바이스 이름이 지원 목록에 없음 | `mdev->driver_name = "mtk-seninf"` |
| `Unable to set format on pad 11/0` | ISP 서브디바이스에 pad ops 부재 | `get_fmt`/`set_fmt`/`init_state` 추가 |
| `Media bus code filtering not supported` | `V4L2_CAP_IO_MC` 없음 | 플래그 + `enum_fmt` 의 `mbus_code` 구현 |
| `No valid configuration found` | MTISP fourcc 를 libcamera 가 모름 | 표준 `SRGGB8` 등 함께 노출 |
| `dma-heap allocation failure` | CMA 16MB, 버퍼 하나가 7.7MB | 커널 cmdline `cma=128M` |
| `Capture N frames` 에서 정지 | 비요청 스트리밍 미지원 | 합성 요청 구현 |

### 합성 요청에서 커널을 두 번 죽였다

**1차 — 리스트 이중 삭제.** `job_done` 안에서 `list_del` + `kfree` 했는데
호출자가 직후에 같은 요청을 다시 `list_del` 한다. 해제된 메모리를 리스트에서
빼면서 링크가 깨지고, 그 리스트를 건드리는 모든 프로세스가 D 상태로 묶였다
(load average 가 계속 오르고 `cam` 이 좀비로 쌓이는 증상).

수정: 소유권을 호출자 한 곳으로 통일.

**2차 — NULL `container_of`.** 컴포저 워커가 디바이스를 이렇게 역추적한다:

```c
struct mtk_cam_dev *cam =
    container_of(req->req.mdev, struct mtk_cam_dev, media_dev);
```

합성 요청은 `kzalloc` 이라 `req.mdev` 가 NULL 이고, `container_of` 가
NULL 에서 오프셋을 빼서 엉뚱한 주소를 만든다.

수정: 합성 요청에 디바이스 포인터를 직접 들려보냄.

**교훈**: 기존 자료구조를 빌려 쓸 때는 그 구조의 **모든 참조 지점**을
먼저 찾는다 (`grep -n 'req->req\.'`). 필드 하나만 보고 넘어가면 안 된다.

## 결과

```
Available cameras:
1: 'ov02a10' (전면)
2: 'ov8856'  (후면)

30.08 fps, ABGR8888 1596x1200
```
