# Rufus: The Reliable USB Formatting Utility
# The UEFI variables of a Secure Boot enabled OVMF
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Enrolls Secure Boot keys into a copy of the blank variable store of OVMF
(OVMF_VARS.fd), which is what virt-fw-vars does, as distributions do not all
ship an enrolled one: a PK and KEK of the tests (a self-signed certificate that
openssl makes), and a db of the certificates of a test (the Microsoft UEFI CA
2011 that signed UEFI:NTFS, for instance). With these, and the "secboot" code of
OVMF, the firmware enforces Secure Boot.

The store is the authenticated variable store of edk2 (MdeModulePkg
VariableFormat.h): a firmware volume, a variable store header, then the
variables, each with an AUTHENTICATED_VARIABLE_HEADER."""

import os
import struct
import subprocess
import uuid

OVMF_SECBOOT_CANDIDATES = (
    ("/usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
    ("/usr/share/OVMF/OVMF_CODE_4M.secboot.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
    ("/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),
)

AUTHENTICATED_VARIABLE_GUID = uuid.UUID("aaf32c78-947b-439a-a180-2e144ec37792")
EFI_GLOBAL_VARIABLE = uuid.UUID("8be4df61-93ca-11d2-aa0d-00e098032b8c")
EFI_IMAGE_SECURITY_DATABASE = uuid.UUID("d719b2cb-3d3a-4596-a3bc-dad00e67656f")
SECURE_BOOT_ENABLE_GUID = uuid.UUID("f0a30bc7-af08-4556-99c4-001009c93a44")
CUSTOM_MODE_GUID = uuid.UUID("c076ec0c-7028-4399-a072-71ee5c448b9f")
EFI_CERT_X509_GUID = uuid.UUID("a5c059a1-94e4-4aa7-87b5-ab155c2bf072")
# The owner of the signatures of the tests
OWNER_GUID = uuid.UUID("0cd9b97e-1a3c-4f63-9d2e-52a8b1f0c0de")

VARIABLE_START_ID = 0x55AA
VAR_ADDED = 0x3F
EFI_VARIABLE_NON_VOLATILE = 0x01
EFI_VARIABLE_BOOTSERVICE_ACCESS = 0x02
EFI_VARIABLE_RUNTIME_ACCESS = 0x04
EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS = 0x20
AUTH_HEADER = struct.Struct("<HBBIQ16sIII16s")


def ovmf_secboot() -> tuple[str, str] | None:
    """The Secure Boot code of OVMF, with its blank variable store
    (overridable with RUFUS_E2E_OVMF_SECBOOT_CODE and RUFUS_E2E_OVMF_VARS)."""
    code, nvram = os.environ.get("RUFUS_E2E_OVMF_SECBOOT_CODE"), os.environ.get("RUFUS_E2E_OVMF_VARS")
    if code and nvram:
        return code, nvram
    for code, nvram in OVMF_SECBOOT_CANDIDATES:
        if os.path.exists(code) and os.path.exists(nvram):
            return code, nvram
    return None


def test_certificate(work_dir: str) -> bytes:
    """The DER certificate of the PK and KEK of the tests (made once)"""
    der = os.path.join(work_dir, "rufus-test-pk.der")
    if not os.path.exists(der):
        key = os.path.join(work_dir, "rufus-test-pk.key")
        subprocess.run(["openssl", "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
                        "-days", "3650", "-subj", "/CN=Rufus Test Platform Key/", "-keyout", key,
                        "-outform", "DER", "-out", der], check=True, capture_output=True)
    with open(der, "rb") as f:
        return f.read()


def pe_certificates(pe_file: str) -> list[bytes]:
    """The certificates of the Authenticode signature of a PE (as DER)"""
    with open(pe_file, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    directory = pe + 24 + (112 if magic == 0x20B else 96)
    offset, size = struct.unpack_from("<II", data, directory + 8 * 4)
    if size <= 8:
        return []
    # A WIN_CERTIFICATE: length, revision, type, then the PKCS#7 signed data
    pkcs7 = data[offset + 8:offset + size]
    pem = subprocess.run(["openssl", "pkcs7", "-inform", "DER", "-print_certs"], input=pkcs7,
                         capture_output=True, check=True).stdout.decode()
    certs = []
    for block in pem.split("-----END CERTIFICATE-----")[:-1]:
        block = block[block.index("-----BEGIN CERTIFICATE-----"):] + "-----END CERTIFICATE-----\n"
        certs.append(subprocess.run(["openssl", "x509", "-outform", "DER"], input=block.encode(),
                                    capture_output=True, check=True).stdout)
    return certs


def signature_list(certs: list[bytes]) -> bytes:
    """EFI_SIGNATURE_LISTs of X.509 certificates (one per certificate, as their sizes differ)"""
    out = b""
    for cert in certs:
        size = 16 + len(cert)
        out += EFI_CERT_X509_GUID.bytes_le + struct.pack("<III", 28 + size, 0, size)
        out += OWNER_GUID.bytes_le + cert
    return out


def efi_time() -> bytes:
    # 2026-01-01 00:00:00, for the time based authenticated variables
    return struct.pack("<HBBBBBBIhBB", 2026, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0)


def variable(name: str, guid: uuid.UUID, attributes: int, data: bytes) -> bytes:
    name_utf16 = (name + "\0").encode("utf-16-le")
    timestamp = efi_time() if attributes & EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS else bytes(16)
    header = AUTH_HEADER.pack(VARIABLE_START_ID, VAR_ADDED, 0, attributes, 0, timestamp, 0, len(name_utf16),
                              len(data), guid.bytes_le)
    return header + name_utf16 + data


def enroll(template: str, out: str, db_certs: list[bytes], work_dir: str):
    """A copy of a blank variable store, with Secure Boot keys and enabled"""
    with open(template, "rb") as f:
        store = bytearray(f.read())
    if store[40:44] != b"_FVH":
        raise ValueError(f"{template} is not a firmware volume")
    start = struct.unpack_from("<H", store, 48)[0]
    if uuid.UUID(bytes_le=bytes(store[start:start + 16])) != AUTHENTICATED_VARIABLE_GUID:
        raise ValueError(f"{template} has no authenticated variable store")
    end = start + struct.unpack_from("<I", store, start + 16)[0]
    # After the variables that the store already has
    pos = start + 28
    while struct.unpack_from("<H", store, pos)[0] == VARIABLE_START_ID:
        fields = AUTH_HEADER.unpack_from(store, pos)
        pos = (pos + AUTH_HEADER.size + fields[7] + fields[8] + 3) & ~3
    auth = (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
            EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)
    cert = test_certificate(work_dir)
    for var in (variable("PK", EFI_GLOBAL_VARIABLE, auth, signature_list([cert])),
                variable("KEK", EFI_GLOBAL_VARIABLE, auth, signature_list([cert])),
                variable("db", EFI_IMAGE_SECURITY_DATABASE, auth, signature_list(db_certs)),
                variable("SecureBootEnable", SECURE_BOOT_ENABLE_GUID,
                         EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, b"\x01"),
                variable("CustomMode", CUSTOM_MODE_GUID,
                         EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, b"\x00")):
        if pos + len(var) > end:
            raise ValueError("the variable store is full")
        store[pos:pos + len(var)] = var
        pos = (pos + len(var) + 3) & ~3
    with open(out, "wb") as f:
        f.write(store)
