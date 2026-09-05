# 설치

pmOS 순정 설치본에 apk 세 개를 얹으면 디스플레이와 카메라가 동작한다.
기기에서 빌드할 것은 없다.

대상: postmarketOS v26.06, `google-kukui` (aarch64), 커널 6.18.28

**krane (Lenovo IdeaPad Duet) 에서만 실측했다.** 같은 DT 를 쓰는 나머지
kukui 보드는 디스플레이 쪽은 그대로 동작할 가능성이 높지만, 카메라는
보드마다 센서와 핀이 다르므로 `mt8183-kukui-krane-cameras.patch` 를
참고해 해당 보드용 노드를 따로 써야 한다.

## 미리 만들어진 apk 로 설치

[Releases](https://github.com/rycont/mt8183-kukui-pmos/releases/latest) 에서 받아 기기로 복사한 뒤:

```sh
sudo apk add --allow-untrusted \
    linux-postmarketos-mediatek-mt81-*.apk \
    device-google-kukui-*.apk \
    libcamera-*.apk libcamera-ipa-*.apk libcamera-tools-*.apk
sudo reboot
```

`apk add` 가 `mkinitfs` 와 `boot-deploy` 를 자동으로 돌려 새 DTB 로 커널
파티션을 다시 굽는다. 따로 칠 필요 없다.

`--allow-untrusted` 가 필요한 이유는 로컬 abuild 키로 서명됐기 때문이다.
받은 파일이 맞는지는 릴리스의 `SHA256SUMS.txt` 로 확인할 것.

재부팅 후 확인:

```sh
cam --list                    # 카메라 두 대가 보여야 한다
cat /sys/class/drm/card*-DP-1/status
```

Snapshot 을 열면 카메라가 잡힌다.

## 직접 빌드

호스트(리눅스)에 pmbootstrap 이 필요하다. 기기에서는 빌드하지 않는다.

```sh
pmbootstrap init                     # channel v26.06, device google-kukui
```

이 저장소의 패치를 pmaports 에 적용한다:

```sh
PM=~/.local/var/pmbootstrap/cache_git/pmaports
K=$PM/device/community/linux-postmarketos-mediatek-mt81

cp display/patches/0001-dt-enable-dpi-it6505.patch  $K/mt8183-kukui-enable-dpi-it6505.patch
cp camera/dt/0001-dt-add-seninf-camisp.patch        $K/mt8183-add-seninf-camisp.patch
cp camera/dt/0002-dt-krane-cameras.patch            $K/mt8183-kukui-krane-cameras.patch
cp camera/patches/0003-kernel-add-isp50-drivers.patch $K/mtk-isp50-drivers.patch
```

`$K/APKBUILD` 의 `source=` 에 위 네 개를 추가하고 `pkgrel` 을 올린 뒤,
`config-postmarketos-mediatek-mt81.aarch64` 에 다음을 켠다:

```
CONFIG_MTK_SENINF=m
CONFIG_VIDEO_MEDIATEK_ISP_PASS1=m
CONFIG_VIDEO_OV8856=m
CONFIG_VIDEO_OV02A10=m
```

`device-google-kukui/kernel-cmdline.conf` 에 `cma=128M` 을 추가한다
(libcamera 소프트웨어 ISP 의 프레임 버퍼용, 기본 16M 으로는 부족하다).

libcamera 는 `temp/libcamera/APKBUILD` 에 `prepare()` 를 넣어
소프트웨어 ISP 를 켠다 — `libcamera/pmaports-libcamera-prepare.patch` 참고.

그다음:

```sh
pmbootstrap checksum linux-postmarketos-mediatek-mt81
pmbootstrap checksum libcamera
pmbootstrap build linux-postmarketos-mediatek-mt81 --arch aarch64 --force
pmbootstrap build device-google-kukui --arch aarch64 --force
pmbootstrap build libcamera --arch aarch64 --force
```

`libcamera` 에는 소프트웨어 ISP 활성화 외에 ov8856·ov02a10 센서 헬퍼도
넣어야 한다 — `libcamera/0004-libipa-add-ov8856-and-ov02a10-sensor-helpers.patch`
참고. 이게 없으면 AGC 가 게인을 못 올려서 실내가 4스톱쯤 어둡다.

`CONFIG_VIDEO_DW9768=m` 도 켜야 후면 AF VCM 이 잡힌다.

결과물은 `~/.local/var/pmbootstrap/packages/v26.06/aarch64/` 에 생긴다.

## 디스플레이만 필요하다면

외부 디스플레이와 DP 오디오는 커널 재빌드 없이도 된다.
DTB 만 고치면 IT6505 드라이버는 이미 메인라인에 있다.

```sh
patch -p1 -d <kernel-tree> < display/patches/0001-dt-enable-dpi-it6505.patch
make -C <kernel-tree> LLVM=1 ARCH=arm64 mediatek/mt8183-kukui-krane-sku176.dtb
scp <kernel-tree>/arch/arm64/boot/dts/mediatek/mt8183-kukui-krane-sku176.dtb \
    <device>:/tmp/
```

기기에서:

```sh
sudo cp /tmp/mt8183-kukui-krane-sku176.dtb /boot/dtbs/mediatek/
sudo mkinitfs && sudo reboot
```

## 되돌리기

pmOS 저장소 버전으로 돌아가면 된다:

```sh
sudo apk add --force-refresh \
    linux-postmarketos-mediatek-mt81 device-google-kukui libcamera
sudo mkinitfs && sudo reboot
```

커널 파티션은 하나뿐이라 잘못된 DTB 를 넣으면 부팅이 안 된다.
작업 전에 백업해 두는 편이 좋다:

```sh
sudo dd if=/dev/mmcblk0p1 of=~/kpart-backup.bin bs=1M
```

복구 USB (ChromeOS recovery 또는 pmOS 설치 USB) 도 준비해 두면 안전하다.

## GPU — ARM 공식 Mali 드라이버 (선택)

Panfrost 대신 ARM 의 벤더 스택(kbase + libmali)으로 돌리는 경로다. **Panfrost 와
동시에 쓸 수 없다** — 같은 DT 노드를 두고 배타적이다. 되돌리려면 DTB 만 원래대로
굽고 `mali-vendor-krane` 을 지우면 된다.

얻는 것과 잃는 것을 먼저 적어둔다.

| | |
|---|---|
| Zed | 33.3 ms → **18.1 ms** (1920x1200 전체화면) |
| Plasma Mobile | 동작 (KWin 은 GLES, Qt 앱은 Vulkan) |
| GTK3 앱 | **가속 경로 없음** — 블롭에 Wayland EGL 플랫폼이 없다 |
| 애니메이션 | 프레임 순서가 완전히 고르지는 않다. `gpu/README.md` 참고 |

### 1. 커널

kbase 모듈과 kbase 용 DTB 를 커널 패키지가 직접 만들게 한다. 손으로 넣어두면
커널을 다시 깔 때마다 조용히 지워진다 — 실제로 한 번 당했다.

```sh
PM=~/.local/var/pmbootstrap/cache_git/pmaports
K=$PM/device/community/linux-postmarketos-mediatek-mt81
cp gpu/patches/kbase-6.12-to-6.18.diff  $K/mali-kbase-6.12-to-6.18.diff
cp gpu/dt/mt8183-kukui-mali-kbase.diff  $K/
patch -p1 -d $PM < gpu/pmaports-kernel-mali.patch
```

위 "직접 빌드" 의 카메라·디스플레이 단계를 먼저 마친 상태여야 한다 —
`source=` 문맥이 겹친다. `pkgrel` 도 하나 올려둘 것.

`config-postmarketos-mediatek-mt81.aarch64` 에서 둘을 켠다:

```
CONFIG_DMABUF_HEAPS_SYSTEM=y
CONFIG_DEVFREQ_THERMAL=y
```

앞엣것이 없으면 WSI 레이어가 스왑체인 버퍼를 잡을 힙을 못 찾아 아무 말 없이
`abort()` 한다. CMA 힙으로 대신하면 창이 몇 개만 열려도 128MB 를 다 쓴다.
뒤엣것은 kbase 의 devfreq 쿨링이 요구한다.

```sh
pmbootstrap checksum linux-postmarketos-mediatek-mt81
pmbootstrap build linux-postmarketos-mediatek-mt81 --arch aarch64 --force
```

kbase 소스는 `source=` 가 알아서 받아온다 — ChromeOS 커널 트리의 사본을
[릴리스](https://github.com/rycont/mt8183-kukui-pmos/releases/tag/kbase-r44p1)
에 올려뒀다. 원본 URL 은 브랜치 머리를 그때그때 tar 로 말아주는 주소라
바이트가 재현되지 않아 체크섬을 걸 수 없다.

**DTB 는 두 벌이 깔린다.** kbase 와 Panfrost 는 같은 GPU 를 서로 맞지 않게
기술한다 (`gpu/README.md` 의 표) — 그래서 순정 DTB 는 그대로 두고 kbase 용을
`/boot/dtbs/mediatek-mali/` 에 따로 넣는다. 고르는 건 `/etc/deviceinfo` 한 줄:

```
deviceinfo_dtb="mediatek-mali/mt8183-kukui*"
```

`mali-vendor-setup` 이 이 줄을 쓰고 `mkinitfs` 까지 돌린다. Panfrost 로
돌아가려면 그 줄을 지우고 `mkinitfs` 를 다시 돌리면 된다.

### 2. 유저스페이스

패키지는 미리 빌드해두지 않는다 — 안에 든 게 스크립트와 C 소스뿐이라
기기에서 만드는 편이 릴리스에 올려두는 것보다 짧다. 이 저장소를 기기에
복사한 뒤:

```sh
cd gpu
./build-packages.sh                       # abuild 없으면 alpine-sdk 를 깐다
sudo apk add --allow-untrusted ~/packages/*/aarch64/mali-vendor-krane-*.apk
```

드라이버 자체는 ARM EULA 라 패키지에 못 넣는다. G72 용 리눅스 빌드는 ChromeOS
것뿐이므로 복구 이미지에서 꺼내온다:

```sh
sudo mali-vendor-setup
```

ChromeOS 복구 이미지 목록에서 이 보드 것을 찾아 받는다(압축 2GB). **받으면서
읽어 루트 파티션만 디스크에 쓰고**, 드라이버를 꺼낸 뒤 지운다. 이어서 WSI
레이어와 libc++ 를 받고, 심을 컴파일하고, `/etc/deviceinfo` 를 kbase DTB 로
돌린 뒤 `mkinitfs` 로 부트 파티션을 다시 굽는다.

`apk add` 만으로 여기까지 하지 않는 이유는 이 단계가 2GB 를 내려받고 부트
파티션을 다시 쓰기 때문이다. 패키지 설치가 조용히 할 일이 아니다. 이미 받아둔
파일이 있으면 `--image <파일>`, 마운트해둔 ChromeOS 루트가 있으면 `--root <경로>`
로 건너뛸 수 있다.

같은 스크립트가 블롭 혼자서는 모자란 두 개도 같이 받아온다:

- `libVkLayer_window_system_integration.so`
  ([ginkage/libmali-rockchip](https://github.com/ginkage/libmali-rockchip)).
  블롭엔 `VK_KHR_wayland_surface` 가 없고, 이 레이어가 그 자리를 메운다.
  GPU 와 무관한 코드라 Rockchip 배포본을 그대로 쓴다.
- LLVM 20 이상의 `libc++` (Debian arm64 `libc++1`). 블롭이
  `std::__1::__hash_memory` 를 쓰는데 그 이전 판엔 없다. flatpak 쪽
  `/opt/mali/glibc/lib` 에 둔다.

### 3. 확인

재부팅하면 KWin 이 `Mali-G72` 로, Qt 앱이 `/dev/mali0` 로 붙는다.

```sh
qdbus6 org.kde.KWin /KWin supportInformation | grep -i "OpenGL renderer"
```
