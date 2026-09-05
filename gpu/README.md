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

`0x7002` 는 G52 다. Rockchip 은 G72 를 쓰는 SoC 를 만들지 않으니 해당 빌드가 없다.
공개된 G72 빌드는 **ChromeOS 것뿐이다** (`/usr/lib64/libmali.so.0.54.1`, 복구
이미지에서 꺼낼 수 있다).

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

### 남은 것

Vulkan 에 `VK_KHR_wayland_surface` 가 없어 Wayland 창에 출력할 수 없다. ARM 이
그 자리를 메우는 WSI 레이어를 따로 배포한다
([ginkage/libmali-rockchip](https://github.com/ginkage/libmali-rockchip) 의
`libVkLayer_window_system_integration.so`). Vulkan 로더가 드라이버 앞에 레이어를
끼우는 구조라, WSI 만 밖에서 붙일 수 있다.

## 되돌리기

DTB 백업을 되돌리고 `mkinitfs` 후 재부팅하면 Panfrost 로 돌아온다.
커널 자체는 건드리지 않는다.

## 라이선스

`kbase` 는 GPL-2.0 (ARM). `tools/cros-mali-shim.c` 는
[tech4bot/rk3562deb](https://github.com/tech4bot/rk3562deb) 의 musl 심에서
출발해 이 블롭이 요구하는 심볼까지 채운 것이다.
유저스페이스 블롭은 ARM EULA 로 재배포할 수 없어 포함하지 않는다.
