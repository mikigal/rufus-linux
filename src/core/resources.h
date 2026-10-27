/*
 * Rufus: The Reliable USB Formatting Utility
 * Binary payloads embedded in the executable
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The RCDATA resources of rufus.rc, as RESOURCE(id, path relative to res/).
 * Left out: hogger.exe (Windows only) and embedded.loc (the .loc file is
 * installed with the application instead). Added: the icon of the
 * application, which is not an RCDATA resource upstream.
 */

RESOURCE(IDR_SL_LDLINUX_V4_BSS, "syslinux/ldlinux_v4.bss")
RESOURCE(IDR_SL_LDLINUX_V4_SYS, "syslinux/ldlinux_v4.sys")
RESOURCE(IDR_SL_LDLINUX_V6_BSS, "syslinux/ldlinux_v6.bss")
RESOURCE(IDR_SL_LDLINUX_V6_SYS, "syslinux/ldlinux_v6.sys")
RESOURCE(IDR_SL_MBOOT_C32, "syslinux/mboot.c32")
RESOURCE(IDR_GR_GRUB_GRLDR_MBR, "grub/grldr.mbr")
RESOURCE(IDR_GR_GRUB2_CORE_IMG, "grub2/core.img")
RESOURCE(IDR_SBR_MSG, "mbr/msg.txt")
RESOURCE(IDR_FD_COMMAND_COM, "freedos/COMMAND.COM")
RESOURCE(IDR_FD_KERNEL_SYS, "freedos/KERNEL.SYS")
RESOURCE(IDR_FD_DISPLAY_EXE, "freedos/DISPLAY.EXE")
RESOURCE(IDR_FD_KEYB_EXE, "freedos/KEYB.EXE")
RESOURCE(IDR_FD_MODE_COM, "freedos/MODE.COM")
RESOURCE(IDR_FD_KB1_SYS, "freedos/KEYBOARD.SYS")
RESOURCE(IDR_FD_KB2_SYS, "freedos/KEYBRD2.SYS")
RESOURCE(IDR_FD_KB3_SYS, "freedos/KEYBRD3.SYS")
RESOURCE(IDR_FD_KB4_SYS, "freedos/KEYBRD4.SYS")
RESOURCE(IDR_FD_EGA1_CPX, "freedos/EGA.CPX")
RESOURCE(IDR_FD_EGA2_CPX, "freedos/EGA2.CPX")
RESOURCE(IDR_FD_EGA3_CPX, "freedos/EGA3.CPX")
RESOURCE(IDR_FD_EGA4_CPX, "freedos/EGA4.CPX")
RESOURCE(IDR_FD_EGA5_CPX, "freedos/EGA5.CPX")
RESOURCE(IDR_FD_EGA6_CPX, "freedos/EGA6.CPX")
RESOURCE(IDR_FD_EGA7_CPX, "freedos/EGA7.CPX")
RESOURCE(IDR_FD_EGA8_CPX, "freedos/EGA8.CPX")
RESOURCE(IDR_FD_EGA9_CPX, "freedos/EGA9.CPX")
RESOURCE(IDR_FD_EGA10_CPX, "freedos/EGA10.CPX")
RESOURCE(IDR_FD_EGA11_CPX, "freedos/EGA11.CPX")
RESOURCE(IDR_FD_EGA12_CPX, "freedos/EGA12.CPX")
RESOURCE(IDR_FD_EGA13_CPX, "freedos/EGA13.CPX")
RESOURCE(IDR_FD_EGA14_CPX, "freedos/EGA14.CPX")
RESOURCE(IDR_FD_EGA15_CPX, "freedos/EGA15.CPX")
RESOURCE(IDR_FD_EGA16_CPX, "freedos/EGA16.CPX")
RESOURCE(IDR_FD_EGA17_CPX, "freedos/EGA17.CPX")
RESOURCE(IDR_FD_EGA18_CPX, "freedos/EGA18.CPX")
RESOURCE(IDR_UEFI_NTFS, "uefi/uefi-ntfs.img")
RESOURCE(IDR_SETUP_X64, "setup/setup_x64.exe")
RESOURCE(IDR_SETUP_ARM64, "setup/setup_arm64.exe")
RESOURCE(IDR_MD5_BOOTIA32, "md5/bootia32.efi")
RESOURCE(IDR_MD5_BOOTX64, "md5/bootx64.efi")
RESOURCE(IDR_MD5_BOOTARM, "md5/bootarm.efi")
RESOURCE(IDR_MD5_BOOTAA64, "md5/bootaa64.efi")
RESOURCE(IDR_MD5_BOOTRISCV64, "md5/bootriscv64.efi")
RESOURCE(IDR_MD5_BOOTLOONGARCH64, "md5/bootloongarch64.efi")
RESOURCE(IDR_DBX_IA32, "dbx/dbx_ia32.efiauth2")
RESOURCE(IDR_DBX_X64, "dbx/dbx_x64.efiauth2")
RESOURCE(IDR_DBX_ARM, "dbx/dbx_arm.efiauth2")
RESOURCE(IDR_DBX_AA64, "dbx/dbx_aarch64.efiauth2")
RESOURCE(IDR_DBX_IA64, "dbx/dbx_ia64.efiauth2")
RESOURCE(IDR_DBX_RISCV64, "dbx/dbx_riscv64.efiauth2")
RESOURCE(IDR_DBX_LOONGARCH64, "dbx/dbx_loongarch64.efiauth2")
RESOURCE(IDR_RUFUS_ICON, "icons/rufus.ico")
