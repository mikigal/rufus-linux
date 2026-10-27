# Rufus for Linux (Wayland): porting plan

Baseline: Rufus 4.16 (`85873804`). This document describes how to bring **every feature
and the full user interface** of the Windows version to Linux, running natively under
Wayland, and tracks the progress of the port.

---

## 1. Summary

- **Rufus' own code** is about 46k lines (`src/*.c|h`), and the bundled libraries add
  about 150k more (`bled`, `ext2fs`, `libcdio`, `ms-sys`, `syslinux`, `wimlib`).
  - The libraries are mostly upstream POSIX code with thin Windows shims.
  - Rufus' own code is **tightly bound to Win32**:
    - worker threads read GUI controls directly;
    - paths assume a drive letter (`"D:"`);
    - all I/O goes through `HANDLE` and `DeviceIoControl`.
- **Wayland forces a privilege split.** A GUI should not (and by default cannot) run as
  root under Wayland, so the work is divided between two processes:
  - **Unprivileged GUI**: device enumeration, image analysis, hashing and downloads.
  - **Privileged `rufus-helper`** (polkit/pkexec, no GUI): formatting, writing, and every
    other raw-disk operation.
- **Toolkit: Qt 6 Widgets.** It gives the most faithful rendition of the classic Rufus
  window (split button, combos, progress bar with text, status bar) and mature Wayland
  support. The core stays in C, and the C++/C boundary doubles as the logic/UI
  separation that the code base lacks today.
- **Strategy: fork and refactor in place.**
  - Logic files (`iso.c`, `hash.c`, `wue.c`, `format*.c`, `dos*.c`, `parser.c`...) keep
    their names and function structure, so upstream fixes can still be carried over.
  - The GUI layer (`ui.c`, `stdlg.c`, `darkmode.c`, parts of `rufus.c`) and the disk
    layer (`dev.c`, `drive.c`) are rewritten as a Linux backend.
  - This repository will never build for Windows.
- **Feature parity** is achievable for almost everything.
  - Infeasible: ReFS (no `mkfs` on Linux), FFU (needs `dism`), post-write Chkdsk.
  - Dropped: Windows To Go (it needs a `bcdboot` replacement; decision of 2026-10-27).
  - Expensive: "Download ISO" (Fido is a PowerShell script with a WPF GUI and must be
    reimplemented), done in stage 4.

---

## 2. Status

| Stage | Status |
|---|---|
| 0. Foundation (build system, compat layer, libraries, CI, test harness) | **done** |
| 1. Decouple the logic from the UI | **done** |
| 2. Linux backend, helper, CLI | **done** (the NTFS, exFAT and UDF runs need their tools on the host) |
| 3. Qt 6 GUI | **done** |
| 4. Advanced features | **done** (T3 under Secure Boot with UEFI:NTFS needs mkntfs or mkfs.exfat on the host) |
| 5. Windows To Go | **dropped** (decision of 2026-10-27) |
| 6. Distribution | not started |

**Stage 0 (done)**

- **CMake build**:
  - strict warnings for Rufus' code (fatal with `-DRUFUS_WERROR=ON`) and relaxed ones
    for vendored code;
  - Qt 6 entry point that sets the Wayland `app_id`.
- **`src/compat/compat.h`**: Windows types, error codes and C runtime spellings only;
  no API emulation.
- **Libraries**:
  - **bled**: POSIX port. File descriptors replace `HANDLE`s, and two security fixes
    were added: a zip-slip guard and no file-sized stack allocations.
  - **ms-sys**: file descriptor I/O.
  - **Syslinux** libfat/libinstaller: no Windows code left.
  - **libcdio** (Rufus fork, for El Torito virtual files): glibc `config.h`.
  - **wimlib**: re-vendored at 1.14.5 with the Rufus ISO/UDF reader ported.
  - **ext2fs**: now the system library.
- **Unit tests** under `ctest`, and GitHub Actions on Arch (gcc, clang), Fedora and
  Ubuntu.
- **E2E harness** (`tests/e2e`, §8), 12 tests in about 35 s:
  - QEMU boot verifier with the serial, VGA text and OCR channels;
  - marker initramfs and synthetic media builder;
  - root-in-VM runner with an emulated USB stick.

  Verified results:
  - the synthetic GRUB ISO boots from CD and as a `dd`-written USB stick under SeaBIOS
    and OVMF (xHCI, EHCI, UAS);
  - root writes go through the Linux `usb-storage` and `uas` drivers in the guest.

  OVMF has no UAS driver, and SeaBIOS boots UAS only on USB 2, as on real hardware.

**Follow-up**: run `ctest -L e2e` in GitHub Actions once the repository is pushed
(hosted runners expose KVM).

**Stage 1 (done)**

- **`librufus-core`** (`src/core`, plus the ported upstream files, which stay in
  `src/`): built without any Windows header.
  - `rufus.h` no longer includes `windows.h`; it keeps the portable declarations, and
    the Win32/GUI ones come back with POSIX types as their files get ported.
  - `ui_iface.h`: log, status, progress, blocking questions (`Notification()`,
    `SelectionDialog()`, `ListDialog()`) and events (`UM_*`). The headless defaults
    never imply consent.
- **Main window rules** (`core/rules.c`): the partition scheme, target system, file
  system, cluster size, label and option rules of `rufus.c`/`ui.c`, as events on a plain
  `rules_state_t`. `tests/test_rules.c` checks scenarios, and replays random user
  sequences on both the rules and the **original Windows functions** (copied verbatim
  into `tests/rules_oracle`, run against mock controls), comparing the whole window after
  every step: 3000 sequences of 60 steps on every run.
- **Utility layer**: `stdio.c`, `stdfn.c`, `localization.c`, `parser.c`, `xml.c`, cregex
  and the progress computations (`core/progress.c`), covered by `tests/test_core.c`
  (also clean under ASan/UBSan).
  - `WindowsErrorString()` describes `errno`; `WindowsErrorFromErrno()` maps it to the
    `ERROR_*` codes of the status.
  - `RunCommandWithProgress()` takes an argument vector (`posix_spawn`), not a command
    line.
  - The configuration file functions work on UTF-8 with explicit BOM/UTF-16LE handling
    and Windows text mode line endings, so patched files match the Windows output.
  - `.loc` strings use LF line breaks.
- **Options of an operation** (`core/options.h`): `rufus_options_t opts` holds everything
  a format depends on: the selections and check boxes of the window, the decisions of the
  boot check (DD/ESP mode, WUE flags, `.c32` replacements) and the settings and cheat
  modes. It is what the GUI will send to the helper.
  - `rules_get_options()` takes the snapshot on START; the differential test compares it
    with what the Windows format thread reads from the controls and globals.
  - The worker code (`format*.c`, `dos.c`, `wue.c`, `drive.c`, `iso.c`, `syslinux.c`,
    `badblocks.c`, `hash.c`) no longer reads any control, and reads `opts.*` instead of
    the globals it replaces (same names).
  - Files (image, extra archive, `unattend.xml`), the image report and the drive
    properties stay process context (`core/globals.c`); the helper gets the files as
    file descriptors and establishes the rest itself.
  - `InitProgress()` is in `core/progress.c`, and is checked against the upstream one
    (without VDS) for every START of the differential test.
- **Hashes and Secure Boot** (`hash.c`, `pki.c`, `core/resource.c`): the digests, the
  hash DB, the DBX/SBAT/SVN/certificate revocation checks (OpenSSL) and the payloads of
  `rufus.rc` (`.incbin`, §4.5). `HashThread()` runs on pthreads. Covered by
  `tests/test_hash.c`, with PEs that it signs with its own test CA.
- **Image analysis** (`iso.c`, `vhd.c`, `core/analyze.c`): the ISO scan and extraction,
  the disk image detection and the MBR/PBR analysis.
  - Paths are relative to the extraction directory (from its length) instead of
    skipping a drive letter, and `wininst_path[]` is relative to the root of the ISO
    (`/sources/install.wim`).
  - The lookups that relied on a case insensitive file system use
    `PathFileExistsNoCase()`, and the FAT attributes go through
    `FAT_IOCTL_SET_ATTRIBUTES`.
  - VHD and VHDX images are read directly (`core/vdisk.c`, stage 4; upstream mounts them
    with VirtDisk).
  - Saving an image (optical disc to ISO, drive to VHD/VHDX/ISO) came with stage 4.
  - `tests/test_iso.c` builds Linux and Windows-like ISOs (xorriso, mtools, wimlib),
    VHDs (`qemu-img`) and disk images, and checks the image report, the extraction to
    FAT and NTFS (persistence and label patches, symbolic links, `md5sum.txt`) and the
    reads from the WIM and from `efi.img`.
- **Windows images** (`wue.c`): the version, the editions and the Windows To Go index
  from the XML index of `install.wim`, and the `unattend.xml` of the Windows User
  Experience.
  - The time zone and the regional options of this system get duplicated (stage 4,
    §5.6).
  - WinPE and the customization of `boot.wim` are in `wue_apply.c` (stage 4); Windows
    To Go is not ported (stage 5 was dropped).
  - `tests/test_wue.c` checks them on WIMs that wimlib creates with the properties of
    Windows images, with scripted answers to the questions.
- **Image scan** (`core/image_scan.c`): `ImageScanThread()`, with `StartImageScan()` for
  the thread, which reports `UM_IMAGE_SCAN_COMPLETED`. What it did to the main window is
  in `rules_on_image_scan_start()`/`rules_on_image_scan_end()`. Covered by
  `tests/test_scan.c` (ISOs, ISOHybrids, Windows images and disk images).
- **Boot check** (`core/boot_check.c`): `BootCheckThread()`, with `StartBootCheck()` for
  the thread, which reports `UM_FORMAT_START`. It works on `opts` and a
  `bootcheck_drive_t` (the block device and whether it is fixed), asks its questions
  through `ui_ask()` and writes its decisions to `opts`.
  - The bootloader files are looked up in `app_data_dir` with absolute paths, as the
    current directory is shared by all the threads.
  - The files get downloaded into `app_data_dir` (`core/download.c`, stage 4).
  - An image stored on the target is detected through sysfs (the disk of its file
    system), not through drive letters.
  - The Windows version checks of the host are gone: the Windows User Experience is
    always offered, and Windows To Go accepts GPT on removable drives.
  - `settings.h` only uses the ini file (§5.8): without one, the settings are not saved.
  - `tests/test_bootcheck.c` covers every question and outcome, with scripted answers
    and files placed in `app_data_dir`.
- **Threads and cancellation**: the threads of the core (hash, image scan, boot check)
  are pthreads, and only cooperative cancellation exists.
  - `ErrorStatus` and `LastWriteError` are C11 atomics. `CancelOperation()` sets
    `ErrorStatus`, which the workers check: ISO scan and extraction, hashes, WIM split,
    bled decompression, external commands (which get terminated) and write retries.
  - The helpers that return a static buffer (`lmprintf()`, `SizeToHumanReadable()`,
    `WindowsErrorString()`, etc.) have one per thread, and the strings given to the
    `ui_iface.h` callbacks are only valid during the call.
  - Upstream terminates the blocking process search (`process.c`) and the update check
    threads. The process search got ported with cooperative cancellation (stage 2), and
    there is no update check (see §5.7).
  - `tests/test_threads.c` cancels operations from another thread in the middle of their
    work, and formats strings from several threads; it runs clean under ThreadSanitizer.

**Stage 2 (done)**

Steps, each with its tests (unit tests on image files where root is not needed, T2/T3
in the VM otherwise):

1. Devices: enumeration, filters and system disks, `rufus-cli list`.
2. Disk access: the identity check (`diskseq`), unmounting, whole-disk `O_EXCL` opens,
   `BLKRRPART`, automount suppression and the private mount namespace.
3. Partitioning: `plan_partitions()` from `CreatePartition()`, and the MBR/GPT writer.
4. File systems: the `mkfs.*` tools, ext through libext2fs, large FAT32 through
   `format_fat32.c`.
5. The format pipeline (`format.c`): MBR/PBR, Syslinux, GRUB, ISO extraction, DD images,
   zeroing, bad blocks, persistence, UEFI:NTFS, FreeDOS.
6. `rufus-helper`, its IPC protocol and polkit action, and `rufus-cli` writing through it.
7. The T2/T3 scenarios of "Done when" (§9).

Done:

- **Devices** (`core/devices.c`, `platform/linux/devices.c`, `mounts.c`):
  - The filter chain of `GetDevices()` is `ShouldListDevice()`, in its order and with its
    log messages, on a `rufus_device_t` that the platform code fills from sysfs (USB
    device, SCSI INQUIRY data, size) and the udev database (partition table, file
    systems, labels). Nothing needs root, and no device gets opened.
  - Names are the ones Windows gives ("<vendor> <model> USB Device", or "UAS Device"),
    which `IsHDD()` (now in `core/hdd.c`) relies on. The display name shows the device
    name (`sdb`) where Windows shows drive letters or a disk number.
  - Card readers are MMC devices, or USB readers that udev identifies as such. Loop, RAM,
    zram, device mapper, RAID, NBD, optical and floppy devices are not listed.
  - A disk that holds a system mount (`/`, `/boot`, `/boot/efi`, `/efi`, `/usr`, `/var`,
    `/home`), a swap area or `app_dir` is never listed, whatever the options. Mounts are
    traced back to their disks through partitions and the slaves of device mapper and
    RAID devices; btrfs mounts through their source device.
  - `GetRulesDrive()` gives the main window rules what they need about the drive.
  - `rufus-cli list [--hdd] [--non-usb] [--json]`.
  - `tests/test_devices.c` checks the filters and names, and that no disk of the machine
    that runs the tests is listed. `tests/e2e/test_devices.py` lists an emulated BOT and
    UAS stick in the VM, never a virtio disk, and no longer the stick once it is mounted
    on `/var`, used as swap or below the device mapper.
- **Disk access** (`platform/linux/disk.c`, `automount.c`, `processes.c`):
  - `OpenDrive()` opens a drive from its device node, then checks on the descriptor
    that it still has the `diskseq` of the selected one, that it is not write protected
    for writing, and gets its size and sector sizes. The exclusive open (`O_EXCL`) is
    retried for `DRIVE_ACCESS_TIMEOUT`, as upstream retries its volume lock: the kernel
    refuses it while a partition is mounted or held by the device mapper or RAID, and
    it then keeps any partition from being mounted.
  - `UnmountDrive()` unmounts the file systems of the drive, of its partitions and of the
    devices built on them. When the drive stays busy, the mounts, holders (`dm-0
    (luks-...)`) and processes that use it get logged.
  - `SearchProcesses()` replaces `GetProcessSearch()` (`process.c`): the processes that
    have a device of the drive open (with their access mode), or a file, directory,
    executable or mapping on one of its file systems.
  - `RefreshPartitions()` is `BLKRRPART`, retried while udev probes the partitions, then
    a wait for udev.
  - `SuppressAutomount()` adds a udev rule (`UDISKS_IGNORE`, `UDISKS_AUTO=0`) in
    `/run/udev/rules.d`, which `RemoveAutomountRules()` removes, including the ones a
    crashed instance left.
  - `EnterPrivateMountNamespace()` and `MountVolume()`: mounts in `/run/rufus/mnt-*`,
    which the desktop doesn't see, and which go away with the process.
  - `tests/test_disk.c` checks what a user can (the mounts, partitions and processes of
    the disk of the build directory). `tests/test_disk_vm.c` runs as root in the VM
    (`tests/e2e/test_disk.py`, with a BOT and a UAS stick); it refuses to run outside of
    it, and only touches a QEMU stick.
- **Partitioning** (`core/partition.c`):
  - `PlanPartitions()` is the layout of `CreatePartition()`: the 1 MB (or cylinder, with
    the old BIOS fixes) offset, the ESP and MSR first, persistence, ESP, UEFI:NTFS or BIOS
    compatibility partitions at the end, the track and cluster alignments, and the MBR
    types and GPT types, names and attributes. The 255 × 63 geometry that Windows gives
    USB drives is used for the alignment, and the ESP always comes first (upstream needs
    a fixed drive or Windows 10 1703 for that).
  - `WritePartitionTable()` writes what `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` writes: MBR
    entries with CHS values, or a protective MBR and both GPTs (128 entries, random
    GUIDs), after zeroing the start of each partition and writing the UEFI:NTFS image.
    `ClearMBRGPT()` is the upstream one. Both work on any file descriptor.
  - `tests/test_partition.c` checks the layouts against values computed separately, and
    the tables on image files with `sfdisk` (512 and 4096 byte sectors). The VM test
    partitions the stick, and checks what the kernel and udev see.
- **File systems** (`platform/linux/filesystems.c`, `format_fat32.c`, `format_ext.c`):
  - `FormatPartition()` formats a partition (or an image file) with `mkfs.fat`, `mkntfs`,
    `mkfs.exfat` or `mkudffs`, with the 255 × 63 geometry and hidden sectors of the
    partition, the cluster size of the rules, NTFS compression (Alt-N) and indexing
    (Alt-Q), and no partition table of their own. A slow format zeroes the volume
    first, as FormatEx does. ReFS is not available.
  - Large FAT32 (over 32 GB, or Alt-L) is still `FormatLargeFAT32()`, on a file
    descriptor, which now writes the label itself (`SetVolumeLabel()` on Windows). The
    PBR is left to the format pipeline.
  - ext2/3/4 use the system libext2fs (`unix_io`), and ext4 now gets created (with
    `metadata_csum`), rather than ext3.
  - `ToValidLabel()` works on UTF-8 code points.
  - The mkfs tools open the partitions exclusively, so the drive can't be held with
    `O_EXCL` while they run; the udev rule keeps its partitions from being mounted.
  - `tests/test_filesystems.c` checks each file system on image files (`blkid`, the
    fsck tools, the boot sectors); NTFS, exFAT and UDF need ntfsprogs, exfatprogs and
    udftools. The VM test formats and mounts the partitions of the stick.
  - `RefreshPartitions()` now waits for udev to have processed each new partition: its
    queue can still be empty right after `BLKRRPART`.
- **Boot records and disk images** (`core/boot_records.c`, `core/write_drive.c`):
  - `WriteMBR()`, `WriteSBR()` and `WritePBR()` are the upstream ones, through ms-sys on
    the file descriptor of the drive; the PBR is written at the offset of its partition,
    once the volume is unmounted (Windows writes it through the locked volume).
  - `WriteDrive()` writes raw and compressed images (bled) and VHD/VHDX images (stage 4)
    from a file descriptor, and zeroes drives (fast zeroing included). FFU needs DISM.
  - `tests/test_write.c` checks them on image files, with ms-sys identifying the boot
    records.
- **Format pipeline** (`platform/linux/format_thread.c`): `FormatDrive()` is
  `FormatThread()`, in the order of §5.5: hide the drive from udisks, unmount it and
  open it exclusively; zero it, write an image to it, or clear its tables; partition
  it; release it and format the persistence and main partitions; take it back for the
  MBR, SBR and PBR; mount the main partition (in a private namespace when the caller is
  single threaded) for the ISO extraction, Grub4DOS, KolibriOS, Windows 7 EFI setup,
  `md5sum.txt`, extended label (`core/autorun.c`) and extra archive; unmount.
  `StartFormat()` runs it in a thread, which reports `UM_FORMAT_COMPLETED`.
  - Not ported, and refused rather than silently skipped: Windows To Go (which the GUI
    does not offer). WinPE and the Windows User Experience came with stage 4.
  - `FormatPartition()` waits for the device of the volume, and runs the mkfs tool again
    if that device got created again while it ran: udev reads the partition table of a
    drive that was open for writing again when it gets closed, at a moment that
    `WaitForUdev()` can't tell (`mkfs.fat` once found no `/dev/loop0p1`, with all the
    tests running at once).
  - `FormatPartition()` refuses FAT32 volumes with fewer than 65525 clusters, as
    FormatEx does: mkfs.fat creates them, and UEFI firmwares then take them for FAT16.
  - `tests/e2e/test_format.py` runs the pipeline in the VM (`tests/test_format_vm.c`),
    and boots the result in QEMU: FAT32/MBR and ext4/GPT non-bootable drives, the
    synthetic GRUB ISO in ISO mode (GPT, UEFI and MBR, BIOS) and in DD mode (BIOS and
    UEFI).
- **Boot loaders and the bad blocks check** (`syslinux.c`, `dos.c`, `dos_locale.c`,
  `badblocks.c`):
  - Syslinux writes `ldlinux.sys` through the mounted volume, and maps it to its sectors
    with `FIEMAP` (or `FIBMAP`), whatever the file system. Its boot sector gets written
    once the volume is unmounted, onto the boot sector read again: the FAT driver flags
    the volume as in use in it while mounted.
  - FreeDOS and MS-DOS (`diskcopy.dll`, from the Microsoft symbol server as upstream)
    write their files to the mounted volume, with their FAT attributes (FAT ioctls) and,
    for MS-DOS, timestamps. The keyboard and codepage follow the locale of the user
    (`opts.locale`, from `LC_ALL`/`LC_CTYPE`/`LANG`, as the Windows language ID and OEM
    codepage of that locale) rather than the keyboard layout. `CONFIG.SYS` and
    `AUTOEXEC.BAT` keep their CRLF line endings.
  - The bad blocks check runs on the drive with `O_DIRECT`. Its report goes to a file
    descriptor of the caller (truncated if no bad blocks were found), or to a file in
    `temp_dir` (deleted then), as upstream does with `%USERPROFILE%`.
  - The FAT32 boot records of ms-sys leave "NO NAME" as the label of the boot sector, as
    upstream (and Windows) do: `fsck.fat` reports it.
  - `tests/test_dos.c` checks the FreeDOS files and the keyboard and codepage of several
    locales. `tests/test_badblocks.c` checks an image file, and in the VM, device mapper
    devices with failing sectors and a fake drive (its second half mapped onto the
    first). The VM boots Syslinux 4 to its prompt and FreeDOS to `C:\>` with a Polish
    keyboard, and runs the pipeline with a bad blocks check.
- **`rufus-helper` and `rufus-cli write`** (`src/helper/`, `platform/linux/ipc.c`,
  `helper_client.c`, `res/linux/ie.rufus.helper.policy.in`):
  - The protocol (`ipc.h`): typed messages on a `SOCK_SEQPACKET` socket, the standard
    input of the helper, with descriptors (`SCM_RIGHTS`). `IPC_HELLO` checks the version
    and `sizeof(rufus_options_t)`, as both ends come from the same build; then
    `IPC_FILE`, `IPC_FORMAT` (options, drive, image, archive and bad blocks report),
    `IPC_ANSWER` and `IPC_CANCEL` one way, `IPC_LOG`, `IPC_STATUS`, `IPC_PROGRESS`,
    `IPC_EVENT`, `IPC_ASK` and `IPC_DONE` the other. The end of the socket cancels.
  - The helper checks what it gets: options in range (`CheckOptions()`), a drive that
    `GetDevices()` lists with its most permissive filters (so never a system disk), with
    the `diskseq` and size the front end saw, an image that it scans again and that is
    not on the drive, and only the boot files that the options use (`GetBootFiles()`),
    which it copies into an `app_data_dir` of its own in `/run/rufus` before
    `LoadBootFiles()` sets up what the boot check does. The image gets scanned through
    a link with its name in that directory, as Rufus tells how an image is compressed
    from its extension.
  - Each format runs in a child process, in a mount namespace of its own. The questions
    go to the front end (notifications and lists: the format asks no selection), and a
    cancellation reaches it as `SIGUSR1`. The helper reports `IPC_DONE` once the child
    is gone, so that the next request can't arrive before, and removes the udev rule
    of a child that crashed.
  - The front end side: `StartHelper()` (through pkexec unless already root, or
    `$RUFUS_HELPER` for development), `HelperFormat()`, which runs in the calling thread,
    forwards the messages to the `ui_iface` of the process and the cancellation of
    `ErrorStatus` to the helper, and `StopHelper()`.
  - The polkit action `ie.rufus.helper` allows the installed helper with
    `auth_admin_keep` in the active session: a single prompt, as with UAC.
  - `rufus-cli write <device>` sets the options through the rules of the main window,
    runs the boot check (questions on the terminal, or `--iso`/`--dd` and `--yes`) and
    asks for the confirmation of upstream (MSG_003), then writes through the helper.
    SIGINT cancels, and `--archive` adds a zip archive. The boot files get downloaded
    as in the GUI (stage 4).
  - Cleaning up no longer depends on the operation: `udevadm` runs after a
    cancellation (`RunCommand()` refuses to), and the drive gets a change event once its
    rule is gone, or udisks would keep ignoring it.
  - `tests/test_ipc.c` checks the messages (descriptors, malformed and oversized ones),
    `CheckOptions()`, the front end side against a fake helper, and that the helper
    refuses to run without root or its socket. `tests/test_bootcheck.c` checks the boot
    files. In the VM, `tests/test_helper_vm.c` checks the requests that the helper
    refuses, the boot files, and a cancellation and a disconnection during a bad blocks
    check, after which nothing is left; `tests/e2e/test_cli.py` writes a non-bootable
    drive, the GRUB ISO in ISO mode (booted with UEFI) and in DD mode (booted with
    BIOS), and interrupts a write.
- **The media of "Done when"** (`tests/e2e/test_media.py`): written with `rufus-cli`
  through the helper, with the options of the main window, then booted:
  - non-bootable FAT16, FAT32, ext2 and ext3, checked with their fsck tools; NTFS, exFAT
    and UDF with ntfsprogs, exfatprogs and udftools (skipped without them). ext4 is not
    offered, as on Windows (it is for persistence partitions);
  - DD images: raw (`test_cli.py`), xz and zstd, booted with BIOS;
  - the GRUB ISO in ISO mode (UEFI on GPT, and BIOS on MBR in `test_format.py`) and in
    DD mode; the Syslinux ISO in ISO mode, booted with BIOS. The DD mode of a Syslinux
    ISO would need a real isohybrid image, and is the same code path as for GRUB;
  - the Windows-like ISO on GPT/UEFI (FAT32), booted with UEFI, and on MBR with NTFS and
    UEFI:NTFS (with ntfsprogs), booted with UEFI through UEFI:NTFS;
  - FreeDOS to its prompt, the Ubuntu-like ISO with its `casper-rw` partition and
    `persistent` kernel parameter, and an extra archive (`--archive`).

**Stage 3 (done)**

- **Main window** (`src/ui/qt/main_window.cpp`): the layout of `IDD_DIALOG` with Qt
  layouts (sections, collapsible advanced options, the image option or persistence
  row), rendered from a `rules_state_t`: each user action goes to `rules_on_*()`,
  then the widgets get rendered from the state, with their signals blocked.
  - The widgets have the `IDC_*`/`IDS_*` name of their Windows counterpart as their
    object name, which `apply_localization()` and the tests use. As on Windows, the
    `.loc` has no dialog texts for en-US: the window sets those of `rufus.rc`.
  - `rules_entry_text()` (`core/rules_text.c`) renders the dropdown entries as the
    Windows version does. The persistence slider and units got their rules events
    (`rules_on_persistence_slider()`, `rules_on_persistence_units()`), and
    `rules_on_persistence_size()` now follows `EN_CHANGE`/`EN_KILLFOCUS` exactly; the
    differential test covers all three.
  - The drive list follows udev (`DeviceMonitor`, a refresh a second after the first
    event of a burst, queued during an operation), and the progress bar draws its
    text over the bar, with the paused and error colours and the marquee.
- **The core in C++**: the core headers compile as C++23 (`std::atomic` for `_Atomic`,
  `min`/`max` as templates), and the `libintl.h` stub of ms-sys, which hid glibc's from
  libstdc++, is gone. The callbacks of `ui_iface_t` get marshalled to the GUI thread
  (`ui_bridge.cpp`): queued for the log, status, progress and events, and blocking
  queued for the questions.
- **Operations**: image selection (file dialog, drag and drop, `-i`, Ctrl-SELECT for
  an archive), the scan and boot check threads, START with the confirmations of
  upstream (MBR over 2 TB, UDF duration, processes using the drive, MSG_003, several
  partitions, 4K sectors), the format through `rufus-helper` in a thread (the helper
  stays for the session: a single polkit prompt), the hash, cancellation with its
  confirmation, the elapsed time, and after a failure the processes that keep the
  drive busy (`IDD_LIST`) or a port cycle for "device not ready".
- **Dialogs** (`dialogs.cpp`, `log_window.cpp`): notification (icons, button sets,
  "don't show again", "more information"), selection (radio buttons or check boxes,
  user name and edition of the Windows User Experience, the silent install warning),
  list, hash, about and license (the RTF of the `.loc` and `license.h` as HTML), and
  the log as a window of its own (Wayland does not let it
  stick to the main window), saved to `$XDG_STATE_HOME/rufus/rufus.log` on exit.
  - The buttons that open a modal dialog do it once their click has returned, so that
    an accessibility client (a screen reader, or the tests) gets its answer.
  - Fido (DOWNLOAD), saving a drive to an image (IDC_SAVE) and Alt-O came with
    stage 4. There is no update check (§5.7), hence no `IDD_UPDATE_POLICY` (and no
    IDC_SETTINGS button, which only opens it), `IDD_NEW_VERSION`, Alt-Y or Ctrl-Alt-Y.
- **Localization**: the `.loc` of `$RUFUS_LOC`, the installed one or the one of the
  source tree; the locale of `-l`, the settings or `LC_ALL`/`LC_MESSAGES`/`LANG`. The
  language menu creates the window again in the selected language, RTL included.
- **Settings and directories**: `$XDG_CONFIG_HOME/rufus/rufus.ini`, the downloads in
  `$XDG_DATA_HOME/Rufus` (`FILES_DIR`, as in `%LOCALAPPDATA%\Rufus`), a lock file in
  `$XDG_RUNTIME_DIR` against a second instance (MSG_001/MSG_002).
  `SETTING_DARK_MODE` (Ctrl-Alt-D) forces a colour scheme, with the dark palette of
  the Windows version when the platform theme does not honour the request.
- **Cheat codes**: an application-wide event filter, ahead of the mnemonics, with
  every key of the message loop of `WinMain()` (see §6.5). A bare Alt cycles the
  progress text, also in the helper, which gets the type before and during a format
  (`IPC_PROGRESS_TYPE`, handed to its worker through a pipe and a thread, as a signal
  would interrupt the system calls of the format).
- **Desktop integration**: the taskbar progress over
  `com.canonical.Unity.LauncherEntry`, `QApplication::alert()`, and a desktop
  notification for the end of an operation in the background; `rufus.desktop` (the
  `app_id`), the hicolor icons and `rufus.loc` get installed.
- **Backend changes**:
  - virtual disks (the VHDs of Linux): the writable loop devices of files, with
    partition scanning (not the read-only ones of snaps), and the virtual disks of
    hypervisors that upstream knows, listed unless Alt-G hides them;
  - `IPC_CYCLE_PORT` (Alt-C, `rufus-cli cycle`): the helper deauthorizes the USB
    device and authorizes it again, as upstream disables and enables it;
  - `rufus_options_t.thread_priority` (Alt-+/-), which the helper turns into a nice
    value;
  - `MountVolume()` retries while udev reads the partition table of a drive that was
    closed after a write (an `ENOENT` race that e2e_media hit once);
  - `rufus-cli` and `rufus-helper` never called `SetEmbeddedSyslinuxVersions()`, so the
    boot check compared the Syslinux version of images with 0.00.
- **Tests (T4, §8.5)**: `tests/gui/test_gui.py` drives the GUI through AT-SPI under
  headless KWin at 100, 125 and 150% scaling (and sway, which is optional, when it is
  installed): the layout,
  eight languages (three RTL ones), the language menu, a write, a question, a
  cancellation and a failure, an image with persistence and its hashes, an ISOHybrid
  (selection dialog), the about, license and log dialogs, the taskbar
  progress and every cheat code, with fake drives and a fake helper, in about 30 s.
  `tests/e2e/test_gui_vm.py` writes a drive with the GUI and the real helper, as root
  in the VM, and checks it. e2e_cli also cycles a port and writes to the loop device
  of a file.

**Stage 4 (done)**

Persistence, the bad blocks check, the UEFI media validation and MS-DOS with its locale
came with stage 2 (and their GUI with stage 3); the rest is here.

- **Downloads** (`core/download.c`, the port of `net.c` without the update check): libcurl
  with the options of the WinInet session of upstream (user agent, timeouts, no
  compression, redirections only to HTTPS), the progress on the main window, the
  cancellation, the `.bak` of a file being replaced, and the errors as `errno`.
  - `DownloadSignedFile()` checks the `.sig` of the bootloader files with the key of
    `pki.c`; the boot check and `rufus-cli` download them into `app_data_dir`, and
    report a failed download as upstream (MSG_044). The helper never downloads.
  - Upstream refuses a file without a length, which HTTP/2 servers may omit; here it
    gets downloaded, into a buffer that grows.
  - The GUI gets `sbat_level.txt`, `sb_active.txt` and `sb_revoked.txt` when it starts,
    as `CheckForFidoThread()` does, and checks for DBX updates once a day, some time
    after the start and when nothing is going on, as the update check of upstream
    did (asking before it downloads them). As the AppStore version of upstream, this
    does not depend on an update setting. `IsRevokedByDbx()` uses a downloaded DBX
    when it is more recent than the embedded one (`UseLocalDbx()`).
  - Test hook: `$RUFUS_TEST_URL_PREFIX` maps every URL to a server of the tests
    (`https://rufus.ie/x` to `<prefix>/rufus.ie/x`). The unit and GUI tests point it to
    a port where nothing listens, so that they never reach the network.
  - `tests/test_download.c` serves files, redirections, errors, truncated and slow
    transfers from a server on 127.0.0.1, and a real signed file of rufus.ie (in
    `tests/data/download`), as is and tampered with.
- **Windows User Experience** (`wue.c`, `wue_apply.c`, `core/regional.c`):
  - "Set regional options to the same values as this user's": the IANA zone of the
    system (`$TZ`, `/etc/localtime`) as its Windows name, from the windowsZones table of
    Unicode CLDR with its aliases (`core/windows_zones.h`, generated by
    `res/linux/windows_zones.py`); the POSIX locales of the user as BCP-47 names
    (SystemLocale from LC_CTYPE, UserLocale from LC_TIME, UILanguage from LC_MESSAGES);
    and the first XKB layout of KDE's `kxkbrc` (or of localed) as a Windows keyboard
    layout ID, for InputLocale. UILanguageFallback is en-US.
  - `ApplyWindowsCustomization()` and `SetupWinPE()` work on the mount point of the main
    partition, with the paths of the ISO looked up without case (NTFS is case sensitive
    on Linux). The LabConfig keys go into the SYSTEM hive of `boot.wim` with hivex
    (RegLoadKey() upstream), or through `unattend.xml` without it.
  - The front end sends `unattend.xml` to the helper (`IPC_FD_UNATTEND`), which works on
    a copy of its own, as the customization modifies it.
  - `UpdateMD5Sum()` hashed the modified files through their lowercased paths, which
    NTFS does not find on Linux.
  - `tests/test_wue.c` checks the regional mappings, and applies the customization to a
    media tree with a `boot.wim` that wimlib creates, with a minimal SYSTEM hive
    (`tests/test_hive.h`): `appraiserres.dll`, the `setup.exe` wrapper, the 'Windows CA
    2023' bootloaders, the registry keys (and the fallback), and the WinPE patches of
    `setupldr.bin`. `test_media.py` writes a Windows 11 like ISO with the default
    options through the helper in the VM, checks the hive of `boot.wim` on the stick,
    and boots it.
- **Virtual disks and saved images** (`core/vdisk.c`, `iso_save.c`, the helper):
  - What upstream does with VirtDisk: reading the disk of a fixed or dynamic VHD or VHDX
    (to write it to a drive, and for the image analysis), and saving a drive to a fixed
    VHD (the raw drive and a footer, usable as a DD image) or a dynamic VHDX (32 MB
    blocks, without the ones that only hold zeroes, 512-byte logical sectors), as the
    VirtDisk of upstream creates them. Differencing images, and VHDX images with a log
    to replay, are refused. The geometry of a VHD whose size no CHS geometry gives is
    the maximum one, so that the readers that use it (qemu) take the size of the footer.
    `vhd_save.c` (VirtDisk and DISM) is gone, as FFU needs DISM.
  - IDC_SAVE: the front end opens the image and sends it to the helper (`IPC_SAVE`),
    which reads the drive (refusing an image on the drive itself), or mounts its first
    volume read-only in its mount namespace for an ISO of its files, which xorriso
    makes (ISO-9660 level 3 with Joliet and Rock Ridge, where upstream makes a UDF image
    with oscdimg.exe). As upstream, a VHD needs the space of the drive, the type of the
    last image is the one proposed (SETTING_PREFERRED_SAVE_IMAGE_TYPE), and the ISO is
    only offered for a drive with a file system.
  - Alt-O: the optical drive with a data disc, which the front end reads itself.
  - `rufus-cli save <device> <file>` and `rufus-cli save-optical <file>`.
  - `rufus-cli write --dual` is Alt-E (the dual BIOS+UEFI mode of Windows images), which
    the MBR media with UEFI:NTFS of `test_media.py` need, as upstream only adds UEFI:NTFS
    to a Windows image on MBR in that mode. That test had never run (no mkntfs on the
    development machine, and no e2e tests in the CI).
  - `tests/test_vdisk.c` reads the VHD and VHDX images of `qemu-img` (fixed, dynamic,
    small blocks) and checks the images of `VdiskSave()` with it; `tests/test_write.c`
    writes dynamic VHDs and VHDX to drives. In the VM, `test_media.py` saves a written
    stick to a VHD, a VHDX and an ISO, and a CD-ROM to an ISO. The GUI test saves
    through the fake helper (`savefile`, a test hook for the file dialog).
- **Fido** (`core/fido.c`, `core/json.c`, `src/ui/qt/fido_dialog.cpp`):
  - As upstream, the GUI gets the URL of Fido.ps1 from `rufus.ie/Fido.ver` when it
    starts (with the Secure Boot data), and SELECT becomes SELECT▾DOWNLOAD when GitHub
    has the script. As the AppStore version, this does not depend on an update setting.
  - DOWNLOAD downloads Fido.ps1 once (`.lzma` and `.sig`, checked with the key of
    `pki.c`), and parses the data of the script: `$WindowsVersions` (a small parser of its
    PowerShell arrays), `$OrgId`, `$ProfileId` and `$InstanceId`. The steps of the script
    are in C: Check-Locale, the session ID that vlscppe and ov-df whitelist, the SKUs of
    the product editions (getskuinformationbyproductedition), their download links
    (GetProductDownloadLinksBySku, with the Referer it needs, and the message of a ban,
    715-123130), and the ISOs of the UEFI Shell releases on GitHub (with the
    architectures of their `Version.xml`). The replies are JSON, which `core/json.c`
    parses (RFC 8259, with the escapes and surrogates of the localized names).
  - The dialog is the one of the script: each step below the previous one (version,
    release, edition, language, architecture), with Back, and "Download using a
    browser" at the end. The queries run in a thread; closing the dialog cancels them.
    The ISO then gets downloaded with the progress of the main window, and selected.
  - Qt gives a tool button with a menu no press action for accessibility clients, so
    IDC_SELECT has an accessible interface of its own (press and show menu).
  - `rufus-cli download [VERSION [RELEASE [EDITION [LANGUAGE [ARCH]]]]] [-o FILE]` lists
    the choices of each step, then gives the URL (or downloads it).
  - The connections of libcurl share their name resolutions, and may take 20 s
    (including the resolution, which the timeout of WinInet does not include): the
    redirections of GitHub resolved in more than 3.5 s on the development machine.
  - `tests/test_fido.c` checks the JSON parser, the data of scripts, and the whole flow
    against a server of the test: the real signed Fido.ps1 v1.71, and replies of the
    download site that check the requests (session, w and rticks, Referer). The GUI test
    does the same with a server in Python (`menu`, a test hook for the menu of SELECT),
    down to the download and the scan of the ISO. The flow was also checked against
    the Microsoft site (`rufus-cli download 1 1 1 Polish` gives the x64 and ARM64 links).
- **T3 under Secure Boot** (`tests/e2e/harness/secureboot.py`): the Secure Boot code of
  OVMF, with a copy of its blank variable store where the harness enrolls the keys itself
  (as virt-fw-vars does, which distributions do not all package): a PK and KEK of the
  tests (made by openssl), and a db of the Microsoft UEFI CA 2011, taken from the
  signature of the UEFI:NTFS that Rufus embeds. `test_media.py` writes an ISO whose boot
  loader is the one of UEFI:NTFS (signed by Microsoft), which starts and reports that
  Secure Boot is enabled, and the GRUB ISO, which the firmware refuses. With mkntfs (or
  mkfs.exfat), a Windows-like ISO on NTFS goes through UEFI:NTFS and its NTFS driver,
  which are then refused the unsigned boot loader of the media.

---

## 3. Code inventory

| Area | Files | Win32 coupling | Verdict |
|---|---|---|---|
| Main GUI | `rufus.c` (4376), `ui.c`, `ui.h`, `ui_data.h`, `rufus.rc`, `resource.h` | total; ~55% of `rufus.c` is logic mixed with `ComboBox_*`/`EnableWindow` | move logic to `core/`, rewrite UI in Qt |
| Dialogs | `stdlg.c` (2539), `darkmode.c` (1205) | total (dialog procs, RichEdit RTF, uxtheme, taskbar) | rewrite / drop |
| Devices | `dev.c` (SetupAPI, CfgMgr, USB hub IOCTLs) | total | new backend: libudev + sysfs |
| Disks | `drive.c` (2732): VDS COM, MountMgr, `IOCTL_DISK_*`, drive letters | total | new backend; keep the partition layout maths of `CreatePartition` |
| Formatting | `format.c` (2078), `format_fat32.c`, `format_ext.c` | medium: fmifs/VDS, widgets read from the worker thread | keep the pipeline; FS via `mkfs.*`, ext via system libext2fs |
| ISO | `iso.c` (2283) + libcdio | low, but `"D:/..."` paths with hard-coded offsets | path refactor (mount root + relative path) |
| WUE / Windows | `wue.c` (1485) | `RegLoadKey`, `bcdboot`, `dism`, locale/TZ APIs | unattend is portable; hives via hivex; WTG is hard |
| DOS | `dos.c`, `dos_locale.c` | low (FAT attributes, keyboard APIs) | portable + XKB to KLID tables |
| Hash / Secure Boot | `hash.c` (2707), `pki.c` | Win32 threads, WinCrypt/WinTrust | algorithms portable; signatures via OpenSSL |
| Network | `net.c` | WinInet, COM NetworkList | libcurl |
| Utilities | `stdio.c`, `stdfn.c`, `parser.c`, `msapi_utf8.h` (79 wrappers, 241 calls) | high | rewrite on POSIX; `parser.c` to UTF-8 |
| Processes | `process.c` | NT native API | `/proc/*/fd` scan |
| Bad blocks | `badblocks.c` | low (HANDLE, `SetTimer`) | portable, but **needs `O_DIRECT`** |
| SMART/HDD | `smart.c`, `hdd_vs_ufd.h` | heuristics are pure; passthrough only in test builds | portable; `SG_IO` optional |

How pervasive Windows is in the main files:

| Item | Uses |
|---|---|
| `BOOL` | 706 |
| `DWORD` | 583 |
| `HWND` | 215 |
| `HANDLE` | 202 |
| `SendMessage` | 180 |
| `MAX_PATH` | 112 |
| `wchar_t` | 107 |
| string literals with `\\` path separators | 206 |
| `%lu`/`%lx` format specifiers | 54 |

---

## 4. Key decisions

### 4.1 Privilege split (forced by Wayland)

On Windows, Rufus requires administrator rights at startup (`rufus.c:3787`). Under
Wayland, a GUI started as root cannot connect to the compositor, whose socket lives in
the user's `$XDG_RUNTIME_DIR`. Running graphical applications through `pkexec` is also
discouraged. Hence:

```
┌──────────────────────── rufus (GUI, user, Wayland) ─────────────────────────┐
│  Qt 6 Widgets  ──►  controller (C++)  ──►  librufus-core (C)                 │
│  • enumeration + hotplug (libudev, udev database: no root needed)            │
│  • image analysis (libcdio/wimlib), BootCheck, questions to the user         │
│  • hashing, downloads (libcurl + RSA verification), settings, localization   │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │ pkexec rufus-helper   (socketpair as stdin/stdout,
                │ framed messages, image fd passed with SCM_RIGHTS)
┌───────────────▼──────────────────────────────────────────────────────────────┐
│  rufus-helper (root, no GUI): librufus-core + platform/linux                 │
│  • FormatThread: partitions, mkfs, MBR/PBR, ISO extraction, syslinux/grub,   │
│    WUE                                                                       │
│  • bad blocks, image read/write, zeroing, scan for blocking processes        │
│  • private mount namespace (unshare CLONE_NEWNS), automount suppression     │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Rules**

- **The helper is started lazily**, on the first operation that needs it: START, saving
  a drive image, or the blocking-process scan. It stays alive for the session, and its
  polkit action uses `auth_admin_keep`, so the user sees a single prompt, much like UAC.
- **The helper does not trust the GUI.**
  - It enumerates the device again and checks its `diskseq`.
  - It refuses system disks: `/`, `/boot`, swap, dm/LVM/md holders, and the disk that
    holds the image.
  - It scans the image again itself, so `img_report` never needs to be serialized.
- **The image file is passed as a file descriptor** (`SCM_RIGHTS`), so the helper never
  opens user-chosen paths as root.
- **EOF on the socket means cancel and clean up**: unmount, and remove the temporary
  udev rule.
- **Transport**: start with pkexec plus stdio. The protocol is transport-agnostic, so it
  can later become a D-Bus-activated system service.

**Rejected alternatives**

- **UDisks2 alone**:
  - no control over cluster size (older versions);
  - no sector-exact partition layout;
  - no whole-disk `O_EXCL`, no FIEMAP, no process scan;
  - it mounts under `/run/media`, which the desktop sees.
- **GUI as root**: does not work under Wayland, and is bad practice.

### 4.2 Toolkit: Qt 6 Widgets (confirmed)

| Rufus needs | Qt 6 | GTK 4 |
|---|---|---|
| SELECT▾DOWNLOAD split button | `QToolButton::MenuButtonPopup` | only `AdwSplitButton` (libadwaita) |
| Classic combo box, status bar | `QComboBox`, `QStatusBar` | `GtkComboBox`/`GtkStatusbar` deprecated since 4.10 |
| Dialogs opened from worker threads | `Qt::BlockingQueuedConnection` (same semantics as `SendMessage`) | manual `g_main_context_invoke` + `GCond` |
| RTL, dark mode, fractional scaling | built in | built in |
| Look on KDE Plasma | native, server-side decorations | client-side decorations, Breeze-GTK theme |

### 4.3 Code strategy and upstream

- **A fork.** Upstream explicitly does not plan a Linux version.
- **`src/compat/compat.h`** covers only types, error codes and C runtime spellings:
  - `BOOL=int`;
  - `DWORD=uint32_t` (not `unsigned long`, because Linux is LP64);
  - `ERROR_*` codes and the HRESULT macros;
  - `_stricmp→strcasecmp`, `_mm_malloc→posix_memalign`.

  This keeps the logic files compiling with small diffs. Windows APIs (`CreateFile`,
  `DeviceIoControl`, `SendMessage`) are **never emulated**.
- **Tracking upstream**: add an `upstream` remote (pbatard/rufus) and merge at each
  release. Conflicts concentrate in rewritten files, where only logic changes need to be
  carried over by hand.

### 4.4 Libraries

| Library | Decision | Status |
|---|---|---|
| **bled** | vendored, POSIX platform layer, file descriptor API | ✅ done |
| **libcdio** | vendored Rufus fork: El Torito boot images as virtual `/[BOOT]/` files, missing from the system 2.4.0 | ✅ done |
| **wimlib** | upstream 1.14.5 with the Rufus patches re-applied (see `src/wimlib/README.rufus.md`), NTFS-3G backend when available | ✅ done |
| **ext2fs** | system `libext2fs` + `com_err` | ✅ done |
| **ms-sys** | vendored, file descriptor I/O | ✅ done |
| **syslinux** libfat/libinstaller | vendored; the Windows NTFS sector mapper dropped (FIEMAP later) | ✅ done |
| cregex, ezxml | unchanged | with the core |
| getopt, msvc-missing | dropped (glibc) | ✅ done |

Other dependencies:

- **System libraries**: libudev, OpenSSL (libcrypto), libcurl, Qt 6 (Widgets, DBus), and
  optionally hivex (without it, the registry keys of the Windows User Experience are
  created through `unattend.xml`, as the fallback of upstream does).
- **Runtime tools**: `mkfs.fat`, `mkntfs`, `mkfs.exfat`, `mkudffs`, `mke2fs`, plus
  optionally `xorriso` (saving a drive to an ISO).

### 4.5 Build and resources

- **CMake targets**:
  - the vendored libraries (done);
  - `librufus-core` (C, from stage 1);
  - `rufus-helper` (C) and `rufus-cli` (developer/test tool driving the full pipeline
    without a GUI), both from stage 2;
  - `rufus` (Qt 6).
- **Binary payloads** (`rufus.rc:454–540`: syslinux, grub, FreeDOS, `uefi-ntfs.img`,
  `md5/*.efi`, `dbx/*.efiauth2`, `setup_*.exe`) are embedded with the assembler's
  `.incbin` (`core/resource.c`), since the GCC of Ubuntu 24.04 has no C23 `#embed`.
  - A `GetResource(IDR_*)` shim stays; `embedded.loc` is not embedded, as the `.loc`
    file is installed with the application.
  - A file in `/usr/share/rufus` may override an embedded payload.
  - Never substitute the distribution's syslinux/grub/FreeDOS binaries: `db.h` and
    libinstaller expect exact versions.
- **Dropped or replaced**: `hogger.exe` is dropped. Icons go to `hicolor` (PNG/SVG are
  already in `res/icons`), plus a `.desktop` file.

---

## 5. Mapping Windows to Linux

### 5.1 Devices (`dev.c`)

| Windows | Linux |
|---|---|
| SetupAPI/`CM_*` over USBSTOR, UASPSTOR, ... | `udev_enumerate` (`block`/`disk`), skipping loop/ram/zram/dm/md |
| VID/PID/bcdUSB/speed via hub IOCTLs | parent `usb/usb_device` in sysfs: `idVendor`, `idProduct`, `bcdUSB`, `speed`, `rx_lanes` |
| UASP detection by driver name | `ID_USB_DRIVER` = `uas` vs `usb-storage` |
| `SPDRP_REMOVAL_POLICY` | sysfs `removable` (the SCSI RMB bit, which also gives Windows its media type); MMC cards count as removable |
| system disk filter | mountinfo (`/`, `/boot`, `/boot/efi`, `/efi`, `/usr`, `/var`, `/home`), `/proc/swaps`, traced through the `slaves/` of dm/md devices |
| "X USB Device" names | sysfs `vendor` + `model` + " USB Device"/" UAS Device" suffix, so the `IsHDD` tables keep working |
| `WM_DEVICECHANGE` + 1 s debounce | `udev_monitor` (QSocketNotifier) + the same debounce |
| `0x80+N` drive index | table index + devnode + `diskseq`, re-checked before every open |

The filter chain (`dev.c:944-1157`) becomes a pure `ShouldListDevice()` (done, see §2).

### 5.2 Disk access and locking (`drive.c`)

| Windows | Linux |
|---|---|
| `\\.\PhysicalDriveN` + `FSCTL_LOCK_VOLUME` (150×100 ms retries) | unmount partitions, then `open(O_RDWR\|O_EXCL)` the whole disk with the same retry loop |
| drive letters, `SetVolumeMountPoint`, `AltMountVolume`, `GLOBALROOT` | mount in the helper, in a private namespace (`/run/rufus/mnt-*`); ESPs mount normally, so the hacks go away |
| VDS `DeletePartition`/`Refresh` | zeroing (`ClearMBRGPT`) + `BLKRRPART` (fallback `BLKPG`) + wait for the udev event |
| MountMgr AutoMount off | a whole-disk `O_EXCL` blocks partition mounts, plus a temporary `/run/udev/rules.d` rule with `UDISKS_IGNORE=1` |
| `CycleDevice`/`CyclePort` | `USBDEVFS_RESET` or toggling `authorized` in sysfs |
| hook that closes the "Format disk" prompt | not needed: automount suppression also hides desktop notifications |

### 5.3 Partitioning

- **Layout maths: portable as-is** (`CreatePartition`, `drive.c:2326-2456`).
  - It covers alignment, ESP 260 MB, MSR 128 MB, persistence, UEFI:NTFS and the "BIOS
    Compatibility" partition, plus all MBR/GPT types.
  - It is extracted as `plan_partitions()`, returning a neutral structure.
- **Writing: our own MBR/GPT writer** (~300–400 lines: protective MBR, header, CRC32,
  backup).
  - It gives exact control over the disk signature (`MBR_UEFI_MARKER`), the GPT
    attributes (`NO_DRIVE_LETTER`) and the partition names.
  - It allows byte-level comparison with the Windows output.
- After writing: `BLKRRPART`, then wait for the partition node.

### 5.4 File systems

| FS | Windows | Linux |
|---|---|---|
| FAT16/FAT32 ≤32 GB | fmifs/VDS | `mkfs.fat -F16/-F32 -s -S -n` |
| FAT32 >32 GB | `format_fat32.c` | the same `format_fat32.c` (writes through an fd; keeps parity) |
| NTFS | fmifs/VDS | `mkntfs -Q -c -L` (+ `WritePBR` overwrites the boot code as before) |
| exFAT | fmifs/VDS | `mkfs.exfat -c -L` |
| UDF | fmifs/VDS | `mkudffs --blocksize --label --udfrev` |
| ext2/3/4 | vendored libext2fs (`nt_io`), no ext4 | system libext2fs (`unix_io`), **ext4 enabled** |
| ReFS | VDS, selected editions only | **infeasible**: no open source `mkfs` |

Notes:

- **ext4**: needs `s_checksum_type = CRC32C` with `metadata_csum`. This is verified by
  `tests/test_ext2fs.c`.
- **NTFS**: mounted with the kernel **ntfs3** driver, which supports `fallocate` (the AMI
  contiguous file workaround). NTFS is case-sensitive on Linux, so path lookups in the
  code must ignore case.

### 5.5 Image writing, ISO extraction, boot loaders

- **DD / compressed images**: `bled` on fds, then `pwrite` with aligned buffers.
- **ISO extraction**:
  - `"D:/..."` becomes a mount root plus a relative path;
  - buffers become `PATH_MAX`;
  - the hard-coded offsets go (`iso.c:153`, `:185`, `:767`, `:1052`, `hash.c:2499`);
  - hidden/system attributes use `FAT_IOCTL_SET_ATTRIBUTES` (vfat) or the
    `system.ntfs_attrib` xattr;
  - `CreateSymbolicLink` becomes `symlink`, and `SetFileTime` becomes `futimens`.
- **Order of operations**: MBR/PBR are written **after unmounting**. Windows writes the
  PBR through a locked, mounted volume.
- **Syslinux**:
  1. create `ldlinux.sys` through the file system;
  2. map its sectors with FIEMAP (vfat/ntfs3);
  3. run `syslinux_patch`;
  4. write the VBR.
- **GRUB2 / Grub4DOS**: raw MBR/SBR writes (`core.img` at LBA 1).
- **Config patching** (`fix_config`): the logic is unchanged, but `parser.c` moves from
  `_wfopen(..., "ccs=UNICODE")` to UTF-8 with explicit UTF-16LE/BOM handling.
- **Split `install.wim` >4 GB, CA2023, `boot.wim` updates**: wimlib, logic unchanged
  (open as `"<iso>|sources/install.wim"`).

### 5.6 Windows User Experience (`wue.c`)

- **`unattend.xml`** is portable. Host data must be mapped:
  - IANA time zone → Windows name, via CLDR `windowsZones.xml`;
  - `LANG` + XKB layout → BCP-47/KLID.
- **TPM/Secure Boot/RAM bypass**: **hivex** replaces `RegLoadKey`. The existing unattend
  `RunSynchronous reg add` fallback stays.
- **Windows To Go**: dropped (decision of 2026-10-27). It would need wimlib's NTFS-3G
  apply mode, the ESP, a BCD hive built from `BCD-Template` with hivex (for `bcdboot`),
  and `SanPolicy` in the SYSTEM hive: the hardest part of the port, for a feature that
  Microsoft has removed from Windows.

### 5.7 Network, signatures, updates, Fido

- **WinInet → libcurl**. Downloads stay in the unprivileged GUI, cached in
  `$XDG_CACHE_HOME/rufus`.
- **RSA `.sig` verification** (`pki.c:825`) → OpenSSL `EVP_DigestVerify`.
- **Boot loader issuer certificates** (used by the Secure Boot, DBX and SBAT checks) →
  OpenSSL `d2i_PKCS7` + `X509_digest`.
- **Authenticode checks**: dropped.
- **Self-update and update check**: dropped (decision of 2026-10-27). The packages of
  the distribution update Rufus, and nothing contacts `rufus.ie` to check for a new
  version.
- **What the GUI gets when it starts**, without asking, as the AppStore version of
  upstream does (decision of 2026-10-27): the Secure Boot revocation data
  (`sbat_level.txt`, `sb_active.txt`, `sb_revoked.txt`) and `Fido.ver` from `rufus.ie`,
  and whether GitHub has the script of Fido; the DBX updates of GitHub once a day
  (downloaded only once the user agrees). The requests carry no identifier, only the
  version of Rufus in the user agent. Upstream (outside of the AppStore) only makes
  them when the user enabled the update check.
- **Fido**: reimplemented in C with a Qt dialog (it does not work under `pwsh`: it has a
  WPF GUI and uses Windows named pipes). The script is still downloaded and checked, for
  its data (versions, releases, editions and the IDs of the download site), so that a
  new Fido release updates them.

### 5.8 Miscellaneous

| Windows | Linux |
|---|---|
| registry / `rufus.ini` | always an ini in `$XDG_CONFIG_HOME/rufus/rufus.ini` (`parser.c` already has an ini backend) |
| `%LOCALAPPDATA%\Rufus\rufus.log` | `$XDG_STATE_HOME/rufus/rufus.log` |
| `NtQuerySystemInformation` (blocking processes) | helper: `/proc/*/fd` (`S_ISBLK` + `st_rdev`), `fdinfo`, `cmdline`, mountinfo, holders |
| `CreateThread`/`TerminateThread`/`SetTimer` | pthreads; only cooperative cancellation (atomic `ErrorStatus`) |
| `WindowsErrorString` (264 calls) | `strerror(errno)` + mapping to `ERROR_*` |
| `RunCommandWithProgress` | `posix_spawn` + `poll` + `kill` |
| `GetKeyboardLayoutName`/`GetOEMCP` (DOS locale) | XKB/`LANG` → KLID / OEM code page tables |
| raw disk I/O (uncached) for bad blocks | **`O_DIRECT`**, otherwise fake drive detection passes from the page cache |
| VirtDisk (VHD/VHDX) | `core/vdisk.c`: the VHD and VHDX formats, read and written by hand |

---

## 6. GUI under Wayland

### 6.1 Window mapping

| Windows | Qt 6 |
|---|---|
| `IDD_DIALOG`: 3 sections, collapsible "advanced" areas, hand-computed pixel layout | layouts; section = header + line; collapse with an arrow `QToolButton` + `adjustSize()` |
| `IDC_SELECT` split button | `QToolButton` (`MenuButtonPopup`) with a SELECT/DOWNLOAD menu; Ctrl+click = extra .zip |
| `IDC_PROGRESS` with text, states, marquee | `QProgressBar` subclass with its own `paintEvent` |
| status bar with a `00:00:00` timer | `QStatusBar` + permanent timer widget |
| toolbar (language/about/settings/log), save, hash | `QToolButton`s with the `res/icons` images, without settings (§5.7) |
| `IDD_LOG` docked next to the main window | separate window (Wayland forbids positioning) or a slide-out panel |
| `IDD_ABOUTBOX`/`IDD_LICENSE` (RichEdit RTF) | `QTextBrowser`; RTF from `license.h` and release notes converted to HTML |
| `IDD_NOTIFICATION` (OK/YesNo/AbortRetryIgnore, "don't show again", "more information") | custom `QDialog` |
| `IDD_SELECTION` (≤16 radios/checkboxes, user name field, edition combo, nesting) | custom `QDialog` + `QButtonGroup` |
| `IDD_LIST`, `IDD_HASH` | simple `QDialog`s |
| `IDD_UPDATE_POLICY`, `IDD_NEW_VERSION` | none: there is no update check (§5.7) |
| language menu with check mark | `QMenu` with checkable actions |
| `IFileDialog` | `QFileDialog` (native KDE dialog / portal) |

### 6.2 Threads and UI communication

- **`rufus_options_t`**: a snapshot of every option, taken on START. No worker thread
  reads widgets any more (today ~40 places do: `format.c:699`, `:796`, `:1792-1803`,
  `dos.c:408`, `wue.c:872`, ...).
- **`ui_iface.h`**: `ui_log`, `ui_status`, `ui_info`, `ui_progress`, a blocking `ui_ask`,
  and `ui_event` (replacing `UM_*`).
  - In the GUI, implemented with `QMetaObject::invokeMethod` (queued / blocking
    queued).
  - In the helper, implemented as IPC messages.
- **`lmprintf`** (rotating static buffers) becomes thread-safe.

### 6.3 Wayland specifics

- **No window positioning**: the log cannot stick to the main window, and dialogs are
  transient children.
- **Taskbar progress** (`ITaskbarList3`): D-Bus `com.canonical.Unity.LauncherEntry`
  (supported by the Plasma task manager and docks). `FlashWindowEx` →
  `QApplication::alert`, plus `org.freedesktop.Notifications`.
- **Drag & drop**: `dropEvent` with `text/uri-list`.
- **Dark mode**: follows Plasma. Rufus' system/dark/light setting (Ctrl-Alt-D) →
  `QStyleHints::setColorScheme`. `darkmode.c` disappears.
- **Scaling**: layouts + fractional scaling (`wp_fractional_scale_v1`) instead of
  `fScale`/`AdjustForLowDPI`.
- **RTL**: `QGuiApplication::setLayoutDirection(Qt::RightToLeft)`.
- **Single instance**: D-Bus name `ie.rufus.Rufus` or a `QLockFile`; xdg-activation to
  raise the existing window.
- **Alt+letter cheat codes**: an application-wide event filter, ahead of button
  mnemonics. A bare Alt press/release still cycles the progress text (%/speed/ETA).

### 6.4 Localization

- The `.loc` engine (38 languages) stays.
- Qt widgets get an `objectName` equal to their `IDC_*` identifier, and
  `apply_localization` calls `ui_set_text(id, text)` instead of `SetWindowTextU`.
- New Linux-specific messages go into `rufus.loc`, with an English fallback.

### 6.5 Cheat codes

All of them are carried over (`rufus.c:3904-4280`, `cheatCode()` in
`main_window.cpp`). Windows-only ones are mapped:

| Shortcut | Windows | Linux |
|---|---|---|
| Alt-G | list VHDs | list virtual disks: the writable loop devices of files, and the virtual disks of hypervisors |
| Alt-W | VMware disk detection | count VMware virtual disks as virtual disks |
| Alt-C | cycle USB port | through the helper: deauthorize the USB device, then authorize it again |
| Alt-+/- | thread priority of the format | nice value of the format in the helper (from the next operation) |
| Alt-R | delete registry keys | empty `rufus.ini` |
| Alt-D | delete app files dir | `$XDG_DATA_HOME/Rufus` |
| Ctrl-Alt-D | dark mode, with a relaunch | the colour scheme (or a dark palette), without a relaunch |
| Alt-V, Alt-X, Alt-P | VDS, NoDriveTypeAutorun, ESP toggle | a message saying that Linux does not need them |
| Alt-O | optical disc to ISO | the same, from the optical drive that udev reports with a data disc (read by the user, as systemd gives the session access to it) |
| Alt-N, Alt-Q | NTFS compression, indexing | passed to the format (`mkntfs -C`, and no `-I`) |
| Ctrl-T | test hashes (debug builds) | not carried over |
| Alt-Y, Ctrl-Alt-Y | force the update check | not carried over: there is no update check (§5.7) |

---

## 7. Feature matrix

| Feature | Status | Notes |
|---|---|---|
| Formatting FAT/FAT32/NTFS/exFAT/UDF/ext2/ext3 | ✅ | + ext4 (disabled on Windows) |
| Large FAT32, cluster sizes, labels, quick/full | ✅ | |
| DD images (raw, gz/xz/bz2/zstd/lzma/zip/vtsi) | ✅ | |
| ISOHybrid: ISO / DD / ISO→ESP modes | ✅ | |
| Linux ISOs: Syslinux 4/6, GRUB2, Grub4DOS, ReactOS | ✅ | FIEMAP instead of retrieval pointers |
| Windows ISOs: MBR/GPT, BIOS/UEFI, UEFI:NTFS | ✅ | |
| FreeDOS, MS-DOS | ✅ | `diskcopy.dll` from the MS symbol server |
| Persistence partition (casper-rw / persistence) | ✅ | |
| Windows User Experience (local account, privacy, TPM/SB/RAM, BitLocker, S-Mode) | ✅/⚠️ | hivex + locale/TZ mapping |
| WIM split >4 GB, CA2023, UEFI media validation | ✅ | |
| Secure Boot checks: DBX, SBAT, SVN, certificates | ✅ | OpenSSL |
| Bad blocks + fake drive detection | ✅ | requires `O_DIRECT` |
| MD5/SHA-1/SHA-256/SHA-512 | ✅ | |
| Save drive to VHD, VHDX or ISO | ✅ | `core/vdisk.c` (fixed VHD, dynamic VHDX), xorriso for the ISO |
| Write VHD/VHDX to drive | ✅ | `core/vdisk.c` reads fixed and dynamic VHD and VHDX |
| Save optical disc to ISO | ✅ | read `/dev/sr*` |
| USB HDDs, non-USB removables, VID:PID filters | ✅ | |
| Processes blocking the drive | ✅ | helper, `/proc` |
| 38 languages, RTL, dark mode, cheat codes, drag & drop | ✅ | done (stage 3) |
| Update check | ❌ | dropped: the packages of the distribution update Rufus |
| Windows / UEFI Shell ISO download (Fido) | ✅ | the steps of Fido.ps1 in C (`core/fido.c`), with its signed data, and a Qt dialog |
| Windows To Go | ❌ | dropped: NTFS-3G apply + BCD via hivex, at a high cost |
| NTFS Chkdsk after writing | ❌ | no equivalent |
| ReFS | ❌ | no `mkfs.refs` |
| FFU (save and write) | ❌ | needs `dism` |
| Self-replacing executable | ❌ | replaced by packages |

---

## 8. Testing without a human in the loop

Goal: every feature can be verified end to end **by a script, without root on the host
and without physical hardware**. The only manual steps are installing the tools once
(see `tests/e2e/README.md`) and an optional real-hardware tier.

### 8.1 Tiers

| Tier | What | Privileges | Where | Run with |
|---|---|---|---|---|
| T0 | unit tests: libraries, rules, layout planner, MBR/GPT writer, parsers | none | host | `ctest` |
| T1 | component tests on **image files**: boot records, DD writes, VHD, bled, ext2fs | none | host | `ctest` |
| T2 | **end-to-end write** to an emulated USB stick, as root | root **inside a VM** | QEMU/KVM VM booted from the host kernel (virtme-ng) | `ctest -L e2e` |
| T3 | **boot verification** of the written stick | none | QEMU with SeaBIOS (BIOS) and OVMF (UEFI, and UEFI + Secure Boot) | `ctest -L e2e` |
| T4 | **GUI end-to-end** under a headless Wayland compositor | none (root in the VM for the full stack) | `kwin_wayland --virtual` (sway headless, optional) | `ctest -L gui` |
| T5 | real ISOs (Ubuntu, Fedora, Debian, Arch, Windows 11, ...) | as T2/T3 | as T2/T3 | `ctest -L slow`, nightly, opt-in download |
| T6 | real hardware with a dedicated stick, identified by USB serial | root | host | opt-in only |

### 8.2 T2: writing to an emulated USB stick, as root, without root on the host

- **The VM boots the host kernel with the host root file system**, read-only, through
  virtio-fs/9p (virtme-ng), in about a second with KVM. Inside, the tests are root, and
  the freshly built `rufus-cli`/`rufus-helper` run unchanged from the build directory.
- **The target is a sparse raw file on the host**, attached to the VM as a real USB
  device: `-device qemu-xhci -device usb-storage` (or `usb-uas` for the UASP path).
  The whole Linux backend is therefore exercised for real:
  - udev reports `ID_BUS=usb` and `ID_USB_DRIVER=usb-storage|uas`;
  - the filters, unmount, `O_EXCL`, `BLKRRPART`, mkfs, the private mount namespace and
    automount suppression all run against it.
- **Hotplug**: QMP `device_add` / `device_del` of the stick while Rufus runs (device
  list refresh, removal during a write).
- **Structural checks after the VM exits**, on the host and unprivileged, against the
  stick image:
  - `sfdisk --json` (layout, types, attributes, disk signature);
  - `blkid -p -O <offset>` (file system, label);
  - `mdir`/`mcopy -i img@@offset` (FAT contents);
  - `fsck.* -n` on extracted partitions;
  - comparison of file hashes with the source ISO;
  - byte checks of the MBR/PBR against the boot code tables in ms-sys.

### 8.3 T3: does the stick boot?

The written image is booted in QEMU **as a USB stick** (xHCI + `usb-storage`,
`bootindex=0`), with SeaBIOS and OVMF. Success is detected through three observation
channels, without a screen or a person:

1. **Serial console marker**: the synthetic test media boot a tiny Linux system (the
   host kernel + a static `init`) that prints `RUFUS-E2E-BOOT-OK`, the kernel command
   line and the block devices it sees (with ext labels) to `ttyS0`, then powers off.
   This checks, for example, that persistence parameters were injected and that a
   `casper-rw` partition exists.
2. **VGA text memory**: in BIOS text mode, QMP `pmemsave` of `0xB8000` is decoded as
   CP437, to read boot menus (syslinux, GRUB), the FreeDOS/MS-DOS prompt, or SeaBIOS
   errors such as "No bootable device".
3. **Screenshots + OCR**: QMP `screendump` (PNG) + `tesseract`, for graphical screens
   (UEFI GRUB, Windows Setup, OVMF shell).

Each test case states its firmware, expected channel and timeout. Serial logs,
screenshots and OCR output are kept as artifacts.

### 8.4 Test media, generated without downloads

A media builder (`tests/e2e`) creates small, deterministic images from tools that are
already installed:

| Media | Built with | Exercises |
|---|---|---|
| `linux-grub.iso` | `grub-mkrescue` (BIOS + UEFI ISOHybrid) + host kernel + marker initramfs | ISO mode with GRUB2 (incl. Rufus' own `core.img`), DD mode, ISO→ESP |
| `linux-syslinux.iso` | `xorriso`, with an isolinux tree whose `isolinux.bin` only holds the version string of Syslinux 4 (no syslinux package needed: Rufus installs its own) | Syslinux install, config patching (ISO mode only) |
| `linux-casper.iso` | GRUB ISO with an Ubuntu-like `/casper` layout | persistence partition + kernel parameters |
| `windows-like.iso` | ISO-9660 tree with `bootmgr`, `bootmgr.efi` and `efi/boot/bootx64.efi` (a standalone GRUB that boots the marker kernel from FAT or NTFS). Still to come: a >4 GB `install.wim` variant (slow tier) | Windows detection, UEFI:NTFS chain loading; later WIM split |
| `windows11-like.iso` | `windows-like.iso` with the `sources/boot.wim` (with a SYSTEM hive) and `sources/install.wim` (Windows 11 24H2) of `rufus-test-mkwim` | Windows User Experience |
| `disk.img[.gz/.xz/.zst/.bz2]`, `.vhd`, `.vhdx` | the ISOs above as raw disks, compressed or converted | DD writes, decompression, VHD/VHDX |
| FreeDOS / MS-DOS | Rufus' own embedded files | DOS boot to a prompt (VGA text) |

**Real ISOs (T5)** are downloaded once into `~/.cache/rufus-tests`, pinned by SHA-256,
only with the user's consent. Their pass criteria are OCR of the boot menu / Windows
Setup screen.

### 8.5 T4: GUI

- **Compositor**: each test gets a headless compositor, `kwin_wayland --virtual` (the
  compositor this port targets; its scale comes from its output configuration, as its
  virtual backend ignores `--scale`), or optionally sway (the wlroots headless backend,
  skipped when it is not installed), with a session bus and an accessibility bus of the tests, and the
  AT-SPI registry, so that nothing depends on systemd or on the desktop of the user.
- **Driver**: AT-SPI (`python-gobject`), finding widgets by their `IDC_*` object
  names, which Qt exposes as accessible ids:
  - select the device, choose options, press START, answer dialogs;
  - assert enabled states, translated labels, the layout (inside the window, without
    overlaps) and the RTL mirroring.
- **Test hooks** (`src/ui/qt/test_hooks.h`), active with their environment variables,
  for what AT-SPI can't do under Wayland: keys (the cheat codes, and the dropdowns,
  whose AT-SPI selection does not commit), drag and drop, captures with
  `QWidget::grab` (with their device pixel ratio), and the language menu, as a
  headless compositor does not pop up a menu without an input event.
- **Helper and drives**: on the host, fake drives (`RUFUS_TEST_DEVICES`, through the
  real filters) and a fake helper (`tests/fake_helper.c`, `RUFUS_FAKE_HELPER`) that
  records the job and replays progress, a question, a failure or a cancellation. The
  full stack (GUI + real helper + emulated stick) runs inside the T2 VM
  (`test_gui_vm.py`).

### 8.6 Parity with Windows

The partition layout planner and the boot record writers are table-tested against
expected values derived from the Windows code: offsets, sizes, types, GUIDs, attributes
and boot code bytes.

An optional golden set (the same ISO written by the Windows Rufus to a VHD in a Windows
VM) can be added later. Producing it needs a Windows installation, so it is not part of
the autonomous loop.

### 8.7 CI

GitHub-hosted Linux runners expose KVM, so T0–T4 (except the slow tier) can run on every
push. On the host: `ctest` for T0/T1, `ctest -L e2e` for T2/T3, and `ctest -L gui` for
T4.

---

## 9. Stages

Size: S / M / L / XL (relative effort). Each stage ends with a working, testable result.

### Stage 0: Foundation (M), done

- CMake with the targets of §4.5, compat header, library ports, CI.
- E2E harness (§8):
  - a VM runner with an emulated USB stick;
  - a QEMU boot verifier with the serial, VGA text and OCR channels;
  - the synthetic media builder.
- **Done when** the harness boots the synthetic media directly (as a CD and as a
  `dd`-written USB stick) under BIOS and UEFI, and recognizes success and failure on
  every channel.

### Stage 1: Decouple the logic from the UI (XL, critical)

- `rufus_options_t` + `ui_iface.h`; no more widget reads from threads (~40 places).
- Move from `rufus.c` into pure functions in `core/rules.c`: `SetAllowedFileSystems`,
  `SetPartitionSchemeAndTargetSystem`, `SetFileSystemAndClusterSize`,
  `SetProposedLabel`, and the option applicability rules.
- `ImageScanThread` and `BootCheckThread` move into the core, with questions going
  through `ui_ask`.
- Path refactor (no more `"D:"`), `parser.c` to UTF-8, `msapi_utf8.h` removed, pthreads,
  atomic `ErrorStatus`.
- **Done when** the core compiles without `windows.h`, and rule matrix tests (image type
  × options → allowed FS/partition/target) match the Windows behaviour.

### Stage 2: Linux backend + helper + CLI (XL)

- `platform/linux`:
  - enumeration and hotplug, filters, `O_EXCL` opens;
  - the MBR/GPT writer, `BLKRRPART`, private mounts, mkfs;
  - automount suppression, the blocking-process scan, bad blocks with `O_DIRECT`.
- `rufus-helper` + IPC protocol + polkit action `ie.rufus.helper`; `rufus-cli`.
- **Done when** T2 + T3 pass for:
  - non-bootable formatting in every FS;
  - DD (raw/xz/zst);
  - GRUB and Syslinux ISOs (ISO and DD modes);
  - the Windows-like ISO (GPT/UEFI, and MBR/BIOS+UEFI with UEFI:NTFS);
  - FreeDOS;
  - persistence.

### Stage 3: Qt 6 GUI (L)

- The main window and every dialog of §6.1, plus localization, RTL, dark mode, cheat
  codes, drag & drop, hotplug, taskbar progress, log and settings (with no update
  settings, §5.7).
- **Done when** T4 passes on KWin at 100/125/150% scaling, including a sample of
  languages with RTL, and every cheat code is reachable. (sway was dropped from this:
  the port targets KDE Plasma.)

### Stage 4: Advanced features (L), done

- WUE (hivex, locale/TZ/keyboard mapping), and T3 under OVMF + Secure Boot.
- Signed syslinux/grub downloads, DBX/SBAT updates.
- VHD/VHDX, optical disc → ISO, and Fido in C with a Qt dialog.
- (Persistence, the bad blocks check, the UEFI validation and MS-DOS with its locale came
  with stage 2.)

### Stage 5: Windows To Go (L, optional), dropped

- NTFS-3G apply, ESP, BCD from the template via hivex, SanPolicy, and `unattend.xml` in
  `Panther`. Dropped on 2026-10-27 (see §5.6).

### Stage 6: Distribution (S)

- PKGBUILD (Arch/CachyOS) first, then .deb/.rpm.
- `.desktop` file, AppStream metainfo, hicolor icons, polkit action, man page.
- Flatpak is **not** possible with a pkexec helper. AppImage is possible as a secondary
  format (polkit then shows the generic `pkexec` prompt).

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| Stage 1 touches most of the code, which makes upstream merges painful | small mechanical commits; keep function names; merge at every upstream release |
| Races with desktop automount (KDE Device Notifier, gvfs) | whole-disk `O_EXCL` + `UDISKS_IGNORE` rule + private mount namespace; covered by T2 |
| NTFS: ntfs3 vs ntfs-3g, case sensitivity, file contiguity | ntfs3 by default; case-insensitive lookups in the code; T3 boot tests |
| Fido breaks when Microsoft changes its site | follow upstream Fido; feature is optional |
| A fork called "Rufus" may confuse users and load upstream servers | clearly marked as unofficial, own update endpoint, possibly a mirror for syslinux/grub files |

---

## 11. Upstream bugs found along the way

- `src/dev.c:778` compares `usbstor_name[0]` instead of `usbstor_name[j]`. As a result,
  only USBSTOR sets `is_USB`, and card reader enumerators are never recognized.
- `src/format.c:516` compares `Label` (the volume label) with `"UDF"` instead of the file
  system name, so `SETTING_USE_UDF_VERSION` effectively never applies.
- **Main window rules** (`rufus.c`). `core/rules.c` keeps these on purpose, so that it
  stays identical to the Windows behaviour; each one is marked in the code.
  - The `IDC_BOOT_SELECTION` handler ends by swapping the partition scheme and the target
    system: `target_type` gets the partition style and `partition_type` the target type.
  - `SetFSFromISO()` seeds its file system mask with `FS_NTFS | FS_FAT32` (indexes, not
    bits), so FAT32 always counts as available, and FAT16 does whenever there is no file
    over 4 GB.
  - `SetProposedLabel()` compares `"NO_LABEL"` with `""` instead of with the drive label.
  - The `IDC_FILE_SYSTEM` handler never records FAT16 (index 0) as the user's choice.
  - Fixed in this port (and in the reference of `tests/test_rules.c`): at the end of an
    image scan, `SetFSFromISO()` selects its file system through the
    `IDC_FILE_SYSTEM` handler, which returns early as the dropdown is still disabled.
    So when FAT32 replaces FAT16 (drives under 4 GB), it keeps the cluster sizes of
    FAT16, whose default can leave FAT32 with fewer than 65525 clusters.
- **Notifications** (`stdlg.c`): the "Do not show this message again" check box
  always sets `SETTING_DISABLE_SECURE_BOOT_NOTICE`, whatever setting the caller gave.
  The Qt dialog sets the one it got.
- **Utility code**, fixed in this port:
  - `xml.c` (ezxml): entity replacement uses `strncpy_s(..., _TRUNCATE)`, which
    terminates the string inside the replacement, so any value is cut at its first
    `&amp;`, `&lt;`, etc.
  - `GetPeSection()` (`parser.c`) returns the size of the first section of the PE,
    instead of the size of the section it found.
  - `FindResourceRva()` (`parser.c`) only compares the first half of a resource name
    (`wcslen()` bytes rather than characters).
  - `PrintStatusInfo()` (`localization.c`) formats its "UNTRANSLATED" debug message
    into a buffer whose size it takes as `sizeof(char*)`.
  - `UpdateMD5Sum()` (`hash.c`) reads past the end of `md5sum.txt` when looking for
    bootloader entries near its end.
  - `FindResourceRva()` (`parser.c`) computes a pointer from a NULL directory before
    checking for it.
- **Image scan** (`rufus.c`), fixed in this port: for an `install.wim`/`install.esd`, the
  architecture of the EFI boot manager is stored one bit lower in `has_efi` than for an
  ISO, so that the Windows To Go `unattend.xml` of an x86_64 image is for x86_32.
- **Windows User Experience** (`wue.c`), fixed in this port: the first logon commands of
  the "QoL" and "SkuSiPolicy" options are only written when another option of the
  Shell-Setup component (user, privacy, locale, silent install) is selected too.
- **bled**:
  - zip members with `..` or absolute paths were extracted outside the target
    directory;
  - the xz index / 7z header buffers were sized from the file and put on the stack.

  Both are fixed in this port.
- **libcdio**: `_fs_stat_traverse()` and `_fs_iso_stat_traverse()` copy the symbolic
  link buffer of the root directory from a NULL pointer (with a zero size), on every
  scan. Fixed in this port.
- **wimlib ISO reader** (Windows version):
  - `full_read()` always read from offset 0;
  - reads past the end of the file silently returned a short buffer.

  Both are fixed in this port.
