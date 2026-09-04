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
