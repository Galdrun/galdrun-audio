# Connector 3 handover for 2026-10-31

## Current state

### Sleep/deep-sleep parity investigation — 2026-11-01 (ongoing)

#### Latest verified RAM state — Ethernet wake works with stock PLL

The actual repository PSCI implementation now passes Ethernet magic-packet
wake in RAM: kernel `d1043a9368647335a14b646023f1f9dabe5ddd46a88c7fd3cfff7d8a71dcfb74`,
**result0, IRQ32, 6 seconds**, before the 20-second RTC fallback. Secure monitor
`4fdd38d3...` restores CNTFRQ. Playback after resume is RUNNING with advancing
ALSA pointers and `arch_sys_counter`. Evidence:
`sleep-clean-pm-ethernet-20261101.txt`, matching `-host.json`, and
`sleep-clean-pm-playback-20261101.txt` in the reference archive.

The working mode powers down CPU0 through secure PSCI state ID0, puts DDR in
self-refresh and retains SCU/peripheral power for wake. GPC uses the vendor
WFI power-gating path; **stock PLL handling remains unchanged**. Full-SoC STOP
missed Ethernet wake even with the existing peripheral mapping restored
(`sleep-stockpll-mf-retain-20261101/`); that comparison is finished, not pending.
Power consumption has not been measured. Earlier prototype `21f1bcbc...`
also passed Ethernet wake (4 seconds, IRQ32, MAC event set).

Current device: clean Ethernet-only RAM candidate
`private/connector3/sleep-clean-pm-20261101/`, under the timer-fixed USB monitor.
NAND and the qualified installer remain unchanged. Production DT still leaves
suspend disabled; the client helper is not yet installed.

Front POWER wake remains unqualified: the previous 60-second test returned by
RTC without a confirmed press during sleep. User readiness has been requested
for a coordinated repeat. There are no Ethernet link lights to observe; do not
ask again. A WLAN-enabled private DT booted the radio, but the standard network
manager disconnected WLAN because Ethernet was ready. A real WLAN-only test
still requires unplugging Ethernet. Do not classify that intentional policy as
a radio failure or claim Wake-on-WLAN support.

#### Earlier verified RAM baseline — keep stock PLL handling

The user explicitly directed keeping the working stock PLL sequence. **Do not
continue PLL-override experiments.** Kernel `1610f54a...` with stock PLL handling,
CPU1 parked and CPU-PM notifications, under timer-fixed monitor `4fdd38d3...`,
passed real RTC suspend/resume: **20 seconds, IRQ36, result0**, with
**CNTFRQ=8000000/8000000 and CNTKCTL=0x96/0x96**. Playback resumed with ALSA
RUNNING and advancing hw/appl pointers. A following RAM-to-RAM kexec also booted
correctly, retaining `arch_sys_counter` and advancing playback. This is the
working baseline, not the PLL-omission candidate.

Evidence: `sleep-timerpm-stockpll-rtc-20261101.txt`,
`timerpm-stockpll-playback-after-resume-20261101.txt`,
`timerpm-stockpll-after-kexec-20261101.txt`, plus master UART log.
Kernel: `private/connector3/sleep-psci-cpu1-timerpm-stockpll-20261101/`, full SHA256
`1610f54a13b6f27ca5ab60659b9401d4ecab5146d123b945ca7aa733f8974a3f`.
Current device runs that RAM kernel under the USB-loaded timer-fix monitor.
Ethernet wake, front POWER wake and WLAN resume still require qualification.
The NAND installation/installer remain unchanged.

Stock-PLL Ethernet repeats under the timer-fixed monitor still woke only via
RTC: `sleep-psci-timerpm-eth-deep-20261101.txt` (20 s) and
`sleep-stockpll-ethernet-link-observe-20261101.txt` (60 s), result0/IRQ36 and six
broadcasts each. The user reports **no Ethernet port link lights**, so do not
ask for that observation again. Private diagnostic `a6fc8d78...` differs from
`1610f54a...` only by guarded FEC register logging before sleep and after clock
restoration. It found **ECR=0x7000011e, EIR=0x10000000, EIMR=0x20000 both before
and after**: wake mode remains armed, but the magic-packet event bit was not set.
Evidence `sleep-stockpll-fec-flags-20261101.txt` and matching `-host.json`.

Next RAM comparison `sleep-stockpll-mf-retain-20261101/`, kernel
`59c6157eec1fda7a7e5237a144be6b60d827b19df702248d02200708573b0a2a`,
removes only the earlier experimental write mapping Mega/Fast peripherals to
the A7 power domain. It preserves their existing mapping instead; PLL settings
remain exactly stock. `boot-next-sleep-ram-20261101.py` targets this stage.
Its wake test is pending. The verified `1610f54a...` baseline is retained.

The hardware-confirmed CNTFRQ fix is now wired into the production profile of
`scripts/build-connector3-monitor.sh` via `board/connector3/monitor/restore-cntfrq.patch`.
Shell syntax, clean patch dry-run and freshly compiled monitor fail-closed hook
regressions passed. Production artifacts were **not rebuilt/replaced**; only
the isolated USB/RAM candidates contain the fix at present.

The corrected USB loader `8c8f0685...` booted successfully; UART pin/model/six
RO flags, release suffix, kexec state and USB SHA256 passed. It successfully
loaded the RAM test. The combined PLL-omission/timer-PM kernel `41864edb...`
did not return from its RTC test or subsequent magic packets. The user recovered
the same USB loader using Reset (no new file copy), then loaded `1610f54a...`
which passed the checks above. The timer fix is therefore hardware-confirmed
with **stock** PLL handling. The omitted-PLL experiment remains rejected.

#### Latest USB checkpoint — corrected loader awaiting physical boot

The user booted the first timer-monitor USB (`28857cac...`). UART confirmed
the storage-check model, six MTD flags `0x0` and a fresh SSH public key. That key
was pinned from UART in `known_hosts_sleep_monitor_usb_20261101`. SSH verified
the USB file SHA256, all six read-only flags and no UBI mounts. The RAM uploads
passed their hashes, but kexec load returned **Function not implemented**:
that old storage-check kernel lacks CONFIG_KEXEC. No sleep test started and no
NAND writes occurred. The device remains in that responsive read-only USB image.

**Replacement USB image ready:**
`private/connector3/sleep-monitor-cntfrq-kexec-20261101/connect3.img`, SHA256
**`8c8f06852220ebc5f42cf21ebc4c202530450011a2d220fe8902c2f78c7269d0`**.
The user has been asked to copy this onto USB and repeat the Reset boot;
that reply is pending. Do **not** reuse the first image above for this test.
New loader kernel `8671e660...` is an isolated copy of the exact qualified
`e010da1d...` source with SUSPEND/ARM_CPU_SUSPEND/KEXEC enabled. Config and linked
`sys_kexec_load` symbol checks passed. The original rootfs hash and controller
NAND write refusal remain intact. The DT is unchanged, the timer-fix monitor is
still `4fdd38d3...`, and FIT signature/component/tamper checks passed.
Build/pack: `build-sleep-usb-kexec-20261101.py`, log
`logs/connector3-sleep-usb-kexec-20261101.log`.

After boot, repeat the UART marker/model/flags/public-key read and rerun
`pin-sleep-monitor-usb-20261101.py` (new random SSH key on each USB boot).
`boot-sleep-from-monitor-usb-20261101.py` now requires the corrected image hash,
kernel suffix `-galdrun-monitor-ro`, and `/sys/kernel/kexec_loaded=0` **before**
uploading anything. Then it loads RAM kernel `41864edb...` for the timer/PLL test.
The qualified NAND image, installer and persistent monitor remain unchanged.

**Current blocker / next physical action:** the successful CPU suspend exposed
lost timer context. After two real resumes, RAM-to-RAM kexec into the ENET-PLL
candidate booted with **CNTFRQ=0 / sched_clock 0 Hz / Failed to initialize
'/timer': -6**, then stopped progressing. Its Ethernet sleep test **never
started**; `No route to host` is a boot failure, not a result for the PLL fix.
The user was asked to boot a new **read-only USB monitor diagnostic**, replacing
the separate normal recovery cycle. The reply to that USB request is pending.

USB artifact: **`private/connector3/sleep-monitor-cntfrq-20261101/connect3.img`**,
SHA256 **`28857caca71c73c85b7225ab7af12799f8f853677084ae84ebd4d976979b1ac5`**.
It uses the already cold-boot-qualified storage-RO kernel `e010da1d...`, with
controller-level NAND write refusal, and a new monitor `4fdd38d3...` which saves
and restores CNTFRQ in secure CPU suspend. Build, fail-closed config/symbol checks,
FIT component hashes, required signature and tampered-signature rejection passed.
**Hardware validation remains pending.** Qualified installer `a9c033fb...` and
NAND installation are unchanged. This USB image is **not an installer**.
The monitor patch is `board/connector3/monitor/restore-cntfrq.patch`; only the
isolated `connector3-monitor-suspend-cntfrq*` cache builds apply it so far.
Build/pack helper: `reference/connector3-ssh-archive/build-sleep-monitor-20261101.py`.

After USB boot, use the physical UART console to print marker
`MONITOR_TIMER_USB_20261101`, model, all six `/sys/class/mtd/mtd*/flags` and
`/etc/ssh/ssh_host_ed25519_key.pub`. Then run
`pin-sleep-monitor-usb-20261101.py`; it pins the random USB SSH key from UART,
verifying model and six read-only flags. Run
`boot-sleep-from-monitor-usb-20261101.py`: verifies the USB image's SHA256 from
a read-only FAT mount, verifies MTD flags/no UBI mounts, uploads/hash-checks the
known static kexec and RAM components, and boots the test under the new monitor.

The next RAM kernel is **`41864edb52572642c01fbf614719a1c2bc8ece9b94040e85f5aee4f990bb93db`**
in `private/connector3/sleep-psci-cpu1-enetpll-timerpm-20261101/`. It parks CPU1,
uses secure ID0, retains PLL_ENET and adds standard `cpu_pm_enter/exit` calls
(`CONFIG_CPU_PM=y`) to restore Linux timer access controls. It logs/checks
**CNTFRQ and CNTKCTL before/after**; mismatch returns EIO. Build passed; not yet
booted. First run RTC-only `sleep-psci-20261101.sh`, then Ethernet runner,
then verify playback and another RAM-to-RAM kexec before claiming safe resume.
The Ethernet runner now writes unique `sleep-psci-timerpm-eth-deep-*` evidence.

**Newest confirmed progress:** after normal recovery, kernel `f2d5bff...`
successfully suspended/resumed through secure PSCI **ID0** with CPU1 parked.
RTC elapsed **20 seconds**, wake IRQ **36**, Linux suspend result **0**; monitor
printed **Back from Suspended**. The old `info->resumed=0` message belongs to the
unused nonsecure trampoline and is not the secure resume indicator.
This confirms actual secure CPU suspend/resume, not the earlier WFI return.
Evidence: `sleep-psci-cpu1-suspend-rtc-20261101.txt` and master UART log.

Ethernet magic-packet testing on this same kernel still woke only via RTC20/IRQ36,
despite six native Windows broadcasts. Evidence is the master UART log plus
the observed host result (28.297 seconds, six broadcasts, exit0, RTC20/IRQ36).
The original raw `sleep-psci-cpu1-eth-deep-*` filenames were subsequently reused
by the failed next-image boot and now contain that connection failure.
Live clock ancestry shows **PLL_ENET** supplies the PHY's 50 MHz reference,
FEC `enet2_ipg_root_clk` and `enet_axi_root_clk` (250 MHz branch). Generic AHB/IPG
uses PLL_SYS, but those are not the FEC bus clock roots. The next RAM candidate
`sleep-psci-cpu1-enetpll-20261101/` omits **only** PLL_ENET's suspend override,
retaining other PLL overrides, pending Ethernet/RTC testing. Qualified NAND is
unchanged. This candidate's following kexec exposed the timer-context problem
described above; **do not** treat its RTC success as complete resume qualification.

Newest checkpoint after the user's device power cycle and PC restart:
the pinned NAND model, MCU helper hash `3a990234...`, RO RootFS and RW settings
were reverified. The existing Windows stack was restarted with its saved
`scripts/.run/config` files; LAN remains `10.0.0.6/24`. COM5 capture was restarted
hidden, PID **34880**, using the same master log/TX queue below.

Ethernet-only prepared-PSCI kernel `f5bd6eb3...` resumes from **WFI**, but does
**not** yet power the CPU off. With `console_suspend=N`, an RTC-only test returned
after exactly **20 seconds**, wake IRQ **36**. A magic-packet test returned after
**9 seconds**, wake IRQ **32**, with FEC EIR changing `0x10000000 -> 0x10020000`.
Both returned suspend-write error 1 / `Invalid argument`: monitor return through
WFI is not CPU-off resume. Do not treat those results as qualified deep sleep.
The Ethernet run followed a diagnostic `devmem` fault and needs clean repetition.
With `console_suspend=Y`, the earlier RTC-only test hung and required power-cycle
recovery; monitor debug output depends on that UART, so keep its clock enabled.
This UART explanation is a hypothesis, not a proven root cause of that hang.

A fresh RAM kernel `3a7288ef...` repeated the 20-second RTC/IRQ36 WFI result
without a prior fault. Its kernel readbacks confirm C0/SCU power-gating bits 1,
but GPC CPU_MAPPING is `0x4c`, missing the vendor's Mega/Fast-to-A7 mapping bit 0.
The next isolated candidate adds/restores that missing vendor setup step.
No tracked production PM source has changed. Avoid userspace `devmem` reads of
GPC PGC C0: they caused an external abort and the PCIe abort handler faulted.
RAM-to-RAM kexec cleared that fault before the clean RTC repeat.
Evidence: `sleep-psci-rtc-console-on-20261101.txt`,
`sleep-psci-eth-wfi-20261101.txt`, `sleep-psci-eth-wfi-host-20261101.json`,
`sleep-psci-registers-rtc-20261101.txt`, and `sleep-registers-20261101.txt`
under the ignored reference archive. Kernel snapshots are under private
`sleep-psci-ethernet-20261101/`, `sleep-psci-registers-20261101/` and
`sleep-psci-mapping-20261101/`; the latter two include generated PM source.

Further isolated RTC comparisons all returned after 20 seconds with IRQ36 and
CPU-off result 1: mapping-only (`fcf65a1f...`), eight-cycle RBC
(`72bc4e13...`), VSTBY (`b986679b...`), and full stock STOP controls including
SBYOS (`3bdab508...`). Those changes are **not** proven fixes. Their private
directories and `sleep-psci-*-rtc-20261101.txt` preserve source/results.
The next clock-isolation candidate (`851469d...`, no extra FEC clock holds)
stalled before the first GPC diagnostic, while the old diagnostic still read
unclocked FEC registers. This is an invalid diagnostic, not evidence that the
CPU entered sleep or that RTC failed. User recovery was requested. The corrected
build removes both FEC readback messages, retaining GPC readbacks; output
`private/connector3/sleep-psci-no-fec-read-20261101/`.
Corrected kernel SHA256:
`2a127267d79d17bcc5a7a03a3cab7c73a7b53f3d49151c03cca0f6439de84f7f`.
The user recovered normally. This corrected test **did run**: RTC20/IRQ36,
suspend result 1, no CPU-off. Ethernet recovered automatically. Evidence
`sleep-psci-no-fec-read-rtc-20261101.txt`. Do not run the invalid no-hold candidate.
MCU button wake control exists at **`/sys/class/tty/ttymxc2/power/wakeup`** and
is disabled; its IRQ is **31 / GPCv2 28 / 30880000.serial**. Console is the
separate **30860000.serial / GPCv2 26**. Do not confuse the MCU UART with COM5's
console UART. Button wake has not yet been tested.

#### Latest CPU1 finding and pending recovery

Kernel `f8c34ee7...` tested PSCI state ID1 (the monitor's low-power-idle path)
instead of ID0. It also returned WFI/RTC20/IRQ36/result1. Crucially, its kernel
readback was **ACTLR=0x6040, SRC_A7RCR1=0x3**: firmware has CPU1 enabled despite
Linux using only CPU0. The monitor's `get_online_cpus()` interprets SRC bit1
as a second online CPU, so ID1 does not take its last-CPU DDR/clock path.
This is a concrete mismatch, not yet a qualified root-cause fix.

Private DT `sleep-psci-dualcore-20261101/connector3.dtb` enables CPU1 and PSCI
enable-methods, retaining compiled `maxcpus=1`. Both CPUs became possible/present,
but Linux hotplug returned **Input/output error / CPU1: failed to come online**.
Evidence: `cpu1-psci-20261101.txt`, `cpu1-psci-failure-20261101.txt`.

Candidate `2f56c94b...` used existing `imx_enable_cpu(1, false)` only while that
CPU is possible but offline. This successfully parked CPU1 at boot:
**SRC_A7RCR1=0x1**. Its RTC test timed out. **Later UART output confirms that it
did reach secure CPU_SUSPEND ID1**, with the corrected SRC value; the earlier
observation that it stalled before firmware entry was premature. It did not
return through the RTC fallback. No CPU-off/wake success is established.
The user was asked to power-cycle normally again; recovery is currently pending.
Evidence `sleep-psci-cpu1-park-rtc-20261101.txt` plus master UART log.

Private candidate `sleep-psci-cpu1-gpc-order-20261101/` adds precise entry
markers and the vendor's pending-IOMUX-IRQ hold around GPC LPM selection.
Build passed, kernel SHA256
`cfea8b971c1868ac838ffdada24fab905bcdf94675d491266f359956323d9360`.
Vendor `gpcv2.c` explicitly documents premature low-power entry before WFI
when this sequencing is wrong. This candidate was built but **not booted**;
later serial evidence showed the preceding candidate already reached ID1.

**Next test:** `sleep-psci-cpu1-suspend-20261101/` retains CPU1 parking and the
sequencing/markers, returns to PSCI **ID0**, restores FEC clock holds and clears
VSTBY/SBYOS to retain the carrier clocks/supplies. The ID1 path's last-CPU branch
switches off XTAL and LDOs; it is not qualified for this board. Build passed:
`f2d5bffcb0f978163f08f5b40b6390cf11cecec76b80467f8376918c4f114fa3`.
This candidate has **not been booted/tested**. On recovery use
`boot-sleep-psci-20261101.py`, which now creates the dualcore private DT and loads
this ID0 candidate. `boot-next-sleep-ram-20261101.py` targets the same kernel for
later RAM-to-RAM replacements. No production PM changes or
sleep-related NAND writes have occurred. The SSH helper now saves partial output
and reports a concise exit124 on its 90-second timeout.

The user explicitly requested Connector 2's sleep/deep-sleep abilities on
Connector 3. The qualified audio/LED NAND installation below is preserved;
no sleep-related NAND writes or release-image replacements have occurred.
Current NAND still reports only `[s2idle]` and has no suspend helper.

Manual standby/network wake on the qualified NAND image passed: client API
remained reachable while `standbyManual`, audio stopped and resumed on
`standbyOff`; saved timers remained zero. Evidence:
`reference/connector3-ssh-archive/standby-nand-20261101.json`.
Read-only live `iw phy phy0 info` advertises WoWLAN magic packet, disconnect,
pattern match and net-detect; WoWLAN and PCIe wake are disabled. This does not
qualify WLAN wake from deep sleep. Ethernet advertises magic-packet support.

First RAM test used retained kernel `09fb5868...` and a private DT opt-in for
the existing nonsecure OCRAM suspend port. It reached the pre-WFI diagnostic
but **did not resume**, including the 35-second RTC fallback. The user
recovered by a normal power cycle; NAND audio initialization passed afterward.
Do not repeat that test as if its older vendor-monitor RTC result still applies.
Helpers/evidence: `boot-sleep-ram-20261101.py`, `sleep-baseline-20261101.sh`,
`run-sleep-baseline-20261101.py`, and the master COM5 log under the private paths.

The installed monitor has a **strong** `imx7_cpu_suspend` implementation and
advertises PSCI CPU_SUSPEND, while SYSTEM_SUSPEND remains an unsupported weak
stub. A private kernel candidate delegated directly to CPU_SUSPEND with a
private `arm,psci-0.2`/SMC DT node. Firmware returned **“Not suspended, GPC IRQ
Pending”**; resume then hit `mwifiex_pcie ... Unable to change power state from
D3hot to D0, device inaccessible` and did not finish. The user performed a
second normal recovery power cycle. Do not call this a successful suspend.
Candidate `bc30c782...` and inputs remain under
`private/connector3/sleep-psci-20261101/`; host result is
`reference/connector3-ssh-archive/sleep-psci-host-20261101.json`.

Next private candidate retains the existing i.MX7 PLL/GPC/counter preparation
before invoking secure CPU_SUSPEND and disables PCIe/WLAN **only in its RAM DT**
to isolate Ethernet and avoid the WLAN resume failure. Build/boot helpers are
`build-sleep-psci-20261101.py` and `boot-sleep-psci-20261101.py` under the private
reference archive; output `private/connector3/sleep-psci-ethernet-20261101/`.
All kernel changes so far live only in the isolated cache
`~/.cache/raumfeld-buildroot/connector3-sleep-psci-20261101/linux`.
The tracked production PM source and monitor are unchanged. The test uses
the strict RAM SSH pin (same private RAM host key as earlier) and restores
client/LED processes when resume completes. Current UART capture is PID 34568,
same `fit-update-console-20261031.log` and TX queue as below.

### Audio regression investigation (newest checkpoint)

#### 2026-11-01: REINSTALL, COLD BOOT, MUSIC AND THREE LEDs CONFIRMED

The user powered off, removed USB and powered on **without Reset** after the
completed reinstall. The exact new FIT `e9542fdc...` booted from NAND.
`verify-installed-audio-led-20261101.py cold` passed actual installed binary
hashes, read-only root, writable settings, factory protection/fingerprints and
zero uncorrectable ECC errors. SDMA firmware **4.6** loaded before playback.
MCU startup initialized 48000, then the client set/read back actual **44100**
without manual diagnostic commands. Music frames advanced for 15 seconds with
no additional backend/timeline underruns.

Saved client ID **`c170ac6d4af8eddd`**, the new SSH host key and deliberately
saved **`ledsOff=true`** survived the physical power cycle. The verifier then
restored and saved **`ledsOff=false`**, returning normal front-LED status mode.
The final snapshot played **“Miss World”**, `tidal:track:18256342`,
S16_LE/stereo/44100 at hardware **53/100**, mute 0. The user confirmed:
**“Music audible; three LEDs steady.”** This is listening confirmation on the
newly reinstalled, physically cold-booted **NAND** image, without USB/Reset or
manual audio initialization. The requested install/reboot/music/LED scope has
passed. Normal status mode is restored and saved; playback remains running.

Use the qualified installer at **`private/connector3/installer-write/connect3.img`**,
SHA256 **`a9c033fb3a2ccc9ed4b5625f5289956993f4fafccd9e24422d4a42eb81d8bb6f`**.
It installs FIT `e9542fdc...` and rootfs `39b40ae9...` as detailed below.
Its exact USB boot, install/readback and NAND cold-boot evidence now qualify
these new artifacts; do not substitute an old tested container or rebuild and
carry this qualification forward unchanged.

Joined qualification: **`output/connector3-persistent/hardware-verification.json`**,
also frozen with the exact installed inputs under
`private/connector3/persistent-installed-e954-audio-led-20261101/`.
The installer directory's hardware report records the successful install and
cold-boot music result too. Corresponding sources contain the required shared
LED implementation: `connector3-sources-86e60980d1ed.tar`, SHA256
`9c8f7e30efaa70e9f2ca21eb77c2ef008e72b90ed62f6594604da178896a6f19`;
659 member hashes and private-path exclusions passed. No commit or cleanup
was requested; unrelated dirty work is preserved.

Evidence under `reference/connector3-ssh-archive/`:
`installed-audio-led-cold-20261101.json` and matching `-runtime-`/`-status-`
files. Strict current NAND pin:
`private/connector3/known_hosts_persistent_audio_led_20261101`.
The earlier persistent pin belongs to the pre-reinstall system; retain it for
rollback, do not overwrite or disable key checks. Current configuration/cache
paths are **`/settings/client.json`** and **`/settings/audio_devices.json`**.
The server/core remain running, USB is absent and no flash operation is active.
All following checkpoints describe earlier stages of this work.

#### 2026-11-01: USB boot verified; reinstall complete, NAND boot running

Holding Reset before reconnecting power for 10 seconds successfully entered
the new **installer write** USB profile. COM5 verified model, kernel, Ethernet
MAC and the new public SSH host key; strict pin is now
`private/connector3/known_hosts_installer_audio_led_20261101`.
Actual USB image and appended payload hashes, preflight, factory fingerprints,
MTD protections, private settings and zero uncorrectable ECC errors passed.
USB UART archive: `reference/connector3-ssh-archive/audio-led-usb-cold-uart-20261101.log`.

The original USB beta backup was retained as `galdrun-backup-beta-20261031`.
Fresh current-NAND backups completed on USB and every member was copied and
independently hash-verified on the host at
`private/connector3/reinstall-backup-audio-led-20261101/`.
Fresh backup manifest pin:
`f0d187a31bf15103c2eba50361309a6ad04d4303f5ab8c891c6fc48efe439969`.
Report: `reference/connector3-ssh-archive/fresh-usb-backup-audio-led-20261101.json`.

The requested reinstall completed with **exit 0** through the unmodified, verified
`/usr/sbin/galdrun-install --install <fresh-pin>`; only MTD4/MTD5 are writable.
Rootfs/volume/FIT readbacks passed; MTD0 stayed protected and MTD1–MTD3 hashes
were unchanged. Completed host records are
`reference/connector3-ssh-archive/install-audio-led-20261101.{json,log}`.
Its device log/status paths were `/run/install-audio-led-20261101.{log,status}`.
Host helper: `reference/connector3-ssh-archive/install-audio-led-20261101.py`.
Exact installed inputs are frozen at
`private/connector3/persistent-installed-e954-audio-led-20261101/`.
A normal software reboot has reached the **new NAND image**. UART authenticated
the new persistent SSH pin at `private/connector3/known_hosts_persistent_audio_led_20261101`.
Initial startup generated persistent SSH keys (about 16 seconds), then MCU
initialization, actual 44100 rate readback and ready indication 10 passed.
Init/client/LED hashes match; the MCU hash matches the persistent image's
recorded **3a990234...**, not the separately compiled RAM ELF (its assertion
source paths differ). The remaining required check is physical cold boot with
saved LED preference and actual music.
Prepared verifier: `verify-installed-audio-led-20261101.py first|cold` in the
same reference archive; its first phase saves LED off for a cold-boot test,
and its cold phase verifies it then restores normal status mode.

The `first` phase **passed** actual NAND FIT/init/client/MCU/LED hashes,
read-only root, writable settings, preserved client ID, new persisted SSH key,
automatic MCU baseline/actual 44100, embedded SDMA firmware 4.6 and unchanged
factory fingerprints. Music frames advanced for 15 seconds with no new
backend/timeline underruns. It then saved **`ledsOff=true`** in
`/settings/client.json` and synced. The user was asked to unplug power, remove
USB and power on **without Reset**. LEDs are deliberately off until the cold
verifier confirms persistence and restores normal status mode. Keep this
intentional test state distinct from an uninitialized or stuck device.
Evidence: `reference/connector3-ssh-archive/installed-audio-led-first-20261101.json`
and matching runtime/status files. First-boot music listening has not been
separately requested; final cold-boot music confirmation remains required.

#### 2026-11-01: automatic audio startup and three-LED installer candidate

The user's new scope authorizes making a reinstallable working image and LED
patterns. The frozen audible snapshot below remains intact. No NAND has yet
been written in this follow-up.

Implemented and built:

- MCU startup now verifies encoded 48000 Hz while muted, after sync/source/On,
  and publishes its socket only after initialization succeeds. Failed startup
  exits; persistent init refuses to start the client without MCU readiness.
- The root-only socket accepts `rate <Hz>`. `AlsaBackend::open` sends each
  actual Connector 3 PCM rate after hardware parameters, before rendering.
  GET uses selector `0x01000000`; mute is preserved on success and a failed
  rate change remains muted. No fixed-48000 policy is imposed on music.
- Source-qualified 48000 and 44100 SET/readbacks passed on hardware. The user
  confirmed **music audible at actual 44100** with the new helper and **three
  front LEDs lit** in the ready pattern.
- Original firmware was inspected locally with explicit user permission.
  MCU `Indication` is SET `0x20`, sent unchanged by `hardwared` at `0xea330`;
  `libraumfeldcpp-1.0.so` enum metadata at `0xf844c` supplies the names/values.
  Physical hardware **rejects GET `0x20` with status 3**; use SET ACK only.
  The separate Bluetooth indicator is `0x21`; it is not a status LED or an
  implemented Bluetooth audio backend. No vendor daemon/library is packaged.
- Connector 3 reuses Connector 2's status classifier/monitor with
  `CONNECTOR3_STATUS_LEDS`, using autonomous MCU patterns: boot **4**
  (`initializing`), connecting **5**, setup **6**, ready **10** (`apps-running`),
  error **22** (`error-firmware`), off/standby **0**. `statusLeds` is enabled;
  `ledsOff` is saved through the existing client settings API.

Native MCU self-test, ARM build/QEMU self-test, Connector 2 and Connector 3
LED integration tests, persistent seed/settings/readiness checks and content
audits passed. The persistent kernel embeds SDMA 4.6 along with the approved
radio/regulatory firmware. Signed FIT verification and tamper rejection passed.

**Current boot:** rebuilt generic RAM image with a private settings/SSH overlay,
model `Galdrun Connector 3 RAM audio and LEDs`, strict existing RAM pin, `.74`.
It automatically initialized 48000, then actual 44100, and started music with
no manual diagnostic rate SET. Saved LED off/on and 10 seconds of advancing
music without new underruns passed; final preference was restored. The user
confirmed **music audible and three LEDs steady on this exact rebuilt boot**.
This is a warm kexec test, not a cold
NAND installation pass. Current client/config/cache paths remain under `/run`.

New artifacts:

- `private/connector3/installer-write/connect3.img`: SHA256
  `a9c033fb3a2ccc9ed4b5625f5289956993f4fafccd9e24422d4a42eb81d8bb6f`.
- Normal FIT `output/connector3-persistent/galdrun.itb`: SHA256
  `e9542fdcf3bef1b937ca469b9c7a821171f068d3ea640dbbb27ceb91e4087eeb`.
- Rootfs UBI SHA256
  `39b40ae949be759edea08b51e4b6bbbe8acabd48d6398fb7f6636bf2c991674b`;
  MCU helper SHA256 `3a990234ff215937afea4fcfd22b9633ea97535b2d16709adb39363d2c33d68a`.
- RAM boot inputs/report: `private/connector3/audio-led-ram-20261101/`.
  Generic kernel `09fb5868dac4ca9b88ff13c5eb49414d42745c00b678f80bd022b8e35d135078`.
  Runtime/status evidence: `reference/connector3-ssh-archive/audio-led-ram-*-20261101.*`.

The user was asked to manually replace only `connect3.img` on the existing
stick, keep all other files/folders, then physically USB-boot. Do not copy an
image over SSH: their earlier manual-copy preference still applies. Before
reinstalling, authenticate the new USB SSH key through the authorized COM5
capture, verify exact image/payload/preflight/factory protection, then take
**fresh current-NAND backups** and independent host copies. The old beta backup
on USB must be retained separately, not overwritten or reused as if current
NAND still matched it. The existing installer rejects stale backup contents.
After install/readback, qualify a physical cold boot without USB/Reset,
preserved settings/identity/LED preference and audible music. Do not mark this
candidate release-ready from the older installed-image reports.

The user selected “New USB installer booted”, but no new UART output or SSH
response has yet arrived. The user was asked to confirm power, inserted USB
and released Reset. No backup/install command has run. UART capture was
reopened as PID **34568** after verifying/stopping the old owned capture;
same log and TX queue paths. Once boot output returns, request the public host
key/model/kernel/MAC with UART marker `GALDRUN_USB_IDENTITY_20261101`.
Prepared helper `reference/connector3-ssh-archive/qualify-audio-led-installer-20261101.py`
requires that serial identity, exact new image/payload, factory fingerprints,
preflight and capsule hashes; its optional `--backup` preserves the pinned old
USB beta backup under `galdrun-backup-beta-20261031`, creates a fresh current-
NAND USB backup and independent verified host files. Do not reuse the old
20261031 install runner/pin. Installer refusal/rollback tests passed.

Corresponding sources refreshed: 659 files,
`output/connector3-sources/connector3-sources-86e60980d1ed.tar`, SHA256
`9c8f7e30efaa70e9f2ca21eb77c2ef008e72b90ed62f6594604da178896a6f19`.
The source collector now includes the exact shared
`board/connector2/status-leds.cpp` dependency; its verifier requires that file.
This archive captures build inputs, not these live handover notes. The older
658-file archive omitted the shared LED implementation and is superseded.

**USB-entry diagnosis after the user's uncertainty:** the new UART boot was
the old installed `Galdrun Connector 3` NAND system, kernel
`6.12.112-galdrun-connector3`, with the original persistent SSH pin and old MCU
timeouts. No update was running. A temporary **read-only** mount of `/dev/sda1`
verified the actual stick's `connect3.img` SHA256 is exactly `a9c033fb...` above;
both `galdrun-backup` and `galdrun-private` are intact. The mount was unmounted.
The vendor U-Boot `test_update_gpio()` samples Reset during late initialization;
the user was asked to repeat power-on with Reset already held, hold 10 seconds,
then release, judging elapsed time rather than LEDs. No NAND write occurred.

#### 2026-11-01: MUSIC AND TONES AUDIBLE — retained working state

**The user confirmed: “there is msuic now and the test tones worked too.”**
This confirms actual Galdrun client music over RCA on the current **RAM**
profile. It does not qualify the installed NAND image. Leave this working
session running; do not flash or add missing features as part of saving it.

The missing condition in today's first retest was the encoded MCU rate setup.
After the reboot, source-qualified GET `0x16` returned **`0x01000000`**, with
no rate value. The diagnostic sent **SET `0x16 = 0x0100bb80`**, accepted by
the MCU, then restarted the fixed MCU helper. Repeating the exact baseline
tones succeeded audibly, and the restored Galdrun client music was audible too.
No kernel/source/cache/sample-format changes were needed between the silent
and audible RAM tests. Do not claim that raising volume alone fixed this.

Exact retained state:

- Model `Galdrun Connector 3 RAM audio comparison`; Ethernet `10.0.0.74`;
  strict pin `private/connector3/known_hosts_ram_audio_comparison_20261031`.
- Kernel `f9ef8e2f3fdb35372e74869b839fa1da9da1baafa2de9ceaba3bb909de70f044`,
  SDMA firmware **4.6**, no NAND drivers or mounted NAND filesystems.
- Fixed MCU helper SHA256
  `3b2070fff0949a76edf237dd39ff6a8c0315ff58d4f8ee82ddebc8377ef0ce69`,
  runtime `/usr/sbin/galdrun-connector3-mcu`; source `0x100`, On, mute 0.
- Audible tones: S16_LE/stereo/48000, left 440 Hz/right 660 Hz, peak 1000,
  three 3-second bursts at hardware 67; saved/restored hardware volume **53**.
- Restored client music: **S16_LE/stereo/44100**, hardware **53**, mute **0**.
  Saved snapshot identifies **“Down By The Water”**, `local:track:28077`.
  Client SHA256 `0b32a19f3853f2e5a2facc873b6c92cafead2241608106516bd64910843cbb69`.
- Client paths remain **`/run/client.json`** and **`/run/audio_devices.json`**;
  no restricted-format cache was applied. UART capture PID **37128** remains
  active at the existing log/TX-queue paths.

**Frozen private snapshot:**
`hardware/buildroot/private/connector3/audible-music-state-20261101/`.
It contains copies of the exact kernel, DTB and private initrd plus their
SHA256 manifest, client configuration/cache/status/log, MCU log, runtime
readbacks and successful rate/tone evidence. Preserve it: its initrd contains
private settings and an SSH host key; it is not a public release artifact.

The MCU SET used 48000 while the subsequently audible client music used
44100. This proves the successful initialization sequence, **not** a rule to
hardcode 48000 for every production stream. Determine the correct startup/rate
integration from the existing hardware protocol before persisting a fix.
The installed NAND helper still lacks the sync fix; the installed kernel still
has its earlier SDMA provenance. No new NAND write was performed.

Exact successful commands, from the checkout root, **on this RAM profile**:

```powershell
wsl.exe -d Ubuntu -- python3 hardware/buildroot/reference/connector3-ssh-archive/inspect-persistent-audio-20261031.py rate-48000 --ram
wsl.exe -d Ubuntu -- python3 hardware/buildroot/reference/connector3-ssh-archive/test-ram-baseline-tone-20261031.py
```

The first command serializes UART ownership by stopping/restarting the MCU
helper; the second temporarily releases ALSA and restores the client/volume.
Do not rerun them merely to preserve this now-working session. Read-only
snapshot helper: `test-client-music-20261101.py --ram --save-state` in the
same private reference archive. All following checkpoints are earlier history.

#### 2026-11-01: client music test on current NAND boot

The device has rebooted since the RAM-tone checkpoint below. Strict SSH
verification matched the existing **persistent** pin, not the RAM pin; model
`Galdrun Connector 3`, read-only NAND root, client PID 93. Do not use the old
RAM-profile restart paths for this boot. The room is still named
`Connector 3 RAM test`, client ID `c170ac6d4af8eddd`.

The installed MCU helper again timed out. The previously verified helper
`3b2070fff0949a76edf237dd39ff6a8c0315ff58d4f8ee82ddebc8377ef0ce69` was
uploaded to **`/run/mcu-sync-fixed-20261101`** and started with exclusive UART
ownership after stopping the installed helper. Its ARM self-test, application
sync, source `0x100`, power On and unmuted readbacks passed. The installed
binary and flash images were not replaced. `/run/mcu-buttons.pid` now names
the temporary helper; its log is `/run/mcu-buttons-fixed-20261101.log`.

Actual client music: **“Only Happy When It Rains”**, `local:track:18252`,
**S16_LE stereo 44100 Hz**, `hw:CARD=Connector3,DEV=0`, MCU hardware volume
**67/100**, mute **0**. No sample-rate SET was sent in this session.
The previous MCU timeouts coincided with late starts and accumulated underruns;
one REST room `rejoin` was requested after restoring MCU operation.
Three status snapshots spanning 20 seconds then showed advancing packets,
frames and audible timeline, 987–1417 ms buffered, and **zero additional**
backend/timeline underruns or skipped frames. Old totals remain nonzero
(backend 7824, timeline underrun 3450109, skipped 3446856); do not call them zero.

**The user confirmed this NAND music test is still silent.** Stable software
progress did not establish an audible music pass. The next comparison returns
to the previously audible RAM profile. Keep missing-feature work deferred
until client music is confirmed audible.

Local evidence/helpers under `hardware/buildroot/reference/connector3-ssh-archive/`:
`test-client-music-20261101.py`, `client-music-20261101.txt`,
`client-music-status-20261101.json`, `client-music-snapshots-20261101.json`.
The helper's `--inspect` mode sets hardware volume 67 and captures three
snapshots; its default mode expects the original installed MCU helper running.

The retained RAM kernel `f9ef8e2f...` was then booted with the existing helper
`boot-ram-audio-comparison-20261031.py`, without NAND drivers. Upload hashes
passed; strict RAM SSH and model checks passed. The new private-overlay hash
is `0a80dc300c0803b7c0713b953c73ab50750b0addde8e194c834b191adc8b6993`;
kernel/DTB hashes are unchanged. UART capture PID **37128** appends to the
existing `fit-update-console-20261031.log`, with the existing TX queue path.

RAM client music **“Jennifer's Body”**, `tidal:track:18256345`, was likewise
S16/44100/stereo at hardware 67, mute 0. Three snapshots showed 20 seconds of
progress without additional underruns (backend total 45, timeline 19767).
**The user confirmed this RAM client music is also silent**, and requested
test tones first. Evidence: `ram-client-music-20261101.txt` and
`ram-client-music-snapshots-20261101.json` in the same private archive.

The exact S16/48000 baseline helper was repeated: three 3-second stereo
440/660 Hz bursts, peak 1000, hardware 67, mute 0. All three `aplay` runs
passed, and the client restarted with `/run/client.json` and
`/run/audio_devices.json`, restoring volume 67/mute 0. Listening result is
pending. This rerun refreshed the helper's old fixed-name tone log; its
dated copy is `ram-baseline-tone-20261101.txt`. The original audible test's
saved/restored volume was **53**, as documented below; the rerun's was **67**.

#### Latest result: RCA tones AUDIBLE — save/continue here

**The user confirmed “Tones are audible” for the exact earlier baseline test.**
This is the first audible result in the current silence investigation. It ran
on the retained pure RAM profile described below, not the installed NAND image.
Music playback is **not yet confirmed audible**. Do not lose or overstate this.

Exact successful test:

- ALSA device: `hw:CARD=Connector3,DEV=0`.
- Raw **S16_LE**, stereo, **48000 Hz**.
- Left sine **440 Hz**, right sine **660 Hz**; each peak **1000 / 32768**
  (about -30.3 dBFS), generated with `round(1000 * sin(2*pi*f*i/48000))`.
- **Three 3-second tones**, with 1 second between iterations.
- MCU **hardware volume 67/100**, hardware mute **0**. Verified socket readback
  printed `MCU mute=0 volume=67` before playback.
- MCU power On, source **`0x100`**. The preceding encoded rate command
  **`SET 0x16 = 0x0100bb80`** had been accepted, and source-qualified
  **`GET 0x16` with request value `0x01000000` returned `0x0100bb80`**.
  Whether that rate command was necessary has not been isolated.
- The client was gracefully stopped to release ALSA. The MCU helper stayed
  running; socket requests serialized its UART ownership. No OEM code ran.
- An EXIT trap restored the saved **volume 53, mute 0**, then restarted the
  client using **`/run/client.json`** and **`/run/audio_devices.json`**.
  The test process exited **0** and logged all three successful `aplay` runs
  plus both restoration readbacks. No NAND access/write occurred.

Repeatable diagnostic (ignored local helper, run from this checkout):

```powershell
wsl.exe -d Ubuntu -- python3 hardware/buildroot/reference/connector3-ssh-archive/test-ram-baseline-tone-20261031.py
```

This script asserts the current RAM model and uses the strict RAM SSH host-key
pin. It uploads only the raw tone and the existing MCU socket diagnostic to
`/run`. The tone command itself is:

```sh
aplay -D hw:CARD=Connector3,DEV=0 -t raw -f S16_LE -c 2 -r 48000 /run/baseline-tone.raw
```

Evidence: **`hardware/buildroot/reference/connector3-ssh-archive/ram-baseline-tone-20261031.txt`**.
Script source: `reference/connector3-ssh-archive/test-ram-baseline-tone-20261031.py`.
Socket tool source: `reference/connector3-ssh-archive/mcu-audio-query.cpp`, linked
with `client/src/audio/linux/Connector3Mcu.cpp`. It now accepts an optional
single argument such as `'volume 67'` / `'mute 0'`; without arguments it queries.
The current ARM tool is already compiled in that same ignored archive.

**Next controlled comparison:** test music at hardware 67 while retaining its
current format, then compare direct S16 and S32 tones at the same physical
level if needed. The successful test changed **both format and level** versus
the failed quiet S32/volume53 tests, so the cause is not yet isolated. Do not
claim a sample-format defect, a volume-mapping defect, a rate fix, or a NAND
listening pass yet. Current restored music may be S32/48k/volume53; read back
live state first. Do not immediately repeat kernel resets or NAND flashing.

The user reports **no RCA audio** on the cold NAND boot, despite PCM/stream
progress. Software mute was off. Initial MCU UART RX was zero; power/unmute
requests timed out. GET Sync (`type 1`, command `0xfffc`, value 0) immediately
returned ACK plus AppSyncResponse `0xfffd`. This restored actual MCU readback,
hardware room volume and `getDac.supported=true` after `rescanAudio`.
Socket existence alone did not establish working MCU/audio initialization.

`board/connector3/ram/mcu-buttons.cpp` now synchronizes before audio setup,
handles BootloaderSyncResponse `0xfffe` by Notify `0x000c` (start the existing
verified MCU application, no erase/program), mutes while selecting the prior
audible input `0x100`, then verifies On/unmuted. Its native and ARM/rootfs
self-tests passed, including unsynchronized firmware and wrong saved input.
Current helper SHA256 `3b2070fff0949a76edf237dd39ff6a8c0315ff58d4f8ee82ddebc8377ef0ce69`.
The persistent rootfs builder now compiles that helper from current source,
instead of silently inheriting it from an older RAM archive. **This userspace
fix has NOT been installed in NAND.** It has passed runtime startup in RAM.

Mute/volume, restoring input `0x201` to the previously audible `0x100`, a
documented GPIO1_8 DAC reset pulse with MCLK enabled, audio Standby->On, and
quiet direct 48 kHz S32_LE ALSA tones **did not restore audible RCA output**.
Do not record these attempts as listening passes. The user confirmed RCA OUT.

The known audible RAM boots loaded SDMA firmware 4.6; NAND boots requested
it before root mount and fell back to ROM firmware. The persistent builder
and verifier now embed/audit `imx/sdma/sdma-imx7d.bin` alongside radio/regulatory
firmware. New candidate FIT:
`b9eabc8ad4fdf4a46a25292ae0ba1b34acbd6bd54368ef3c1cea81d084eb77fa`,
kernel `7eb77ee627e861f56cd0ca9e80657314f6a21cffef3559223b9187d19107a745`,
exported rootfs `81e5ddcaf3c0b2ea6616085e18c98aaf16ac79a63caa52786ba23c8a61be766f`.
Signature/tamper, actual embedded bytes and ARM MCU/rootfs checks passed.
Protected RAM boot (`private/connector3/sdma-audio-test/`) loaded SDMA 4.6 and
the fixed MCU helper, **but the user still heard no direct tones**. Neither
SDMA nor source selection alone has established the remaining silence cause.
**Installed NAND FIT remains `5eed3ce8...`; no new NAND writes were performed.**

Do **not** try live SDMA unbind again. Even after audio/SAI released its DMA
channels, `sdma_remove -> sdma_free_chan_resources` hit a NULL dereference and
left device shutdown/kexec blocked. SysRq restart also stuck. Settings were
synced/read-only; the user recovered with a physical power cycle. Evidence:
`persistent-audio-dma-reload-20261031.txt` and
`persistent-audio-handoff-state-20261031.txt` under the private reference archive.

**Current active test:** retained RAM audio profile kernel `f9ef8e2f...` booted
with a private settings/SSH overlay and the fixed MCU helper, without NAND
drivers. Model `Galdrun Connector 3 RAM audio comparison`, Ethernet `.74`,
separate pin `private/connector3/known_hosts_ram_audio_comparison_20261031`.
Its settings are `/run/client.json` and `/run/audio_devices.json`, not
`/settings/`. Do not use persistent-path restart commands in this RAM profile.
At the checkpoint it was playing with hardware volume 53/100, mute off and
zero underruns. **The user confirmed this RAM comparison is also silent.**
The same amplifier input and RCA cables play another source correctly (user
confirmed). The external listening path is therefore checked; do not ask again.
Artifacts/helper: `private/connector3/ram-audio-comparison-20261031/` and
`reference/connector3-ssh-archive/boot-ram-audio-comparison-20261031.py`.
Capture PID **41444** owns COM5, appending to
`private/connector3/fit-update-console-20261031.log`; its original TX queue path
remains active. Previous PID 16968 is stopped. Local server/core are now
running at the user's initiative; avoid stopping them for MCU/kernel-only work.

Next: use the **audible baseline at the top of this section** to isolate format
versus hardware level; persist only validated
fixes with independent backups, then cold-boot/audio-control/listening checks.
Refresh USB/rootfs exports and source bundle after final source changes. Existing
exports/source bundle below predate these MCU/SDMA fixes. Old qualified 5eed3ce8
inputs/reports are frozen in `private/connector3/persistent-installed-5eed3ce8/`;
the output directory's old hardware report was renamed to
`hardware-verification-installed-5eed3ce8.json` so it cannot qualify candidate b9.
No further raw MTD4 authorization for candidate b9 has been requested/granted.

#### Latest protocol findings and exact continuation

Do not infer that `SampleRate` (`0x16`) needs a SET: both GET and SET 44100
were rejected with MCU status 4. No rate-setting change was added. Channel
configuration GET `0x12` rejects with status 3; input detect GET `0x1e` rejects
with status 4. Digital input format GET `0x1b` returns 0. These results do not
prove a DSP/rate fault or a writable control.

`McuAudioInputSource` is a **two-byte composite**, not yet fully interpreted.
The retained reference `libmcu.so` `fromCommandValue` at `0xe29c` accepts each
byte only in 0..5, placing the high byte in struct byte 0 and low byte in byte 1.
JSON names are `som` for the high byte and `amp` for the low byte.
Reference `DefaultMcuHardwareHandler::onAllDataReceived` at `0xea458` explicitly
sends SET `0x15 = 0x100`, confirming our source selection. Avoid guessing values.
Trace the remaining startup controls in the locally retained
`reference/connector3/mcu-reference/usr/lib/libmcu.so` and
`raumfeld/hardwared/hardwared`. Reference inspection only; do not run OEM code.
`reference/connector3-ssh-archive/inspect-mcu-rate-20261031.py` currently prints
that disassembly, but its broad strings output can truncate useful details.
`libmcu.asm` is UTF-16, not UTF-8.

**New lead:** `McuHardwareHandler::doSetProperty(SampleRate)` at `0xea38c`
ORs **`0x01000000` into the rate** before SET `0x16`. The earlier unencoded
44100 rejection does not test this documented form. The ignored runtime probe
tested `0x0100ac44` (44100): **SET accepted**, unlike the unencoded form.
Mute was restored off and the fixed helper automatically restarted. The user
has been asked whether RCA music is now audible; listening result is pending.
Evidence: `reference/connector3-ssh-archive/persistent-audio-rate-44100-20261031.txt`.
Do not add a production sample-rate setter or claim this restores sound without
listening evidence. Read-only ALSA/clock inspection then found **48 kHz**
(S32_LE stereo, MCLK 24.576 MHz), so a second test sent **`0x0100bb80`**.
The MCU accepted this matching 48000 setting too; helper restart and mute off
passed. **The user confirmed both 44.1 kHz and matching 48 kHz tests are still
silent.** A source-qualified GET `0x16` with request value `0x01000000` then
successfully returned **`0x0100bb80`**. Earlier GET failures used selector zero.
This verifies the MCU rate setting, but does not resolve physical silence.
Evidence: `persistent-audio-rate-48000-20261031.txt` and
`persistent-audio-clocks-20261031.txt` in the same ignored reference archive.

Pinned diagnostics for the **current RAM boot**:
`wsl.exe -d Ubuntu -- python3 hardware/buildroot/reference/connector3-ssh-archive/inspect-persistent-audio-20261031.py sync-mcu --ram`
(run from this checkout, WSL escalation needed). `--ram` chooses the separate
RAM host-key pin. Read the script before using other modes: many mutate state
or assume persistent `/settings` paths. The query temporarily stops the owned
MCU helper and traps restart of `/usr/sbin/galdrun-connector3-mcu`; never access
`/dev/ttymxc2` concurrently with that helper. Diagnostic logs and scripts are
under the ignored reference archive, not tracked release content.

The exact old audible kernel `92f0e4a...` has not been located; its artifacts
were overwritten. Current comparison uses retained `f9ef8e2f...`, not that
exact binary. Current and persistent caches have identical machine audio,
SAI, SDMA and clock-driver sources. GPR1 difference bit 12 is labelled
`IRQ_MASK` in the kernel header; do not blindly copy the old register value.
Keep saving new findings here before context limits; the unresolved target
remains **audible RCA playback**, not merely successful MCU/ALSA readback.

### Completed network/FIT qualification (before the audio report)

**Latest: the authorized MTD4-only Wi-Fi FIT update passed exact readback.**
Installed FIT is now `5eed3ce8912b83ca9be44d6829a66a60cb67685f9d7eaed6efd683e04336957d`.
MTD0 remained protected; MTD1-MTD3 and MTD5 hashes stayed unchanged.
**Physical NAND cold boot of this FIT passed, without USB or Reset.** The
32768 Hz CLKO2 clock, automatic p14 firmware loading, AT regulatory state,
read-only root/FIT/factory protection, writable settings, preserved client ID,
persisted SSH host key, client API and native MCU checks all passed.
Wi-Fi-only DHCP/pinned SSH at `10.0.0.58` passed with Ethernet administratively
down, a WLAN default route and **30/30 gateway pings, 0% loss** over about
29 seconds. Ethernet SSH at `10.0.0.74` was then restored and verified.
No flash or Wi-Fi handoff process is running. No new listening/deep-sleep test.
Owned UART capture PID 16968 was stopped after archiving; COM5 is released.

Current evidence under `reference/connector3-ssh-archive/`:
`fit-only-update-20261031.json`, `persistent-wifi-cold-boot-20261031.json`,
`persistent-wifi-only-20261031.json`, matching runtime/UART logs, and
`persistent-wifi-final-hardware-20261031.json`. The joined report is also
`output/connector3-persistent/hardware-verification.json`. It deliberately
distinguishes actual installed rootfs plus init fix from unflashed exports.
`persistent-final-runtime-20261031.txt` confirms the final Ethernet default
route, USB absence, normal partition protection/zero ECC failures, MCU and
completed recovery timers. Full UART is `persistent-wifi-final-uart-20261031.log`.

**Galdrun is installed on NAND and boots normally.** MTD4/5 installation and
readback passed, the original FIT/rootfs were restored with matching readback,
then Galdrun was reinstalled successfully. MTD0 was never writable; corrected
MTD1–MTD3 fingerprints stayed unchanged. The original beta was not booted between
rollback and reinstall. No flash process is running.

**Earlier cold NAND boot of FIT `2bfb58b7...` passed.** The user powered off,
removed USB and powered on without Reset. `verify-persistent-boot-20261031.py cold`
passed root/FIT/factory protection, writable settings, persisted SSH host key,
client identity/API and native MCU checks. Evidence is
`reference/connector3-ssh-archive/persistent-cold-boot-20261031.json`,
`persistent-cold-runtime-20261031.txt` and `persistent-cold-uart-20261031.log`.
Do not repeat earlier pending-cold-boot instructions.

Normal kernel: `6.12.112-galdrun-connector3`, Ethernet `10.0.0.74`, MAC
`50:1e:2d:3b:77:f4`. Warm NAND boot passed read-only root/FIT/factory protection,
writable settings and saved `/etc`, owner-key SSH, preserved client identity,
client API, native MCU socket and ALSA Connector3 registration. SSH host keys
survived subsequent reboots and both physical cold boots. Wi-Fi PCIe failed in
the older warm/cold normal boots; the completed clock/firmware fix is below.
Do not reopen the approved radio decision. Audible
volume/mute remains deferred by the user; no new listening test was done.

First NAND boot exposed missing `/dev/pts` and a read-only PID directory. The
startup script now handles kernel-mounted devtmpfs, creates devpts and binds
`/run` onto `/var/run`. It was applied only to MTD5, rebooted and UBIFS recovery
completed; root read-only state, daemon PID file and interactive SSH passed.
Runtime init SHA256:
`1f080a83b3ad21db37485c26c037710a833c074e3f61a3e86938835221a93ecf`.

Previous installed FIT SHA256 (superseded by the MTD4-only update above):
`2bfb58b7b343162219f913de777d01fee110bce1175e9390538398b639cb89a3`.
Initially flashed rootfs container: `480ae910...`; its init was then updated as
above. Current exported rootfs `f849093d...` includes the fix, but that new
container has not itself been flashed byte-for-byte. Keep installed-input versus
exported-container evidence distinct. The detailed hashes and qualification scope
are in [the current checkpoint](connector3.md#persistent-installer-checkpoint--2026-10-31).

The first install attempt refused before erase because raw MTD0 reads vary by a
bit under the unqualified ROM ECC layout. The corrected installer keeps MTD0
read-only and uses exact corrected MTD1–MTD3 fingerprints. Its SHA256
`0ab27f34b54b34ca0f9554e9a3bd12c147e4dfe5aab3a00dec18251400f238c7`
was tested in RAM for install/readback, rollback/readback and reinstall. Never
restore MTD0–MTD3. The limited monitor/PSCI disables CAAM and all TEE/RNG services;
its exact qualified hash is `2b84ee7b...`. All factory CAAM rings are locked;
trying another ring or bypassing ownership checks is not the next step.

USB backups and independently hash-verified host copies are retained at
`private/connector3/rollback-usb-20261031/`. Manifest pin:
`3ce972d6b4e8c4d081a91002856e423315a4b7b97312b6d8b9d0275ba4b2693e`.
The first inconsistent MTD0 read remains `mtd0-first-read.bin`, unqualified for
restore. Private settings/public key were staged separately in `galdrun-private/`;
generic release exports remain SSH-off with no home credentials or developer key.

The user chose **manual image copying** after beta USB write trouble. Do not
resume image-copy/USB-reset troubleshooting over SSH. Later pinned SSH backup
reads and small settings/script transfers worked. The current stick still has
the tested write image `f3223bea...` and both private folders; keep them. Frozen
tested images/reports are under `private/connector3/installer-*-tested-*`.
Corrected export images are built under `private/connector3/installer-check/`
(`dc131cd4...`) and `installer-write/` (`45fcca78...`); their manifests are
authoritative. They include both startup/installer fixes. Do not claim those
new containers were cold booted from old-image evidence. At that checkpoint
they contained normal FIT `2bfb58b7...`/rootfs `07279cfc...`; the newer Wi-Fi
exports recorded before the next steps below now supersede them.

COM5 RX/TX is authorized. Old capture PID 29900 expired; its
`private/connector3/persistent-console-continue-20261031.log` and earlier logs
are retained. The resumed capture is described in the workflow below.
Normal NAND SSH uses `private/connector3/known_hosts_persistent_20261031`;
its key passed the new cold boot too. Never bypass host-key checking.
Local Galdrun servers/core remain stopped.

### Wi-Fi clock investigation (completed)

The normal cold boot had `Phy link never came up`. Its CLKO2 sleep clock was
disabled at 24 MHz, while the working vendor clock snapshot has an enabled
32768 Hz CLKO2. GPIO power/reset and pinmux matched the working board setup.
Snapshot: `reference/connector3-ssh-archive/persistent-cold-pcie-state-20261031.txt`.

Source changes add the `wifi-32k` PCIe clock, assign CLKO2 to `ckil`/32768 Hz in
the shared RAM DTS, and enable it before PCIe initialization using
`devm_clk_get_enabled` in new patch
`board/connector3/ram/patches/linux/0012-connector3-wifi-sleep-clock.patch`.
Other boards are unchanged. These changes are now installed in NAND and
cold-boot qualified in FIT `5eed3ce8...`.

`build-wifi-clock-test-20261031.py` built a RAM test, then
`boot-wifi-clock-test-20261031.py` uploaded/hash-verified it and used kexec.
UART confirms the new kernel booted and **PCIe Gen1 linked and enumerated
Marvell endpoint `11ab:2b38`**. The built-in driver requested firmware before
the NAND root mounted and got `-2`. A sysfs unbind/bind after root mount loaded
the approved p14 firmware (`wifi-clock-reprobe-20261031.txt`). The follow-up
kernel below embeds radio/regulatory firmware and fixes this startup ordering.
At this earlier RAM checkpoint the installed FIT was still `2bfb58b7...`,
with MTD0-MTD4 protected; the later MTD4-only update is recorded below.

RAM artifacts: `private/connector3/wifi-clock-test/`:

- `zImage`: `a87dee44ef939dfe2a4905b77dc054cbb6e0626b22e2e3ffc33e72b66053e000`.
- `connector3.dtb`: `ad81ca67aa8825df5bbecc34074235e8b34e9eacef9eed52133ba89864734a19`.
- Model: `Galdrun Connector 3 Wi-Fi clock RAM test`; release remains
  `6.12.112-galdrun-connector3`. Ethernet MAC is explicitly injected for kexec.

This first RAM test has been superseded by the firmware test below. The old
kexec sessions 45376 and 54319 ended; **do not rerun a handoff for its output**.
Build log: `hardware/buildroot/logs/connector3-wifi-clock-test.log`.
The persistent Linux cache `connector3-persistent/linux-f9ef8e2f3fdb3537` was
modified by the tests and then rebuilt by the production builder.

### Firmware RAM test checkpoint (before the completed NAND update)

`build-connector3-persistent.sh` now copies the current shared RAM DTS,
idempotently applies patch 0012 to older caches, and embeds the approved radio
plus `regulatory.db`/`.p7s` using standard `CONFIG_EXTRA_FIRMWARE`. The verifier
checks actual embedded bytes against the audited rootfs, 32768 Hz clock
assignment, partition protection, signature and tamper rejection. All passed.
The new candidate at `output/connector3-persistent/` is:

- Signed `galdrun.itb`, 5685908 bytes:
  `5eed3ce8912b83ca9be44d6829a66a60cb67685f9d7eaed6efd683e04336957d`.
- `zImage`: `5cbd9cf90bbaa13a30a6a1de5a98669a8d682188fa712e20a1c71dfead6d8fbd`.
- Unsigned `connector3.dtb`:
  `357c2fa6228ba00ab0e7e8983c4219ca880b57f2862eb01e125b49031da19a0d`.
- Exported `rootfs.ubi`:
  `f849093dfbce8e21be5ba0b6ec7cf3cb67d3e78df6ae946f182721c67e96f8e5`.
  UBIFS containers change on regeneration; init source remains `1f080a83...`.
- Monitor remains the qualified `2b84ee7b...`; no monitor rebuild needed.

`prepare-wifi-firmware-test-20261031.py` copied this exact kernel and changed
only the test DT's model/MAC. `boot-wifi-clock-test-20261031.py firmware` RAM
booted it successfully. Current model:
`Galdrun Connector 3 Wi-Fi firmware RAM test`.
Test DT SHA256:
`e4b2320b3cb98d67495dc30c5b7685220522d8f66dc636ffddae2843c423cd7b`.
That RAM test used NAND root/settings and pinned persistent SSH at `.74`,
with installed FIT still `2bfb58b7...`. The board now cold-boots updated
FIT `5eed3ce8...` from NAND, with USB physically removed.

UART confirms automatic 673220-byte firmware download, p14 active, `mlan0`,
and association; the isolated boot is `wifi-firmware-ram-uart-20261031.log`.
`wifi-clock-firmware-20261031.txt` confirms CLKO2 enabled at
32768 Hz, regulatory country AT, root read-only, settings writable, MTD0–4
read-only and the original installed FIT hash unchanged. Wi-Fi later disconnected
with reason 3 while Ethernet was ready, consistent with the network service
preferring Ethernet. **Do not claim sustained Wi-Fi/DHCP or Wi-Fi-only traffic
from this short association.** No firmware/regulatory-file error appeared.

Original exported inputs and hardware report are retained at
`private/connector3/persistent-installed-2bfb58b7/`. Its exported rootfs is
`07279cfc...`; actual NAND was installed from `480ae910...`, then only its init
was patched to `1f080a83...`. Preserve these evidence distinctions.

### FIT-only preparation and refreshed exports

The qualified existing USB write image and COM5-authenticated SSH session were
used for the **successful authorized MTD4-only update** to FIT `5eed3ce8...`.
Exact readback passed. MTD0 remained protected; MTD1-MTD3 fingerprints matched.
MTD5 SHA256 before and after was identical:
`6d1a3b555084728795b7b132610ba7f851576eca3812237e1b1c08952e7c398f`.
Evidence: `reference/connector3-ssh-archive/fit-only-update-20261031.json` and
matching `.log`. The original Galdrun FIT backup remains pinned on the host.
Physical power-off boot without USB/Reset and Wi-Fi-only DHCP/SSH/traffic
qualification both passed. No flash process is running. Earlier approval
rejection was resolved by explicit user consent.

Both refreshed exports now include candidate FIT `5eed3ce8...`, exported rootfs
`f849093d...`, the corrected installer/startup and `/usr/sbin/galdrun-update-fit`.
Build, actual embedded-source/content checks, required signatures, tampered
signature rejection and profile protection checks passed:

- `private/connector3/installer-check/connect3.img`:
  `5deddcb03e5ffd179bbd69fa17d483dd4af63543595109c8a36aaed352f2044f`.
- `private/connector3/installer-write/connect3.img`:
  `43835e8b60a5abca7fc28b7fe1a2c4ad7aed016a26fde206c07e2407b394d68f`.
- Authenticated appended payload SHA256:
  `5c52f9b35dcf4c6a37cc3a8850c3f745c1e332492de8b05e1ce5bca252a81b19`.

These exact new containers are **not** physically boot-qualified. The existing
stick remains `f3223bea...` and was not overwritten. Frozen tested images and
old installation/cold-boot evidence are retained separately.

Completed FIT update workflow:

1. The **MTD4-only** helper is ready at
   `hardware/buildroot/board/connector3/installer/update-fit.sh`. It reuses
   the audited installer preflight/factory checks and pinned USB backups,
   saves current MTD4, requires its independently verified host hash before
   erase, checks exact readback, and restores that saved Galdrun FIT on failure.
   MTD5 is hashed before/after and never passed to a writer. Mocked refusal,
   successful write, failed write and failed-readback rollback checks passed:
   `python3 hardware/buildroot/scripts/test-connector3-fit-update.py`.
   Host orchestration is `reference/connector3-ssh-archive/update-fit-only-20261031.py`
   (`prepare` then `write`), with exact old/new FIT, USB and factory-backup pins.
   It rechecks the actual candidate signature using the existing diagnostic
   public control tree; this passed independently. `prepare` has now passed:
   current MTD4 is independently saved at
   `private/connector3/fit-update-20261031/current-mtd4.bin`, SHA256
   `c3818bc5d261c5aa7ce378798a555750763a4e48a8c6b3b88b7f63c912adfe53`.
   Its leading 5274472 bytes match the installed `2bfb58b7...` FIT. All pinned
   factory backups and the corrected preflight/factory checks passed.
   `inputs.json` records all operation pins and helper hashes.
   The user explicitly authorized this exact MTD4 operation after the first
   automatic approval rejection. `write` then passed with exit code 0 and
   exact FIT readback; MTD0 protected and MTD1-MTD3/MTD5 unchanged.
   Do not run the old full installer: it replaces MTD5 and expects original
   beta partition hashes. Its old host helper also refers to a hardware report
   that has since been moved into the frozen tested-image directory.
2. **The existing stick (`f3223bea...`) has been physically booted again.**
   UART confirms the qualified USB write model/kernel; Ethernet MAC, actual
   stick hash, preflight and pinned backups passed. The ephemeral USB key was
   authenticated against COM5 and enrolled separately in
   `private/connector3/known_hosts_fit_update_usb_20261031`; the persistent NAND
   SSH pin is unchanged. No image copying was needed.
   Old capture PID 29900 expired. Resumed authorized COM5 capture PID 16968 wrote
   `private/connector3/fit-update-console-20261031.log`, accepting JSON `text`
   at `private/connector3/fit-update-uart-tx-20261031.json`; four-hour limit.
   Its script is `reference/connector3-ssh-archive/capture-fit-update-20261031.ps1`.
   It captured the USB write session, NAND cold boot and Wi-Fi-only retest.
   PID 16968 was then stopped and COM5 released; the log/queue files remain.
   Final archive: `reference/connector3-ssh-archive/persistent-wifi-final-uart-20261031.log`.
3. **MTD4-only write/readback is complete.** Write-profile model/kernel,
   no installed UBIFS/UBI attached, geometry, protected factory fingerprints,
   backup pins and new FIT signature/hash passed. Never write MTD0-MTD3 or
   bypass the normal/RAM kernel's MTD4 protection.
4. Physical power-off boot without USB/Reset **passed** the new
   Wi-Fi clock/firmware, root/factory protection, settings/client ID/SSH host
   key/API/MCU checks. The verifier has a deliberately pinned `wifi-cold` phase
   that requires successful FIT-only update evidence, preserves old reports,
   and checks automatic p14 firmware, enabled 32768 Hz CLKO2 and AT regulatory
   state. It reuses the persistent SSH pin and checks original rootfs provenance.
   `verify-persistent-wifi-only-20261031.py` separately passed Wi-Fi-only DHCP,
   pinned SSH and 30/30 WLAN gateway pings with Ethernet down, then verified
   Ethernet recovery. The DHCP deconfig hook re-enables Ethernet, so the helper
   holds it down through supervisor cleanup before collecting evidence; no
   saved network configuration or firmware was changed for that test.
   Listening/deep sleep remain deferred.
5. The refreshed exports already contain the Wi-Fi candidate as recorded above.
   Rebuild only if final qualification requires a source/input change. Keep
   exact container cold-boot claims separate from older tested-image evidence.

Source bundle refreshed after current code changes and verified:
`output/connector3-sources/connector3-sources-988a5630a0f7.tar`, 656 members,
SHA256 `60a386772eddba3d9d1eebeceafdf2e9cc142cecac6dc83264026b1080495cc3`.
All member hashes/private-path exclusions passed. Persistent-init regression
checks and `git diff --check` passed (only the existing CRLF warning).
Regenerate the source bundle if source changes during FIT-only work.

Private evidence/helpers under `reference/connector3-ssh-archive/`:

- `install-retry-20261031.*`, `rollback-20261031.*`, `install-final-20261031.*`:
  all successful operations and exact script/pin hashes. Initial refusal is
  separately preserved in `install-initial-20261031.*`.
- `installed-persistent-inputs-20261031.json`: frozen original installed inputs.
- `persistent-first-boot-20261031.json`, `root-maintenance-20261031.txt`:
  warm boot and fixed startup/SSH evidence.
- `persistent-cold-boot-20261031.json`: physical NAND cold-boot checks passed.
- `verify-persistent-boot-20261031.py`: existing first/cold verifier pins the
  original installed FIT; update its expected inputs deliberately for a new FIT.

FIT-only update, NAND cold Wi-Fi qualification and refreshed artifacts are
complete within the scope above. The exact new USB/rootfs containers remain
physically unqualified; do not transfer old container evidence to them.
Preserve unrelated dirty server/Tidal/mobile-GUI work. No commit, reset or
cleanup was requested. All sections below are earlier history/constraints.

## Start with the user's final firmware decision

**Use the working Marvell/NXP hardware firmware blob `15.87.19.p14`.**
The user explicitly instructed:

> add in the handover that we will usee this working blob. The version missmatch for the licence is we dont care

Do **not** keep searching for a different firmware or make an exact licence-
version match a prerequisite. The mismatch is known and explicitly accepted by
the user. Keep the available manufacturer licence notice with the hardware
firmware, as with Connector 2. Do not claim the short check verified an official
download or an exact-version licence; it did not. This decision authorizes
using this specific working hardware blob, not copying the vendor rootfs/apps.

```text
Version: 15.87.19.p14
Size: 673220 bytes
SHA256: b2c5e7f4f88b061b92be8113ddd5ec688da94729c3a5d07bec9e334335499538
Local source: hardware/buildroot/private/connector3/pcie8897-working-reference.bin
Runtime destination: /lib/firmware/mrvl/pcie8897_uapsta.bin
Embedded identity: $Id: w8897o-B0, RF87XX, FP87, 15.87.19.p14 $
Additional embedded branding: Marvell Micro AP
```

The earlier assumption that extraction from the device makes it Raumfeld code
was too strict. Manufacturer hardware microcode is compatible with the
Connector 2 approach. Continue excluding Raumfeld/OEM OS files, applications,
daemons, proprietary libraries and the vendor-dependent D-Bus bridge.

**Implemented 2026-10-31:** `hardware/buildroot/scripts/connector3_content.py`
allows this exact digest at the radio path with its unchanged manufacturer
notice and source record. OEM-runtime checks remain active.
`verify-connector3-ram.py` now audits explicit local-radio builds too and marks
this approved blob eligible for clean-installer content. Its attribution remains
`local-device`, and the source record preserves the accepted licence mismatch.

## Verified end-of-day state

- Board is back on the **unchanged original beta `2.12-snapshot-7`, kernel
  `4.1.45`**, verified by UART and the existing trusted SSH connection at
  **`10.0.0.58` over WLAN**. Ethernet `.74` was not responding in beta.
- No custom firmware was installed. No NAND erase/program, bootloader update,
  environment repair or `saveenv` was performed.
- No RAM experiment remains active. UART capture PID 35136 was stopped and
  COM5 released. The final log is saved at
  `hardware/buildroot/reference/connector3-ssh-archive/clean-wifi-end-of-day-20261030.log`.
- Local Galdrun server/core/client are stopped. No owned Connector 3 compiler
  processes remain. Do not restart a stack just because an older checkpoint
  says music was running.
- For recovery after any later failed RAM boot: remove the USB stick and power
  cycle normally **without Reset**. Use the beta's original known-hosts file;
  never globally disable SSH host-key checking.

## Results worth keeping

1. Signed source-built USB diagnostic **cold boot, Ethernet DHCP and key-only
   SSH passed**. `output/connector3-diagnostic/connect3.img` SHA256:
   `1e117314f90c1692df5c9f274ba34f3e608aac2ca66c54d9c57592bdf188bdb2`.
   It is RAM-only and cannot install anything.
2. New-kernel **NAND geometry/BCH8 reads, UBI read-only attach, UBIFS read-only
   mount/unmount, and USB/FAT32 stick reads passed**. All six MTD flags were
   `0x0`; original environment/constants/FIT hashes matched; zero uncorrectable
   ECC errors. These were protected kexec tests, not storage cold-boot or
   erase/program qualification.
3. Geometry is 512 MiB SLC, 4096-byte pages, 256-byte OOB, 256 KiB eraseblocks,
   BCH8/512. UBI VID offset 4096, data offset 8192, LEB 253952. Preserve bootloader,
   env, constants and settings partitions. The FIT is MTD4, rootfs MTD5; do not
   reuse Connector 2's partition indices or image format.
4. The old UBI allocation produced `reserved 25, need 34` under the modern
   kernel. Leave enough bad-block reserve when creating a new UBI layout.
5. Standalone RAM packaging now omits `galdrun-connector3-buttons`, which
   depends on the vendor D-Bus service. Its explicit historical vendor-test
   helper builds/uploads that bridge separately. Galdrun's own direct MCU
   service remains in the standalone image.
6. The rebuilt generic RAM archive passed content/source checks: 22 packages,
   81 ELF files, no vendor runtime bridge or saved home Wi-Fi credentials.
   Keep those content checks, allowing the specifically approved radio blob.

The known audio kernel remains:
`f9ef8e2f3fdb35372e74869b839fa1da9da1baafa2de9ceaba3bb909de70f044`.
The older working private radio build remains under
`hardware/buildroot/private/connector3/ram-local/`; it has older packaging, so
rebuild the cleaned standalone userspace with the approved radio instead of
blindly declaring that entire old bundle release-ready.

## Experiments to avoid repeating

- Independently published NXP p154 loads and reports firmware active, then
  times out at first command `0xfa`, printed action `0x3000`; no Wi-Fi interface.
- A second p154 boot with a low PCIe DMA bus aperture failed identically.
  Do not promote that experimental range into the base DT.
- p119 was packed and audited, **not booted**. p53 was downloaded, **not tested**.
  There is no need to pursue these replacements under the user's final decision.
- A standard `pci=nomsi` comparison kernel was compiled, **not booted**:
  `output/connector3-clean-radio-legacy-irq/zImage`, SHA256
  `51d655d68dcacf9c66e74fc984c19b19ddf890c7ba38ef22695d5e42f4b2ec6e`.
  The private boot helper still selects the ordinary kernel, not this component.

## Short licence/provenance check

The working blob carries the Marvell identifiers above. The public vendor GPL
archive's manifest lists `linux-firmware` commit
`a179db97914da5e650c21ba8f9b0bae04a0f8a41` and a Marvell licence permitting
unmodified binary redistribution subject to its conditions. That package's
radio is **15.68.7.p77**, not the working **15.87.19.p14**. Therefore the exact
blob-to-licence association was not proven by this check. **The user explicitly
accepted this version mismatch and chose the working blob; do not reopen that
decision as an installation blocker.**

Records: `hardware/buildroot/reference/connector3/license-check-20261030/`
and `reference/connector3-ssh-archive/end-of-day-provenance-and-state-20261030.json`.
Keep reference/vendor files private and out of the image except the specifically
approved hardware firmware. Never package a vendor rootfs or private signing key.

## Next implementation work

The approved-radio exception, cleaned standalone rebuild and Wi-Fi-only retest
are complete. The manufacturer notice/hash/source attribution ship with the
image. Existing Galdrun identity/settings were restored only in the private RAM
overlay; generic defaults contain no home credentials or board identity.

The new read-only storage USB cold boot and geometry/ECC/protected-hash/UBI/UBIFS
checks passed. Its smaller kernel omits unused NFS/ext*/FUSE drivers to fit the
host FIT loader's conservative 8 MiB kernel limit and has its own live report.
Do not rebuild or replace the qualified artifact without repeating the relevant
checks. Persistent rootfs/settings preparation now proceeds offline; no NAND
program/erase qualification or persistent installation has happened.

An offline developer persistent filesystem candidate is now built and audited
under `private/connector3/persistent-rootfs/`. See its section in the current
checkpoint. `build-connector3-persistent-rootfs.sh` reuses the approved RAM image
and creates `RootFS` (768 LEBs) plus `GaldrunSettings` (128 LEBs) within MTD5,
leaving 548 PEBs before UBI reserves. Factory settings MTD3 stays outside it.
The first-boot init requires a read-only root, persists `/etc`/client state and
host keys in the settings volume, and passed interrupted-copy/restart host
checks. It retained the developer SSH public key. That earlier candidate is
superseded by the release-default rootfs/kernel/FIT and check installer in the
latest checkpoint above; physical installation qualification remains pending.

Then finish the **separate persistent rootfs/settings and USB installer**:
source/license bundle, signed FIT within 12 MiB, USB payload, protected boot
regions, check/preflight mode, storage cold boot, write/readback and rollback.
The latest checkpoint above now has a write-capable installer and a qualified
limited monitor; these earlier implementation notes are historical. Physical
NAND write/readback, rollback and persistent boot remain pending. Do not invent
an i.MX7 RNGB address or enable the i.MX6 RNGB option blindly.

Keep Deep Sleep disabled pending Ethernet wake qualification. Hardware PLL trim
and native rates passed previously. Audible MCU volume/mute remains **pending,
explicitly deferred by the user**; both RCA channels playing were confirmed on
the earlier working image. Do not claim new listening verification.

Read [the detailed checkpoint](connector3.md) for exact earlier artifact hashes,
read-only storage source/build commands, private evidence helpers and known-hosts
files. Inspect `git status` first: substantial pre-existing and new dirty work
is intentionally retained. No commit, reset or cleanup was requested.
