/*
 * Rufus: The Reliable USB Formatting Utility
 * PKI functions (code signing, etc.)
 * Copyright © 2015-2026 Pete Batard <pete@akeo.ie>
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
 * On Windows, these use the CryptoAPI. Here, they use OpenSSL's libcrypto.
 * The Authenticode functions of upstream (GetSignatureName(),
 * GetSignatureTimeStamp() and ValidateSignature(), which check the signature
 * of a downloaded Rufus executable) have no use on Linux and are not ported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "rufus.h"
#include "winpe.h"

// The RSA public key modulus for the private key we use to sign the files on the server.
// NB: This is the big endian modulus that openssl displays, with its extra leading 0x00.
static const uint8_t rsa_pubkey_modulus[] = {
	/*
		$ openssl genrsa -aes256 -out private.pem 2048
		$ openssl rsa -in private.pem -pubout -out public.pem
		$ openssl rsa -pubin -inform PEM -text -noout < public.pem
		Public-Key: (2048 bit)
		Modulus:
			00:b6:40:7d:d1:98:7b:81:9e:be:23:0f:32:5d:55:
			60:c6:bf:b4:41:bb:43:1b:f1:e1:e6:f9:2b:d6:dd:
			11:50:e8:b9:3f:19:97:5e:a7:8b:4a:30:c6:76:58:
			72:1c:ac:ff:a1:f8:96:6c:51:5d:13:11:e3:5b:11:
			82:f5:9a:69:e4:28:97:0f:ca:1f:02:ea:1f:7d:dc:
			f9:fc:79:2f:61:ff:8e:45:60:65:ba:37:9b:de:49:
			05:6a:a8:fd:70:d0:0c:79:b6:d7:81:aa:54:c3:c6:
			4a:87:a0:45:ee:ca:d5:d5:c5:c2:ac:86:42:b3:58:
			27:d2:43:b9:37:f2:e6:75:66:17:53:d0:38:d0:c6:
			57:c2:55:36:a2:43:87:ea:24:f0:96:ec:34:dd:79:
			4d:80:54:9d:84:81:a7:cf:0c:a5:7c:d6:63:fa:7a:
			66:30:a9:50:ee:f0:e5:f8:a2:2d:ac:fc:24:21:fe:
			ef:e8:d3:6f:0e:27:b0:64:22:95:3e:6d:a6:66:97:
			c6:98:c2:47:b3:98:69:4d:b1:b5:d3:6f:43:f5:d7:
			a5:13:5e:8c:28:4f:62:4e:01:48:0a:63:89:e7:ca:
			34:aa:7d:2f:bb:70:e0:31:bb:39:49:a3:d2:c9:2e:
			a6:30:54:9a:5c:4d:58:17:d9:fc:3a:43:e6:8e:2a:
			18:e9
		Exponent: 65537 (0x10001)
	*/
	0x00, 0xb6, 0x40, 0x7d, 0xd1, 0x98, 0x7b, 0x81, 0x9e, 0xbe, 0x23, 0x0f, 0x32, 0x5d, 0x55,
	0x60, 0xc6, 0xbf, 0xb4, 0x41, 0xbb, 0x43, 0x1b, 0xf1, 0xe1, 0xe6, 0xf9, 0x2b, 0xd6, 0xdd,
	0x11, 0x50, 0xe8, 0xb9, 0x3f, 0x19, 0x97, 0x5e, 0xa7, 0x8b, 0x4a, 0x30, 0xc6, 0x76, 0x58,
	0x72, 0x1c, 0xac, 0xff, 0xa1, 0xf8, 0x96, 0x6c, 0x51, 0x5d, 0x13, 0x11, 0xe3, 0x5b, 0x11,
	0x82, 0xf5, 0x9a, 0x69, 0xe4, 0x28, 0x97, 0x0f, 0xca, 0x1f, 0x02, 0xea, 0x1f, 0x7d, 0xdc,
	0xf9, 0xfc, 0x79, 0x2f, 0x61, 0xff, 0x8e, 0x45, 0x60, 0x65, 0xba, 0x37, 0x9b, 0xde, 0x49,
	0x05, 0x6a, 0xa8, 0xfd, 0x70, 0xd0, 0x0c, 0x79, 0xb6, 0xd7, 0x81, 0xaa, 0x54, 0xc3, 0xc6,
	0x4a, 0x87, 0xa0, 0x45, 0xee, 0xca, 0xd5, 0xd5, 0xc5, 0xc2, 0xac, 0x86, 0x42, 0xb3, 0x58,
	0x27, 0xd2, 0x43, 0xb9, 0x37, 0xf2, 0xe6, 0x75, 0x66, 0x17, 0x53, 0xd0, 0x38, 0xd0, 0xc6,
	0x57, 0xc2, 0x55, 0x36, 0xa2, 0x43, 0x87, 0xea, 0x24, 0xf0, 0x96, 0xec, 0x34, 0xdd, 0x79,
	0x4d, 0x80, 0x54, 0x9d, 0x84, 0x81, 0xa7, 0xcf, 0x0c, 0xa5, 0x7c, 0xd6, 0x63, 0xfa, 0x7a,
	0x66, 0x30, 0xa9, 0x50, 0xee, 0xf0, 0xe5, 0xf8, 0xa2, 0x2d, 0xac, 0xfc, 0x24, 0x21, 0xfe,
	0xef, 0xe8, 0xd3, 0x6f, 0x0e, 0x27, 0xb0, 0x64, 0x22, 0x95, 0x3e, 0x6d, 0xa6, 0x66, 0x97,
	0xc6, 0x98, 0xc2, 0x47, 0xb3, 0x98, 0x69, 0x4d, 0xb1, 0xb5, 0xd3, 0x6f, 0x43, 0xf5, 0xd7,
	0xa5, 0x13, 0x5e, 0x8c, 0x28, 0x4f, 0x62, 0x4e, 0x01, 0x48, 0x0a, 0x63, 0x89, 0xe7, 0xca,
	0x34, 0xaa, 0x7d, 0x2f, 0xbb, 0x70, 0xe0, 0x31, 0xbb, 0x39, 0x49, 0xa3, 0xd2, 0xc9, 0x2e,
	0xa6, 0x30, 0x54, 0x9a, 0x5c, 0x4d, 0x58, 0x17, 0xd9, 0xfc, 0x3a, 0x43, 0xe6, 0x8e, 0x2a,
	0x18, 0xe9
};

static const char* OpensslErrorString(void)
{
	static _Thread_local char err_string[256];
	unsigned long err = ERR_get_error();

	if (err == 0)
		return "Unknown error";
	ERR_error_string_n(err, err_string, sizeof(err_string));
	ERR_clear_error();
	return err_string;
}

// Fills the certificate's name and thumbprint.
// Tries the issuer first, and if none is available, falls back to current cert.
// Returns 0 for unsigned, -1 on error, 1 for signer or 2 for issuer.
// NB: As upstream, a signature that cannot be parsed is reported as unsigned. Also, the
// issuer can only be found if the signature includes it (Windows can also find it in the
// system certificate stores), which is the case for the Secure Boot authorities.
int GetIssuerCertificateInfo(uint8_t* cert, cert_info_t* info)
{
	int i, ret = 0, CertIndex = 0, nid_pos;
	WIN_CERTIFICATE* pWinCert = (WIN_CERTIFICATE*)cert;
	const unsigned char* p;
	PKCS7* p7 = NULL;
	STACK_OF(X509)* signers = NULL, *certs;
	X509* pCert[2] = { NULL, NULL }, *candidate;
	X509_NAME_ENTRY* entry;
	unsigned char* name = NULL;
	unsigned int size;
	int name_len;

	if (info == NULL)
		return -1;
	if (pWinCert == NULL || pWinCert->dwLength == 0)
		return 0;
	if (pWinCert->dwLength <= offsetof(WIN_CERTIFICATE, bCertificate))
		return 0;

	// Get the PKCS#7 signed data from the signed file
	p = pWinCert->bCertificate;
	p7 = d2i_PKCS7(NULL, &p, (long)(pWinCert->dwLength - offsetof(WIN_CERTIFICATE, bCertificate)));
	if ((p7 == NULL) || !PKCS7_type_is_signed(p7)) {
		uprintf("PKI: Failed to get signature: %s", OpensslErrorString());
		goto out;
	}

	// Search for the signer certificate in the certificates of the signature
	signers = PKCS7_get0_signers(p7, NULL, 0);
	if ((signers == NULL) || (sk_X509_num(signers) < 1)) {
		uprintf("PKI: Failed to locate signer certificate: %s", OpensslErrorString());
		goto out;
	}
	pCert[0] = sk_X509_value(signers, 0);

	// Try to get the issuer's certificate if available
	certs = p7->d.sign->cert;
	for (i = 0; i < sk_X509_num(certs); i++) {
		candidate = sk_X509_value(certs, i);
		if ((X509_cmp(candidate, pCert[0]) != 0) && (X509_check_issued(candidate, pCert[0]) == X509_V_OK)) {
			pCert[1] = candidate;
			CertIndex = 1;
			break;
		}
	}

	// Isolate the signing certificate subject name
	nid_pos = X509_NAME_get_index_by_NID(X509_get_subject_name(pCert[CertIndex]), NID_commonName, -1);
	entry = (nid_pos < 0) ? NULL : X509_NAME_get_entry(X509_get_subject_name(pCert[CertIndex]), nid_pos);
	name_len = (entry == NULL) ? -1 : ASN1_STRING_to_UTF8(&name, X509_NAME_ENTRY_get_data(entry));
	if (name_len <= 0) {
		uprintf("PKI: Failed to get Subject Name");
		goto out;
	}
	safe_strcp(info->name, sizeof(info->name), (char*)name, (size_t)name_len + 1);

	size = SHA1_HASHSIZE;
	if (!X509_digest(pCert[CertIndex], EVP_sha1(), info->thumbprint, &size) || (size != SHA1_HASHSIZE)) {
		uprintf("PKI: Failed to compute the thumbprint: %s", OpensslErrorString());
		goto out;
	}
	ret = CertIndex + 1;

out:
	OPENSSL_free(name);
	sk_X509_free(signers);
	PKCS7_free(p7);
	return ret;
}

// Verify a SHA-256 RSA (PKCS#1 v1.5) signature, with the public key of modulus
// 'modulus' (big endian) and exponent 65537.
BOOL ValidateRsaSha256Signature(const uint8_t* modulus, size_t modulus_len, const BYTE* pbBuffer,
	DWORD dwBufferLen, const BYTE* pbSignature, DWORD dwSigLen)
{
	BOOL r = FALSE;
	BIGNUM *n = NULL, *e = NULL;
	OSSL_PARAM_BLD* bld = NULL;
	OSSL_PARAM* params = NULL;
	EVP_PKEY_CTX* pctx = NULL;
	EVP_PKEY* pkey = NULL;
	EVP_MD_CTX* mctx = NULL;

	n = BN_bin2bn(modulus, (int)modulus_len, NULL);
	e = BN_new();
	bld = OSSL_PARAM_BLD_new();
	if ((n == NULL) || (e == NULL) || (bld == NULL) || !BN_set_word(e, 65537) ||
		!OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) ||
		!OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e) ||
		((params = OSSL_PARAM_BLD_to_param(bld)) == NULL)) {
		uprintf("PKI: Could not create public key parameters: %s", OpensslErrorString());
		goto out;
	}

	// Import our RSA public key
	pctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	if ((pctx == NULL) || (EVP_PKEY_fromdata_init(pctx) <= 0) ||
		(EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)) {
		uprintf("PKI: Could not import public key: %s", OpensslErrorString());
		goto out;
	}

	// Hash the buffer and verify the signature against it
	mctx = EVP_MD_CTX_new();
	if ((mctx == NULL) || (EVP_DigestVerifyInit(mctx, NULL, EVP_sha256(), NULL, pkey) <= 0)) {
		uprintf("PKI: Could not create hash: %s", OpensslErrorString());
		goto out;
	}
	r = (EVP_DigestVerify(mctx, pbSignature, dwSigLen, pbBuffer, dwBufferLen) == 1);
	if (!r)
		uprintf("Signature validation failed: %s", OpensslErrorString());

out:
	EVP_MD_CTX_free(mctx);
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(pctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(bld);
	BN_free(e);
	BN_free(n);
	return r;
}

// Validate the signature of a file downloaded from our server, which was signed with:
//   openssl dgst -sha256 -sign private.pem -out file.sig file
// NB: The signature is big endian, which Windows needs reversed and OpenSSL does not.
BOOL ValidateOpensslSignature(BYTE* pbBuffer, DWORD dwBufferLen, BYTE* pbSignature, DWORD dwSigLen)
{
	BOOL r = ValidateRsaSha256Signature(rsa_pubkey_modulus, sizeof(rsa_pubkey_modulus),
		pbBuffer, dwBufferLen, pbSignature, dwSigLen);

	// If the signature is invalid, clear the buffer so that
	// we don't keep potentially nasty stuff in memory.
	if (!r)
		memset(pbBuffer, 0, dwBufferLen);
	return r;
}
