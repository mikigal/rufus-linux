# Rufus: The Reliable USB Formatting Utility
# The RPM of Fedora and openSUSE
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# The version of packaging/version.sh (rpmbuild --define "rufus_version ...")
%{!?rufus_version: %global rufus_version 4.16.0}

Name:           rufus
Version:        %{rufus_version}
Release:        1%{?dist}
Summary:        Create bootable USB drives from ISO and disk images
License:        GPL-3.0-or-later
URL:            https://github.com/mikigal/rufus-linux
Source0:        %{url}/releases/download/v%{version}/rufus-linux-%{version}.tar.gz
ExclusiveArch:  x86_64

BuildRequires:  cmake >= 3.25
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  cmake(Qt6Widgets) >= 6.5
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  pkgconfig(com_err)
BuildRequires:  pkgconfig(ext2fs)
BuildRequires:  pkgconfig(hivex)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(openssl)
%if 0%{?suse_version}
BuildRequires:  ninja
%else
BuildRequires:  ninja-build
%endif

Requires:       dosfstools
Requires:       hicolor-icon-theme
Requires:       ntfsprogs
Requires:       polkit
%if 0%{?suse_version}
Requires:       qt6-wayland
Requires:       udev
%else
Requires:       qt6-qtwayland
Requires:       systemd-udev
%endif
Recommends:     exfatprogs
Recommends:     udftools
Recommends:     xorriso

%description
Rufus is a utility that helps format and create bootable USB flash drives, from
Windows and Linux ISO images, disk images (compressed or not, VHD and VHDX) or
FreeDOS. It can customize the installation of Windows 11, download Windows ISOs,
add a persistent partition to Linux live drives and check drives for bad blocks.

This is an unofficial port of Rufus to Linux, with a Qt 6 user interface that
runs natively under Wayland, and a command line interface (rufus-cli). It is not
affiliated with or supported by the upstream Rufus project.

%prep
%autosetup -n rufus-linux-%{version}

# The unit tests need tools that the build does not (mtools, xorriso, qemu-img...): CI runs them
%build
export CFLAGS="%{optflags}" CXXFLAGS="%{optflags}" LDFLAGS="%{?build_ldflags}"
cmake -S . -B rpm-build -G Ninja \
	-DCMAKE_BUILD_TYPE=None \
	-DCMAKE_INSTALL_PREFIX=%{_prefix} \
	-DCMAKE_INSTALL_LIBEXECDIR=%{_libexecdir} \
	-DCMAKE_INSTALL_MANDIR=%{_mandir} \
	-DRUFUS_BUILD_TESTS=OFF
cmake --build rpm-build %{?_smp_mflags}

%install
DESTDIR=%{buildroot} cmake --install rpm-build

%files
%license LICENSE.txt
%doc README.md
%{_bindir}/rufus
%{_bindir}/rufus-cli
%{_libexecdir}/rufus-helper
%{_datadir}/rufus/
%{_datadir}/applications/rufus.desktop
%dir %{_datadir}/metainfo
%{_datadir}/metainfo/io.github.mikigal.rufus_linux.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/rufus.png
%{_datadir}/polkit-1/actions/ie.rufus.helper.policy
%{_mandir}/man1/rufus.1*
%{_mandir}/man1/rufus-cli.1*

%changelog
* Tue Oct 27 2026 mikigal <mikigal.priv@gmail.com> - 4.16.0-1
- The first release of the Linux port, based on Rufus 4.16
