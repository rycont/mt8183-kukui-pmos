# Mali kbase — ARM 공식 드라이버로 G72 구동

메인라인이 쓰는 Panfrost 대신 **ARM 공식 커널 드라이버(kbase)** 를 올려 Mali-G72 를
구동한 기록. 역분석 드라이버가 아닌 벤더 스택을 쓰는 것 자체가 목적이다.

대상: postmarketOS v26.06, 커널 6.18.28 (`linux-postmarketos-mediatek-mt81`)

## 결과

```
mali 13040000.gpu: Kernel DDK version r44p1-01eac0
mali 13040000.gpu: GPU identified as 0x1 arch 6.2.2 r0p3 status 0
mali 13040000.gpu: Probed as mali0

$ cat /sys/class/misc/mali0/device/gpuinfo
Mali-G72 3 cores r0p3 0x6221
```

devfreq 300~800MHz, vgpu/vsram_gpu 정상 인식.

## 왜 ChromeOS 트리인가

kbase 는 GPL 이라 소스가 공개되지만, **SoC 별 전원·클럭 코드는 벤더가 쓴다.**
`platform/mediatek/mt8183_mali_kbase_runtime_pm.c` 를 가진 공개 트리는
ChromeOS 뿐이다. LibreELEC(Amlogic), Rockchip 트리엔 MediaTek 코드가 없다.

`chromeos-6.12` 가 kbase 를 담은 마지막 브랜치다(6.14 이후로는 빠졌다).
릴리스는 r44p1, uAPI major 는 11 이다.

## 구성

```
dt/       DT 패치 — kbase 가 요구하는 형태로 GPU 노드 수정
patches/  kbase 6.12 → 6.18 포팅 diff (17개 파일)
tools/    소스 가져오기 · 빌드 · musl 심
```

## 빌드

```sh
./tools/fetch-kbase.sh kbase-src
patch -p1 -d kbase-src < patches/kbase-6.12-to-6.18.diff

# 커널에 DEVFREQ_THERMAL 이 필요하다
./tools/build-kbase.sh <kernel-tree> kbase-src
```

DT 패치를 커널에 적용해 DTB 를 다시 만들고, `/boot/dtbs/mediatek/` 에 넣은 뒤
`mkinitfs` 로 kpart 를 재생성해야 반영된다. `/boot` 직속 파일만 바꾸면
boot-deploy 가 `dtbs/` 에서 다시 만들어 덮어쓴다.

## 6.12 → 6.18 에서 고친 것

| 항목 | 커널 변경 |
|---|---|
| `$(src)` 절대경로 처리 | 6.13 에서 외부 모듈의 `$(src)` 가 절대경로가 됨 |
| `hrtimer_init` → `hrtimer_setup` | 6.15 통합, `function` 이 `__private` 가 되어 별도 대입 불가 |
| `del_timer` → `timer_delete` | 6.15 이름 변경 |
| `__SetPageMovable` 제거 | 6.13 에서 페이지별 태그가 타입별 `set_movable_ops` 로 바뀜 |
| `fence_value_str` 필드 제거 | `dma_fence_ops` 에서 사라짐 |
| `dev_pm_opp_get_voltage_supply` | ChromeOS 전용 OPP API, 메인라인에 없음 (SVS 경로 차단) |

타이머·페이지 마이그레이션은 `include/version_compat_defs.h` 에 컴팩트 계층을
두는 ARM 의 기존 방식을 따랐다.

## DT 에서 고친 것

Panfrost 와 kbase 는 같은 GPU 를 다르게 기술한다. 넷 다 필요했다.

| 수정 | 이유 |
|---|---|
| `interrupt-names` 를 `JOB/MMU/GPU` 로 | kbase 는 대문자로 조회한다. Panfrost 는 이름 조회에 실패해도 인덱스로 폴백하므로 그대로 동작한다 |
| `sram-supply` + `supply-names` 추가 | kbase 는 코어(vgpu)와 SRAM(vsram_gpu) 두 레귤레이터를 요구한다 |
| OPP 16개에 두 번째 전압 추가 | 공급 수만큼 `opp-microvolt` 항목이 있어야 한다. `vsram = clamp(vgpu + 100000, 850000, 925000)` |
| **레귤레이터 커플링 제거** | 가장 걸리기 쉬운 지점 |

마지막 것이 핵심이다. 보드 DT 는 vgpu 와 vsram_gpu 를
`regulator-coupled-with` 로 묶어두는데, 커플링된 레귤레이터는 커널 커플러가
관리하므로 개별 `regulator_set_voltage()` 가 `-EPERM` 으로 거부된다.
Panfrost 는 OPP 코어를 통해 두 전압을 함께 설정하니 잘 맞지만, kbase 는
`mtk_set_voltages()` 로 bias 를 직접 단계 제어한다. 둘의 역할이 겹쳐 충돌한다.

## 유저스페이스

kbase 는 커널 쪽일 뿐이고, GLES/Vulkan 은 독점 블롭이 담당한다.

**블롭은 GPU ID 별로 빌드된다.** 파일 안에 `Mali-G72` 문자열이 있어도 그건 이름
테이블일 뿐이고, 실제 대상은 따로 있다. Rockchip 배포본을 올리면 이렇게 거부한다:

```
The DDK (built for 0x70020000 r1p0) is not compatible with this Mali GPU device,
/dev/mali0 detected as 0x6221 r0p3
```

`0x7002` 는 G52 다. 검사는 **제품 ID 정확 일치**를 요구한다 — 리비전 status 에만
허용 범위가 있다. HiKey970 용 G71 빌드(`0x6001`, DDK r10p0)로 확인했다.

```
ERROR: The DDK (built for 0x60010000 r0p0 status range [0..15]) is not compatible
with this Mali GPU device, /dev/mali0 detected as 0x6221 r0p3 status 0.
```

같은 Bifrost arch 6 인데도 거부한다. `0x6001` 은 arch 6.0.0/product 1 = G71,
`0x6221` 은 arch 6.2.2/product 1 = G72 다. 세대가 아니라 제품이 달라서다.

공개된 G72 빌드는 **ChromeOS 것뿐이다** (`/usr/lib64/libmali.so.0.54.1`, 복구
이미지에서 꺼낼 수 있다). Rockchip 계열 미러(JeffyCN 및 그 포크들)가 담고 있는
것은 utgard-450 / midgard-t86x / bifrost-g31 / bifrost-g52 / valhall-g310 /
valhall-g610 뿐 — Rockchip 이 실제로 출하한 GPU 목록이다. G72 를 쓴 칩(Kirin 970,
Exynos 9810, Helio P60·P70)은 전부 안드로이드 출하라 bionic 블롭이다.

그 블롭은 Wayland EGL 이 없다 — ChromeOS 는 Chrome 하나만 드라이버를 직접 쓰고
그마저 GBM 경로라, ARM 이 Wayland 플랫폼을 빌드에 넣을 이유가 없었다. 대신
**Vulkan 1.3 ICD** 가 들어 있다.

```
$ run-with-mali.sh ./vktest
device : Mali-G72
vkCreateInstance = 0
physical devices = 1
```

### musl 에서 굴리기

세 겹이 필요하다. `tools/run-with-mali.sh` 가 이를 묶어둔 것이다.

| 문제 | 해법 |
|---|---|
| glibc 가 `DT_RELR` 을 거부 (`GLIBC_ABI_DT_RELR` 표기 없음) | musl 로더로 연다. musl 엔 그 검사가 없다 |
| gcompat 에 없는 glibc 심볼 31개 | `tools/cros-mali-shim.c` — `_FORTIFY_SOURCE` 래퍼, C23 `strtol` 계열, `*64` 대용량 파일 별칭 |
| `initial-exec` TLS | musl 은 시작 시점 라이브러리에만 이 모델을 허용한다. `dlopen` 대신 `LD_PRELOAD` |

`gcompat` 은 `/opt/gcompat` 에 풀어 쓴다. apk 로 설치하면 다른 용도로 깔아둔
진짜 glibc 와 `ld-linux-aarch64.so.1` 을 두고 충돌한다.

### Wayland 출력 (WSI 레이어)

블롭의 Vulkan 에는 `VK_KHR_wayland_surface` 가 없다. Vulkan 로더가 드라이버 앞에
레이어를 끼우는 구조라, WSI 만 밖에서 붙일 수 있다. ARM 이 바로 그 레이어를
배포한다 ([ginkage/libmali-rockchip](https://github.com/ginkage/libmali-rockchip)
의 `libVkLayer_window_system_integration.so`). 이건 GPU 와 무관한 순수 WSI 코드라
Rockchip 배포본을 그대로 쓴다.

```
VK_LAYER_window_system_integration (Window system integration layer) active
  VK_KHR_wayland_surface : extension revision 6
  VK_KHR_swapchain       : extension revision 70
```

### glibc 에서 굴리기

musl 경로는 블롭을 띄우는 데엔 충분하지만, glibc 로 빌드된 앱(예: Zed)을 그 위에
올릴 수 없다. glibc 쪽으로 가려면 두 가지를 해결해야 한다.

**`DT_RELR` 표기.** glibc 는 `DT_RELR` 재배치를 가진 오브젝트가
`GLIBC_ABI_DT_RELR` 의존을 선언하지 않으면 로드를 거부한다. `DT_RELR` 을 모르는
옛 glibc 에서 재배치가 조용히 누락된 채 돌아가는 대신 큰 소리로 실패하게 하려는
장치다. 자체 로더를 쓰는 ChromeOS 빌드는 이 선언을 넣지 않는다.

기능은 다 있고 서류만 없는 상황이라, `tools/declare-dt-relr.py` 가 서류를 채운다.
`.dynstr` 과 `.gnu.version_r` 을 파일 끝에 복사하면서 버전 이름과 `Vernaux`
하나를 덧붙이고, 남아도는 `PT_NOTE` 헤더를 `PT_LOAD` 로 바꿔 그 블록을 매핑한
뒤 `DT_STRTAB`/`DT_VERNEED` 를 옮긴다. 기존 바이트는 하나도 건드리지 않는다.

**libc++.** 블롭은 `std::__1::__hash_memory` 를 쓴다 — LLVM 20 부터 있는 심볼이라
그 이전 libc++ 로는 링크가 안 풀린다. 호스트 것은 musl 빌드라 못 쓰고 freedesktop
런타임엔 아예 없으니, Debian arm64 `libc++1` (21.x) 을 `$PREFIX/lib` 에 같이 둔다.

이 둘을 하면 심(shim)도 gcompat 도 필요 없다. glibc 가 `__*_chk`, `__isoc23_*`,
`*64` 를 전부 원래 제공하기 때문이다. `tools/run-with-mali-glibc.sh` 가 환경만
묶는 것으로 끝나는 이유다.

```
$ run-with-mali-glibc.sh python3 vkprobe.py     # flatpak org.freedesktop.Sdk 안
physical devices: 1
  Mali-G72  api=1.3.313  vendor=0x13b5  type=INTEGRATED_GPU
```

### 화면에 그리기

kbase 가 GPU 를 가져가면 `/dev/dri/renderD128` 이 사라진다. 남는 DRM 노드는
디스플레이 컨트롤러(`mediatek-drm`) 뿐이라 Mesa 에 렌더 디바이스가 없고, KWin 이
뜨지 못한다. Panfrost 와 kbase 는 공존할 수 없으니 데스크톱도 같이 옮겨야 한다.

**KWin 은 이 위에 올릴 수 없다.** 블롭에 EGL 플랫폼이 하나도 없기 때문이다.

| 시도 | 결과 |
|---|---|
| `eglGetPlatformDisplayEXT(GBM / DEVICE / SURFACELESS)` | 셋 다 `EGL_BAD_PARAMETER` |
| `eglGetDisplay(gbm_device*)` (플랫폼 확장 이전의 경로) | `EGL_NO_DISPLAY`, 에러조차 세우지 않음 |
| Vulkan `VK_KHR_display` (컴포지터 없이 KMS 직접 출력) | 없음. `VK_EXT_headless_surface` 뿐 |

ChromeOS 는 Chrome 이 minigbm 으로 버퍼를 따로 할당해 오고 드라이버는 거기에
그리기만 하므로, ARM 이 EGL 플랫폼을 넣을 이유가 없었다. 이건 설정이 아니라
빌드에 없는 기능이다.

대신 **wlroots 에는 Vulkan 렌더러가 있다.** 블롭이 내주는 게 정확히 Vulkan 이니,
컴포지터(cage)와 클라이언트가 모두 벤더 드라이버 위에 올라간다.
`tools/run-cage-mali.sh` 가 그 환경이다.

### wlroots 에서 고친 것

`patches/wlroots-vulkan-no-drm-node.diff` — 둘 다 GPU 가 DRM 밖에 있다는 한 가지
사실에서 나온다.

| 수정 | 이유 |
|---|---|
| `WLR_VK_PHYSICAL_DEVICE` 로 디바이스 지정 | wlroots 는 백엔드의 DRM 노드와 Vulkan 디바이스를 `VK_EXT_physical_device_drm` 으로 짝짓는다. `/dev/mali0` 은 DRM 노드가 아니라 그 확장을 원리상 못 채운다 |
| 렌더러 DRM fd 를 백엔드 것으로 폴백 | fd 가 없으면 wlroots 가 `linux-dmabuf` 글로벌 자체를 만들지 않아, 클라이언트가 shm 으로 떨어진다 |

두 번째를 고치자 컴포지터가 Mali 가 알려준 포맷 목록(AFBC 모디파이어 포함)을
그대로 광고하기 시작했다.

### dma-heap

마지막 한 조각. WSI 레이어의 스왑체인 할당자는 `/dev/dma_heap/system` 을 열고,
없으면 진단 한 줄 없이 `abort()` 한다. pmOS 커널은 `CONFIG_DMABUF_HEAPS_SYSTEM`
없이 CMA 힙만 켜져 있다. 두 힙은 같은 ioctl 을 받으므로 이름만 걸어주면 된다.

```sh
ln -sf default_cma_region /dev/dma_heap/system
chmod 0666 /dev/dma_heap/default_cma_region
```

제대로 하려면 `CONFIG_DMABUF_HEAPS_SYSTEM=y` 로 커널을 다시 빌드하는 쪽이다.
CMA 는 물리적으로 연속된 메모리라 예약량을 넘기면 실패한다.

### EGL 셔틀 — KWin 을 올리려는 시도

블롭의 EGL 은 **완전히 동작한다.** 없는 것은 EGL 이 아니라 EGL 이 물려 있는
윈도우 시스템이다. 확인 결과 ARM 의 **dummy** winsys 였다 — `EGLNativeWindowType`
이 `struct { unsigned short width, height; }` 뿐이고, 그걸 넘기면 창 서피스가
정상적으로 만들어진다. 앞서 `gbm_surface` 를 넘겼을 때 `EGL_BAD_NATIVE_WINDOW` 가
아니라 `EGL_BAD_ALLOC` 이 났던 이유가 이것이다: 포인터를 검증하지 않고 앞 4바이트를
width/height 로 읽었다.

ChromeOS 도 이 winsys 를 쓰지 않는다. `libEGL.so.1` 은 `dlopen("libmali.so")` 후
`dlsym` 으로 넘기기만 하는 53KB 트램펄린이고(`libGLESv2`, `libGLESv1_CM` 도 같다),
`libgbm.so.1` 은 minigbm 이며, 둘을 잇는 코드는 Chrome 바이너리 안에
`GbmSurfaceless` 로 컴파일되어 있다. 이미지 어디에도 재사용 가능한 번역층은 없다.

그런데 KWin 은 `gbm_surface` 를 쓰지 않는다. 맨 `gbm_bo` 를 할당해
`EGL_LINUX_DMA_BUf_EXT` 로 import 하고 FBO 에 그린 뒤 직접 page-flip 한다 — 블롭이
전부 지원하는 동작이다. 그래서 빠진 것은 디스플레이 핸들 하나뿐이고,
`tools/egl-gbm-shim.c` 가 그것을 메운다.

| 필요한 것 | 확인 |
|---|---|
| gbm 버퍼 할당 | Mesa libgbm 으로 됨. minigbm 은 `MTK_GEM_CREATE` 가 ChromeOS 다운스트림 ioctl 이라 메인라인에서 `EINVAL` |
| dma-buf → EGLImage → FBO → 렌더 | **실측**: 256x256 버퍼에 GL 로 칠한 색이 그대로 읽힘 |
| dma-buf import 포맷 | 39개 (`XR24`/`XB24` 포함) |
| `EGL_KHR_no_config_context` / `surfaceless_context` | 있음 |
| `EGL_ANDROID_native_fence_sync` / `KHR_wait_sync` | 있음 |
| `EGL_EXT_buffer_age` | 없음 (KWin 에선 선택 사항) |

셔틀이 채우는 것은 두 가지다. `EGL_PLATFORM_GBM_KHR` 로 온 요청을 기본 디스플레이로
바꿔 넘기고(클라이언트 확장 문자열에 그 이름도 걸어야 한다 — KWin 은 부르기 전에
확인한다), `EGL_EXT_device_query` 에 답해 디스플레이 컨트롤러 노드를 알려준다.
GPU 가 DRM 밖에 있어 원리상 대답할 수 없는 질문이라, 버퍼가 실제로 오는 곳을 대신
알려주는 것이다.

이걸로 끝이다. KWin 이 벤더 드라이버 위에서 GLES 로 합성한다.

```
kwin_core: OpenGL compositing has been successfully initialized

Compositing Type:       OpenGL ES 2.0
OpenGL vendor string:   ARM
OpenGL renderer string: Mali-G72
OpenGL version string:  OpenGL ES 3.2 v1.r54p1-12eac0.d6e444aeb29579d8c20656d21c96307d
```

`kwin_wayland` 의 메모리 매핑에 `swrast` 도 `llvmpipe` 도 없다 — `/dev/mali0`,
셔틀, `libmali.so`, 그리고 버퍼 할당용 Mesa `libgbm` 뿐이다.

마지막에 한참 헤맨 것은 드라이버 탓이 아니었다. 디버깅하려고 준 `KWIN_COMPOSE=O`
가 원인이다. KWin 6.6 은 `O2` 와 `O2ES` 만 받고, 그 밖의 값이면 백엔드 초기화가
끝난 뒤에 **로그 한 줄 없이** `return false` 한다 (`compositor.cpp:166`). 백엔드는
그 전에 이미 성공해 있었다.

**`libEGL.so.1` 자리에 있어야 한다.** KWin 은 libepoxy 로 `dlopen("libEGL.so.1")`
한 뒤 그 핸들에 `dlsym` 하므로 `LD_PRELOAD` 로는 가로채지지 않는다. libmali 를
`DT_NEEDED` 로 걸어두면 나머지 함수는 의존성 사슬에서 해결된다.

```sh
gcc -shared -fPIC -Wl,-soname,libEGL.so.1 -o libEGL.so.1 egl-gbm-shim.c \
    -Wl,--no-as-needed /opt/mali/lib/libmali.so -ldl
```

SONAME 이 `libmali.so.0` 이므로 그 이름의 심볼릭 링크가 있어야 한다.
`system/plasma-mobile-mali.conf` 가 세션에 필요한 환경을 담고 있다.

### 클라이언트도 옮기기

컴포지터가 벤더 드라이버 위에 서도 클라이언트는 따라오지 못한다. 블롭에 Wayland
EGL 플랫폼이 없어서다. Qt 는 Vulkan 이라는 다른 문이 있어 넘어갈 수 있지만, 그
문은 KDE 설정으로만 열린다. `plasma-integration` 이 모든 KDE 앱 시작 시
`QOpenGLContext::create()` 로 GL 을 찔러보고, 실패하면 세션 전체를 Qt Quick
소프트웨어 백엔드로 못박기 때문이다. 백엔드를 명시하면 그 검사를 건너뛴다.

```ini
# ~/.config/kdeglobals
[QtQuickRendererSettings]
SceneGraphBackend=vulkan
```

소프트웨어 백엔드에서는 글자와 아이콘이 렌더되지 않는다. Vulkan 으로 넘기면
그 문제가 사라지고 셸도 GPU 가속을 받는다. KWin 은 영향받지 않는다 — 자기
`setGraphicsApi(OpenGL)` 로 나중에 덮어쓴다.

GTK3 에는 이런 문이 없다. Wayland 에서 EGL 만 쓰므로 이 스택에서는 가속할 방법이
없다.

### 아무것도 프레임 완료를 알려주지 않는다

세 겹이 모두 비어 있다.

| 동기화 수단 | 상태 |
|---|---|
| dma-buf 암묵적 펜스 | kbase 는 DRM 드라이버가 아니다 |
| `zwp_linux_explicit_synchronization_v1` | WSI 레이어는 알지만 KDE 가 Plasma 6 에서 버렸다 |
| `linux-drm-syncobj-v1` | KWin 은 알지만 DRM 렌더 노드가 없어 켜지 못한다 |

클라이언트가 바인드한 것을 보면 확인된다 — `zwp_linux_dmabuf_v1` 뿐이고 동기화
관련 global 은 하나도 광고되지 않는다. 그래서 컴포지터가 GPU 가 아직 쓰고 있는
버퍼를 합성한다. 프레임 시간 통계로는 잡히지 않는다: 60fps 를 지키고 드롭도 없는데
**내용이 틀린다.** 키를 연달아 누르면 앞 키가 눌린 프레임이 표시되지 않고 두 키가
함께 눌린 것처럼 보인다.

`tools/vk-present-sync-layer.c` 가 그 공백을 메운다. 빠진 동기화를 되살리는 대신,
프레젠트 직전에 큐를 비워 겹칠 수 없게 만든다.

```c
vkQueuePresentKHR(queue, info) {
    vkQueueWaitIdle(queue);
    return next_vkQueuePresentKHR(queue, info);
}
```

WSI 레이어보다 앱 쪽에 놓여야 하고(그쪽이 스왑체인 프레젠트를 구현한다), musl 과
glibc 양쪽에 각각 빌드해야 한다. 이걸로 입력 UI 의 어긋남은 사라진다.

**남은 문제.** 셸의 제어센터는 여전히 같은 형상을 보인다 — 슬라이드 애니메이션이
거의 끊겨 보이고, 토글의 색 변화가 한 프레임 앞질러 비쳤다 돌아온다. 스왑체인
이미지를 늘려도(2 → 4) 달라지지 않았다. 원인 미상.

### 결과

같은 화소 수(1920x1200, 2.30 Mpx)에서 Zed 를 재면:

| 스택 | 프레임 시간 |
|---|---|
| Panfrost + KWin | 33.3 ms |
| cage (wlroots Vulkan 렌더러) + Mali | 37.1 ms |
| **Plasma Mobile(KWin/GLES) + Mali** | **18.0~18.2 ms** (p95 27 ms) |

중앙값은 Panfrost 의 1.8 배지만 **p95 가 27ms 로 벌어져 있다** — 프레임이 고르지
않다는 뜻이고, 위 동기화 문제와 같은 뿌리로 보인다. 앞서 cage 에서 37ms 가 나온
것은 드라이버가 아니라 컴포지터 탓이었다: wlroots 의 Vulkan 렌더러는 스스로
실험적이라 밝히고 있고 전체화면 창을 직접 스캔아웃하지 않는다.

## 되돌리기

DTB 백업을 되돌리고 `mkinitfs` 후 재부팅하면 Panfrost 로 돌아온다.
커널 자체는 건드리지 않는다.

## 라이선스

`kbase` 는 GPL-2.0 (ARM). `tools/cros-mali-shim.c` 는
[tech4bot/rk3562deb](https://github.com/tech4bot/rk3562deb) 의 musl 심에서
출발해 이 블롭이 요구하는 심볼까지 채운 것이다.
유저스페이스 블롭은 ARM EULA 로 재배포할 수 없어 포함하지 않는다.
