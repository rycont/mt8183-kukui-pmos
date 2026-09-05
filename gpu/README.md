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

kbase 는 커널 쪽일 뿐이고, GLES/EGL 은 독점 블롭이 담당한다.

- **ChromeOS 블롭은 못 쓴다.** Wayland 지원 없이 GBM/surfaceless 로만 빌드돼
  있어 (`EGL_KHR_platform_wayland` 없음) Wayland 클라이언트가 출력할 수 없다.
- **Rockchip 배포본을 쓴다.** `libmali-bifrost-g52-g13p0-wayland-gbm.so` 는
  파일명과 달리 Bifrost 전 세대를 지원해 G72 가 목록에 있고, EGL Wayland 와
  GBM 을 모두 갖췄다. uAPI major 11 로 r44p1 커널과 맞는다.
- 이 블롭엔 Vulkan 이 없다. Vulkan 이 든 빌드(g29p1 계열)는 `VK_KHR_wayland_surface`
  가 없어 별도의 WSI 레이어가 필요하다.

블롭은 glibc 로 빌드돼 있어 musl 인 pmOS 에서는 `gcompat` 과
`tools/mali-shim.c` (musl 에 없는 glibc 심볼 8개) 가 필요하다.

`mali_platform.conf` 는 ChromeOS 가 쓰는 플랫폼 설정값이다. Rockchip 블롭도
같은 `MALI_PLATFORM_CONFIG` 환경변수를 읽는다.

## 되돌리기

DTB 백업을 되돌리고 `mkinitfs` 후 재부팅하면 Panfrost 로 돌아온다.
커널 자체는 건드리지 않는다.

## 라이선스

`kbase` 는 GPL-2.0 (ARM). `tools/mali-shim.c` 는
[tech4bot/rk3562deb](https://github.com/tech4bot/rk3562deb) 의 심을 확장한 것이다.
유저스페이스 블롭은 ARM EULA 로 재배포할 수 없어 포함하지 않는다.
