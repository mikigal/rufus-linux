/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit tests for the hashes, the embedded resources and the PKI functions
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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include "rufus.h"
#include "resource.h"
#include "options.h"
#include "ui_iface.h"
#include "winpe.h"
#include "test.h"

extern BOOL cpu_has_sha1_accel, cpu_has_sha256_accel, enable_extra_hashes, expert_mode;
extern uint32_t hash_count[HASH_MAX];
extern StrArray modified_files;
extern sbat_entry_t* sbat_entries;
extern thumbprint_list_t *sb_active_certs, *sb_revoked_certs;
extern char *sbat_level_txt, *sb_active_txt, *sb_revoked_txt;

static char tmp_dir[] = "/tmp/rufus-test-hash-XXXXXX";
static int events[UM_HASH_COMPLETED + 1];

static void test_log(const char* text) { }
static void test_event(enum ui_event_type type, intptr_t param) { events[type]++; }

static char* tmp_path(const char* name)
{
	static char path[4][MAX_PATH];
	static int i = 0;
	char* p = path[i++ % 4];

	snprintf(p, MAX_PATH, "%s/%s", tmp_dir, name);
	return p;
}

static char* to_hex(const uint8_t* buf, size_t len)
{
	static char str[2 * MAX_HASHSIZE + 1];
	size_t i;

	for (i = 0; i < len; i++)
		sprintf(&str[2 * i], "%02x", buf[i]);
	return str;
}

/*
 * Message digests: the vectors of TestHashes() (hash.c)
 */
static const char test_msg[] = "Did you ever hear the tragedy of Darth Plagueis The Wise? "
	"I thought not. It's not a story the Jedi would tell you. It's a Sith legend. "
	"Darth Plagueis was a Dark Lord of the Sith, so powerful and so wise he could "
	"use the Force to influence the midichlorians to create life... He had such a "
	"knowledge of the dark side that he could even keep the ones he cared about "
	"from dying. The dark side of the Force is a pathway to many abilities some "
	"consider to be unnatural. He became so powerful... the only thing he was afraid "
	"of was losing his power, which eventually, of course, he did. Unfortunately, "
	"he taught his apprentice everything he knew, then his apprentice killed him "
	"in his sleep. Ironic. He could save others from death, but not himself.";

static const char* test_hash[HASH_MAX][4] = {
	{
		"d41d8cd98f00b204e9800998ecf8427e",
		"74cac558072300385f7ab4dff7465e3c",
		"f99d37d3bee20f9c0ca3204991be2698",
		"e0ea372ac14a3574167543b851d4babb"
	}, {
		"da39a3ee5e6b4b0d3255bfef95601890afd80709",
		"a5bac908bf3e51ff0036a94d43b4f3bd2d01a75d",
		"8aa6c0064b013b8a6f4e88a0421d39bbf07e2e1b",
		"09463ec0b5917706c9cb1d6b164b2582c04018e0"
	}, {
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		"62c1a97974dfe6848942794be4f2f027b5f4815e1eb76db63a30f0e290b5c1c4",
		"dbca61af192edba49ea215c49a23feee302c98cc4d2c018347fe78db572f07a5",
		"c9b43c1058bc7f7661619e9d983fc9d31356e97f9195a2405ab972d0737b11bf"
	}, {
		"cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
		"4913ace12f1169e5a5f524ef87ab8fc39dff0418851fbbbb1f609d3261b2b4072bd1746e6accb91bf38f3b1b3d59b0a60af5de67aab87b76c2456fde523efc1c",
		"33df8a16dd624cbc4613b5ae902b722411c7e90f37dd3947c9a86e01c51ada68fcf5a0cd4ca928d7cc1ed469bb34c2ed008af069d8b28cc4512e6c8b2e7a5592",
		"999b4eae14de584cce5fa5962b768beda076b06df00d384bb502c6389df8159c006a5b94d1324f47e8d7bd2efe9d8d3dc1fa1429798e49826987ab5ae7ed5c21"
	},
};

static void test_digests(void)
{
	const uint32_t blocksize[HASH_MAX] = { MD5_BLOCKSIZE, SHA1_BLOCKSIZE, SHA256_BLOCKSIZE, SHA512_BLOCKSIZE };
	const EVP_MD* md[HASH_MAX] = { EVP_md5(), EVP_sha1(), EVP_sha256(), EVP_sha512() };
	uint8_t hash[MAX_HASHSIZE], ref[EVP_MAX_MD_SIZE], *big;
	size_t len[4], i, big_len = 3 * MB + 17;
	int j, accel, pass;

	len[0] = 0;
	len[1] = 3;
	for (accel = 0; accel < 2; accel++) {
		// Both the C and the CPU accelerated (SHA-NI) implementations, where available
		cpu_has_sha1_accel = accel && DetectSHA1Acceleration();
		cpu_has_sha256_accel = accel && DetectSHA256Acceleration();
		for (j = 0; j < HASH_MAX; j++) {
			// Designed to test the case where we pad into the total message length area
			len[2] = blocksize[j] - (blocksize[j] >> 3);
			len[3] = strlen(test_msg);
			for (i = 0; i < 4; i++) {
				CHECK(HashBuffer(j, (const uint8_t*)test_msg, len[i], hash));
				CHECK_EQ_STR(to_hex(hash, hash_count[j]), test_hash[j][i]);
			}
		}
	}

	// A large buffer, against OpenSSL
	big = malloc(big_len);
	CHECK(big != NULL);
	if (big == NULL)
		return;
	for (i = 0; i < big_len; i++)
		big[i] = (uint8_t)(i * 7 + (i >> 9));
	for (pass = 0; pass < 2; pass++) {
		cpu_has_sha1_accel = pass && DetectSHA1Acceleration();
		cpu_has_sha256_accel = pass && DetectSHA256Acceleration();
		for (j = 0; j < HASH_MAX; j++) {
			unsigned int ref_len = 0;
			CHECK(HashBuffer(j, big, big_len, hash));
			CHECK(EVP_Digest(big, big_len, ref, &ref_len, md[j], NULL));
			CHECK_EQ_INT(ref_len, hash_count[j]);
			CHECK(memcmp(hash, ref, ref_len) == 0);
		}
	}

	// Files, and the hash comparison helpers
	write_file(tmp_path("big.bin"), big, (uint32_t)big_len);
	CHECK(HashFile(HASH_SHA256, tmp_path("big.bin"), hash));
	EVP_Digest(big, big_len, ref, NULL, EVP_sha256(), NULL);
	CHECK(memcmp(hash, ref, SHA256_HASHSIZE) == 0);
	CHECK(FileMatchesHash(tmp_path("big.bin"), to_hex(ref, SHA256_HASHSIZE)));
	CHECK(BufferMatchesHash(big, big_len, to_hex(ref, SHA256_HASHSIZE)));
	ref[0] ^= 1;
	CHECK(!FileMatchesHash(tmp_path("big.bin"), to_hex(ref, SHA256_HASHSIZE)));
	CHECK(!HashFile(HASH_SHA256, tmp_path("nonexistent"), hash));
	ErrorStatus = 0;

	// HashThread(), which hashes image_path with all the algorithms in parallel
	image_path = tmp_path("big.bin");
	img_report.image_size = big_len;
	enable_extra_hashes = TRUE;
	memset(events, 0, sizeof(events));
	CHECK(HashThread(NULL) == NULL);
	CHECK_EQ_INT(events[UM_FORMAT_COMPLETED], 1);
	CHECK_EQ_INT(events[UM_HASH_COMPLETED], 1);
	for (j = 0; j < HASH_MAX; j++) {
		EVP_Digest(big, big_len, ref, NULL, md[j], NULL);
		CHECK_EQ_STR(hash_str[j], to_hex(ref, hash_count[j]));
	}
	// Empty file
	write_file(tmp_path("empty.bin"), (const uint8_t*)"", 0);
	IGNORE_RETVAL(truncate(tmp_path("empty.bin"), 0));
	image_path = tmp_path("empty.bin");
	CHECK(HashThread(NULL) == NULL);
	CHECK_EQ_STR(hash_str[HASH_SHA256], test_hash[HASH_SHA256][0]);
	image_path = NULL;
	free(big);
}

/*
 * Embedded resources: each one must match its file in res/
 */
static void test_resources(void)
{
	static const struct { int id; const char* path; } res[] = {
#define RESOURCE(id, file) { id, file },
#include "resources.h"
#undef RESOURCE
	};
	const char* src = getenv("RUFUS_SOURCE_DIR");
	char path[MAX_PATH];
	uint8_t *buf = NULL, *data;
	DWORD len, size;
	size_t i;

	for (i = 0; i < ARRAYSIZE(res); i++) {
		snprintf(path, sizeof(path), "%s/res/%s", (src == NULL) ? "." : src, res[i].path);
		size = read_file(path, &buf);
		len = 0;
		data = GetResource(res[i].id, res[i].path, &len, FALSE);
		CHECK(data != NULL);
		CHECK_EQ_INT(len, size);
		if ((data != NULL) && (buf != NULL))
			CHECK(memcmp(data, buf, size) == 0);
		CHECK_EQ_INT(GetResourceSize(res[i].id, res[i].path), size);
		safe_free(buf);
	}
	// Duplicated, with a larger buffer
	len = 64 * KB;
	data = GetResource(IDR_SL_MBOOT_C32, "mboot.c32", &len, TRUE);
	CHECK(data != NULL);
	CHECK(len < 64 * KB);
	free(data);
	CHECK(GetResource(12345, "missing", &len, FALSE) == NULL);
	// The DBX of each architecture, and the hash DB
	len = 0;
	CHECK(GetResource(IDR_DBX_X64, "dbx_x64.bin", &len, FALSE) != NULL && len > 1 * KB);
	len = 0;
	data = GetResource(IDR_SL_LDLINUX_V6_SYS, "ldlinux.sys", &len, FALSE);
	CHECK(IsBufferInDB(data, len));
	CHECK(!IsBufferInDB((const uint8_t*)test_msg, sizeof(test_msg)));
}

/*
 * PE executables: Authenticode hash, signatures and revocation
 */

static EVP_PKEY* ca_key, *leaf_key;
static X509 *ca_cert, *leaf_cert;

static X509* make_cert(const char* cn, EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key, long serial)
{
	X509* cert = X509_new();
	X509_NAME* name;

	X509_set_version(cert, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
	X509_gmtime_adj(X509_getm_notBefore(cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
	X509_set_pubkey(cert, key);
	name = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)"Rufus Tests", -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char*)cn, -1, -1, 0);
	X509_set_issuer_name(cert, (issuer == NULL) ? name : X509_get_subject_name(issuer));
	X509_sign(cert, (issuer_key == NULL) ? key : issuer_key, EVP_sha256());
	return cert;
}

// The DER signature of 'data', by the leaf certificate, optionally with the CA certificate
static uint8_t* make_signature(const uint8_t* data, size_t len, BOOL with_ca, int* sig_len)
{
	STACK_OF(X509)* certs = sk_X509_new_null();
	BIO* bio = BIO_new_mem_buf(data, (int)len);
	PKCS7* p7;
	uint8_t* der = NULL;

	if (with_ca)
		sk_X509_push(certs, ca_cert);
	p7 = PKCS7_sign(leaf_cert, leaf_key, certs, bio, PKCS7_BINARY | PKCS7_DETACHED);
	*sig_len = i2d_PKCS7(p7, &der);
	PKCS7_free(p7);
	BIO_free(bio);
	sk_X509_free(certs);
	return der;
}

#define PE_HDR_SIZE     0x400
#define PE_SECTION_SIZE 0x200

// Build an x64 PE with a .text and a .sbat section, signed or not
static uint8_t* make_pe(const char* sbat, BOOL sign, BOOL with_ca, uint32_t* pe_len)
{
	uint32_t len = PE_HDR_SIZE + 2 * PE_SECTION_SIZE + 0x40, cert_off, cert_len = 0;
	uint8_t* pe = calloc(1, len + 16 * KB), *sig = NULL;
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)pe;
	IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)&pe[0x80];
	IMAGE_SECTION_HEADER* sec = (IMAGE_SECTION_HEADER*)&nt[1];
	WIN_CERTIFICATE* cert;
	int i, sig_len;

	dos->e_magic = IMAGE_DOS_SIGNATURE;
	dos->e_lfanew = 0x80;
	nt->Signature = IMAGE_NT_SIGNATURE;
	nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
	nt->FileHeader.NumberOfSections = 2;
	nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
	nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
	nt->OptionalHeader.FileAlignment = 0x200;
	nt->OptionalHeader.SectionAlignment = 0x1000;
	nt->OptionalHeader.SizeOfHeaders = PE_HDR_SIZE;
	nt->OptionalHeader.Subsystem = 10;	// EFI application
	nt->OptionalHeader.CheckSum = 0x12345678;
	nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
	memcpy(sec[0].Name, ".text", 5);
	sec[0].VirtualAddress = 0x1000;
	sec[0].Misc.VirtualSize = PE_SECTION_SIZE;
	sec[0].PointerToRawData = PE_HDR_SIZE + PE_SECTION_SIZE;	// NB: not in file order
	sec[0].SizeOfRawData = PE_SECTION_SIZE;
	memcpy(sec[1].Name, ".sbat", 5);
	sec[1].VirtualAddress = 0x2000;
	sec[1].Misc.VirtualSize = PE_SECTION_SIZE;
	sec[1].PointerToRawData = PE_HDR_SIZE;
	sec[1].SizeOfRawData = PE_SECTION_SIZE;
	for (i = 0; i < PE_SECTION_SIZE; i++)
		pe[sec[0].PointerToRawData + i] = (uint8_t)(i ^ 0x5a);
	strcpy((char*)&pe[sec[1].PointerToRawData], sbat);
	// Some extra data after the sections, which is part of the hash
	memcpy(&pe[PE_HDR_SIZE + 2 * PE_SECTION_SIZE], "trailing data", 13);

	if (sign) {
		// The signed content does not matter here
		sig = make_signature(pe, 64, with_ca, &sig_len);
		cert_off = len;
		cert_len = (uint32_t)((offsetof(WIN_CERTIFICATE, bCertificate) + sig_len + 7) & ~7);
		cert = (WIN_CERTIFICATE*)&pe[cert_off];
		cert->dwLength = (DWORD)(offsetof(WIN_CERTIFICATE, bCertificate) + sig_len);
		cert->wRevision = 0x0200;
		cert->wCertificateType = WIN_CERT_TYPE_PKCS_SIGNED_DATA;
		memcpy(cert->bCertificate, sig, sig_len);
		OPENSSL_free(sig);
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].VirtualAddress = cert_off;
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size = cert_len;
		len += cert_len;
	}
	*pe_len = len;
	return pe;
}

// The Authenticode hash, as the specification defines it, computed with OpenSSL
static void reference_pe256(const uint8_t* pe, uint32_t len, uint8_t* hash)
{
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)&pe[0x80];
	const uint8_t* checksum = (const uint8_t*)&nt->OptionalHeader.CheckSum;
	const uint8_t* ctdir = (const uint8_t*)&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
	uint32_t authsz = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size;
	uint32_t end_of_sections = PE_HDR_SIZE + 2 * PE_SECTION_SIZE;
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();

	EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
	EVP_DigestUpdate(ctx, pe, checksum - pe);
	EVP_DigestUpdate(ctx, checksum + 4, ctdir - (checksum + 4));
	EVP_DigestUpdate(ctx, ctdir + 8, &pe[PE_HDR_SIZE] - (ctdir + 8));
	// Sections, in file order
	EVP_DigestUpdate(ctx, &pe[PE_HDR_SIZE], 2 * PE_SECTION_SIZE);
	EVP_DigestUpdate(ctx, &pe[end_of_sections], len - authsz - end_of_sections);
	EVP_DigestFinal_ex(ctx, hash, NULL);
	EVP_MD_CTX_free(ctx);
}

static void test_pe(void)
{
	static const char sbat_ok[] = "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
		"shim,4,UEFI shim,shim,1,https://github.com/rhboot/shim\n";
	static const char sbat_old[] = "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
		"shim,3,UEFI shim,shim,1,https://github.com/rhboot/shim\n";
	uint8_t *pe, hash[SHA256_HASHSIZE], ref[SHA256_HASHSIZE], ca_thumbprint[SHA1_HASHSIZE];
	uint32_t len, sbat_len;
	unsigned int n = SHA1_HASHSIZE;
	cert_info_t info;
	char ca_list[2 * SHA1_HASHSIZE + 1];

	ca_key = EVP_RSA_gen(2048);
	leaf_key = EVP_RSA_gen(2048);
	ca_cert = make_cert("Rufus Test Secure Boot CA", ca_key, NULL, NULL, 1);
	leaf_cert = make_cert("Rufus Test Bootloader Signer", leaf_key, ca_cert, ca_key, 2);
	X509_digest(ca_cert, EVP_sha1(), ca_thumbprint, &n);
	static_strcpy(ca_list, to_hex(ca_thumbprint, SHA1_HASHSIZE));

	/* Unsigned */
	pe = make_pe(sbat_ok, FALSE, FALSE, &len);
	CHECK_EQ_INT(MachineToArch(GetPeArch(pe)), ARCH_X86_64);
	CHECK(GetPeSection(pe, ".sbat", &sbat_len) == &pe[PE_HDR_SIZE]);
	CHECK_EQ_INT(sbat_len, PE_SECTION_SIZE);
	CHECK(GetPeSection(pe, ".data", NULL) == NULL);
	CHECK(GetPeSignatureData(pe) == NULL);
	CHECK_EQ_INT(GetIssuerCertificateInfo(GetPeSignatureData(pe), &info), 0);
	CHECK(PE256Buffer(pe, len, hash));
	reference_pe256(pe, len, ref);
	CHECK(memcmp(hash, ref, SHA256_HASHSIZE) == 0);
	CHECK_EQ_INT(IsBootloaderRevoked(pe, len), 0);
	CHECK(!IsSignedBySecureBootAuthority(pe, len));
	free(pe);

	/* Signed, with the issuer in the signature */
	pe = make_pe(sbat_ok, TRUE, TRUE, &len);
	CHECK(GetPeSignatureData(pe) != NULL);
	CHECK_EQ_INT(GetIssuerCertificateInfo(GetPeSignatureData(pe), &info), 2);
	CHECK_EQ_STR(info.name, "Rufus Test Secure Boot CA");
	CHECK(memcmp(info.thumbprint, ca_thumbprint, SHA1_HASHSIZE) == 0);
	// The checksum and the certificate table are not part of the Authenticode hash
	CHECK(PE256Buffer(pe, len, hash));
	reference_pe256(pe, len, ref);
	CHECK(memcmp(hash, ref, SHA256_HASHSIZE) == 0);
	((IMAGE_NT_HEADERS64*)&pe[0x80])->OptionalHeader.CheckSum ^= 0xffff;
	pe[len - 1] ^= 0xff;
	CHECK(PE256Buffer(pe, len, ref));
	CHECK(memcmp(hash, ref, SHA256_HASHSIZE) == 0);
	pe[PE_HDR_SIZE + PE_SECTION_SIZE + 3] ^= 1;
	CHECK(PE256Buffer(pe, len, ref));
	CHECK(memcmp(hash, ref, SHA256_HASHSIZE) != 0);
	pe[PE_HDR_SIZE + PE_SECTION_SIZE + 3] ^= 1;
	// Not a Secure Boot authority, until it is listed as one
	CHECK(!IsSignedBySecureBootAuthority(pe, len));
	// Replace the embedded defaults (loaded by the previous call)
	safe_free(sb_active_txt);
	safe_free(sb_active_certs);
	sb_active_txt = strdup(ca_list);
	sb_active_certs = GetThumbprintEntries(sb_active_txt);
	CHECK(IsSignedBySecureBootAuthority(pe, len));
	CHECK_EQ_INT(IsBootloaderRevoked(pe, len), 0);
	// Revoked certificate (expert mode only, as upstream)
	safe_free(sb_revoked_txt);
	safe_free(sb_revoked_certs);
	sb_revoked_txt = strdup(ca_list);
	sb_revoked_certs = GetThumbprintEntries(sb_revoked_txt);
	CHECK_EQ_INT(IsBootloaderRevoked(pe, len), 0);
	expert_mode = TRUE;
	CHECK_EQ_INT(IsBootloaderRevoked(pe, len), 5);
	expert_mode = FALSE;
	free(pe);

	/* Signed, without the issuer: the signer is reported */
	pe = make_pe(sbat_ok, TRUE, FALSE, &len);
	CHECK_EQ_INT(GetIssuerCertificateInfo(GetPeSignatureData(pe), &info), 1);
	CHECK_EQ_STR(info.name, "Rufus Test Bootloader Signer");
	CHECK(!IsSignedBySecureBootAuthority(pe, len));
	free(pe);

	/* SBAT revocation, against the embedded sbat_level.txt (shim,4) */
	pe = make_pe(sbat_old, TRUE, TRUE, &len);
	CHECK_EQ_INT(IsBootloaderRevoked(pe, len), 3);
	free(pe);
	CHECK(sbat_entries != NULL);

	/* Garbage */
	CHECK_EQ_INT(IsBootloaderRevoked((uint8_t*)test_hash, sizeof(test_hash)), -2);
	CHECK(!PE256Buffer((uint8_t*)test_msg, 100, hash));

	safe_free(sb_active_txt);
	safe_free(sb_active_certs);
	safe_free(sb_revoked_txt);
	safe_free(sb_revoked_certs);
	safe_free(sbat_level_txt);
	safe_free(sbat_entries);
	X509_free(leaf_cert);
	X509_free(ca_cert);
	EVP_PKEY_free(leaf_key);
	EVP_PKEY_free(ca_key);
}

static void test_rsa(void)
{
	static const char data[] = "version = 4.16.2310\n";
	EVP_PKEY* key = EVP_RSA_gen(2048);
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	BIGNUM* n = NULL;
	uint8_t modulus[257], sig[256], buf[sizeof(data)];
	size_t sig_len = sizeof(sig);
	int modulus_len;

	// Sign as 'openssl dgst -sha256 -sign' does
	CHECK(EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) == 1);
	CHECK(EVP_DigestSign(ctx, sig, &sig_len, (const uint8_t*)data, strlen(data)) == 1);
	CHECK(EVP_PKEY_get_bn_param(key, "n", &n) == 1);
	// With the extra leading 0x00 of the openssl modulus display, as upstream stores it
	modulus[0] = 0;
	modulus_len = BN_bn2bin(n, &modulus[1]) + 1;
	CHECK(ValidateRsaSha256Signature(modulus, modulus_len, (const uint8_t*)data, (DWORD)strlen(data), sig, (DWORD)sig_len));
	sig[10] ^= 1;
	CHECK(!ValidateRsaSha256Signature(modulus, modulus_len, (const uint8_t*)data, (DWORD)strlen(data), sig, (DWORD)sig_len));
	// Our own key did not sign this, and the buffer gets cleared
	memcpy(buf, data, sizeof(buf));
	CHECK(!ValidateOpensslSignature(buf, (DWORD)strlen(data), sig, (DWORD)sig_len));
	CHECK(buf[0] == 0 && buf[5] == 0);
	BN_free(n);
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(key);
}

/*
 * md5sum.txt updates, on a destination whose case differs from the md5sum entries
 */
static void test_md5sum(void)
{
	char md5[512], path[MAX_PATH], *data = NULL;
	uint8_t sum[MD5_HASHSIZE];
	uint32_t size;
	DWORD res_len = 0;

	CHECK(mkdir_p(tmp_path("dest/boot/grub"), 0755) == 0);
	CHECK(mkdir_p(tmp_path("dest/EFI/BOOT"), 0755) == 0);
	write_file(tmp_path("dest/boot/grub/grub.cfg"), (const uint8_t*)"linux /casper/vmlinuz persistent\n", 33);
	write_file(tmp_path("dest/EFI/BOOT/BOOTX64.EFI"), (const uint8_t*)"original", 8);
	snprintf(md5, sizeof(md5), "%s  ./boot/grub/grub.cfg\n%s  ./efi/boot/bootx64.efi\n",
		"00000000000000000000000000000000", "11111111111111111111111111111111");
	write_file(tmp_path("dest/md5sum.txt"), (const uint8_t*)md5, (uint32_t)strlen(md5));

	// Only the modified file is updated
	img_report.has_md5sum = 1;
	opts.validate_md5sum = FALSE;
	StrArrayCreate(&modified_files, 4);
	StrArrayAdd(&modified_files, tmp_path("dest/boot/grub/grub.cfg"), TRUE);
	UpdateMD5Sum(tmp_path("dest"), "md5sum.txt");
	size = read_file(tmp_path("dest/md5sum.txt"), (uint8_t**)&data);
	CHECK(size == strlen(md5));
	HashFile(HASH_MD5, tmp_path("dest/boot/grub/grub.cfg"), sum);
	if (data != NULL) {
		CHECK(strncmp(data, to_hex(sum, MD5_HASHSIZE), 32) == 0);
		CHECK(strstr(data, "11111111111111111111111111111111  ./efi/boot/bootx64.efi\n") != NULL);
	}
	safe_free(data);

	// With runtime validation, our md5sum bootloader replaces the original one
	opts.validate_md5sum = TRUE;
	StrArrayClear(&modified_files);
	UpdateMD5Sum(tmp_path("dest"), "md5sum.txt");
	snprintf(path, sizeof(path), "%s", tmp_path("dest/EFI/BOOT/BOOTX64_original.efi"));
	size = read_file(path, (uint8_t**)&data);
	CHECK_EQ_INT(size, 8);
	safe_free(data);
	GetResource(IDR_MD5_BOOTX64, "bootx64.efi", &res_len, FALSE);
	size = read_file(tmp_path("dest/EFI/BOOT/BOOTX64.EFI"), (uint8_t**)&data);
	CHECK_EQ_INT(size, res_len);
	safe_free(data);
	size = read_file(tmp_path("dest/md5sum.txt"), (uint8_t**)&data);
	if (data != NULL)
		CHECK(strstr(data, "11111111111111111111111111111111  ./efi/boot/bootx64_original.efi\n") != NULL);
	safe_free(data);
	StrArrayDestroy(&modified_files);
	opts.validate_md5sum = FALSE;
}

static void test_path_case(void)
{
	char path[MAX_PATH];

	CHECK(mkdir_p(tmp_path("Case/Sub Dir"), 0755) == 0);
	write_file(tmp_path("Case/Sub Dir/File.TXT"), (const uint8_t*)"x", 1);
	static_strcpy(path, tmp_path("case/sub dir/file.txt"));
	CHECK(PathFileExistsNoCase(path));
	CHECK_EQ_STR(path, tmp_path("Case/Sub Dir/File.TXT"));
	static_strcpy(path, tmp_path("case/missing/file.txt"));
	CHECK(!PathFileExistsNoCase(path));
	CHECK(copy_file(tmp_path("Case/Sub Dir/File.TXT"), tmp_path("copy.txt"), TRUE));
	CHECK(!copy_file(tmp_path("Case/Sub Dir/File.TXT"), tmp_path("copy.txt"), TRUE));
	CHECK(copy_file(tmp_path("Case/Sub Dir/File.TXT"), tmp_path("copy.txt"), FALSE));
}

int main(void)
{
	const ui_iface_t iface = { .log = test_log, .event = test_event };
	char cmd[64];

	if (mkdtemp(tmp_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	ui_set_iface(&iface);
	ResetOptions(&opts);

	test_digests();
	test_resources();
	test_pe();
	test_rsa();
	test_path_case();
	test_md5sum();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_dir);
	IGNORE_RETVAL(system(cmd));
	return TEST_RESULT();
}
