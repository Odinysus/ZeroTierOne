/*
 * Copyright (c)2013-2020 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2024-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include "Constants.hpp"
#include "Identity.hpp"
#include "SHA512.hpp"
#include "Salsa20.hpp"
#include "Poly1305.hpp"
#include "Utils.hpp"
#include "Endpoint.hpp"
#include "Locator.hpp"

#include <algorithm>

namespace ZeroTier {

namespace {

// This is the memory-intensive hash function used to compute v0 identities from v0 public keys.
#define ZT_V0_IDENTITY_GEN_MEMORY 2097152

void identityV0ProofOfWorkFrankenhash(const void *const publicKey, unsigned int publicKeyBytes, void *const digest, void *const genmem) noexcept
{
	// Digest publicKey[] to obtain initial digest
	SHA512(digest, publicKey, publicKeyBytes);

	// Initialize genmem[] using Salsa20 in a CBC-like configuration since
	// ordinary Salsa20 is randomly seek-able. This is good for a cipher
	// but is not what we want for sequential memory-hardness.
	Utils::zero<ZT_V0_IDENTITY_GEN_MEMORY>(genmem);
	Salsa20 s20(digest, (char *) digest + 32);
	s20.crypt20((char *) genmem, (char *) genmem, 64);
	for (unsigned long i = 64;i < ZT_V0_IDENTITY_GEN_MEMORY;i += 64) {
		unsigned long k = i - 64;
		*((uint64_t * )((char *) genmem + i)) = *((uint64_t * )((char *) genmem + k));
		*((uint64_t * )((char *) genmem + i + 8)) = *((uint64_t * )((char *) genmem + k + 8));
		*((uint64_t * )((char *) genmem + i + 16)) = *((uint64_t * )((char *) genmem + k + 16));
		*((uint64_t * )((char *) genmem + i + 24)) = *((uint64_t * )((char *) genmem + k + 24));
		*((uint64_t * )((char *) genmem + i + 32)) = *((uint64_t * )((char *) genmem + k + 32));
		*((uint64_t * )((char *) genmem + i + 40)) = *((uint64_t * )((char *) genmem + k + 40));
		*((uint64_t * )((char *) genmem + i + 48)) = *((uint64_t * )((char *) genmem + k + 48));
		*((uint64_t * )((char *) genmem + i + 56)) = *((uint64_t * )((char *) genmem + k + 56));
		s20.crypt20((char *) genmem + i, (char *) genmem + i, 64);
	}

	// Render final digest using genmem as a lookup table
	for (unsigned long i = 0;i < (ZT_V0_IDENTITY_GEN_MEMORY / sizeof(uint64_t));) {
		unsigned long idx1 = (unsigned long) (Utils::ntoh(((uint64_t *) genmem)[i++]) % (64 / sizeof(uint64_t))); // NOLINT(hicpp-use-auto,modernize-use-auto)
		unsigned long idx2 = (unsigned long) (Utils::ntoh(((uint64_t *) genmem)[i++]) % (ZT_V0_IDENTITY_GEN_MEMORY / sizeof(uint64_t))); // NOLINT(hicpp-use-auto,modernize-use-auto)
		uint64_t tmp = ((uint64_t *) genmem)[idx2];
		((uint64_t *) genmem)[idx2] = ((uint64_t *) digest)[idx1];
		((uint64_t *) digest)[idx1] = tmp;
		s20.crypt20(digest, digest, 64);
	}
}

struct identityV0ProofOfWorkCriteria
{
	ZT_INLINE identityV0ProofOfWorkCriteria(unsigned char *sb, char *gm) noexcept: digest(sb), genmem(gm)
	{}

	ZT_INLINE bool operator()(const uint8_t pub[ZT_C25519_COMBINED_PUBLIC_KEY_SIZE]) const noexcept
	{
		identityV0ProofOfWorkFrankenhash(pub, ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, digest, genmem);
		return (digest[0] < 17);
	}

	unsigned char *digest;
	char *genmem;
};

#define ZT_IDENTITY_V1_POW_MEMORY_SIZE 131072

struct p_CompareLittleEndian
{
#if __BYTE_ORDER == __BIG_ENDIAN
	ZT_INLINE bool operator()(const uint64_t a,const uint64_t b) const noexcept { return Utils::swapBytes(a) < Utils::swapBytes(b); }
#else
	ZT_INLINE bool operator()(const uint64_t a,const uint64_t b) const noexcept { return a < b; }
#endif
};

// This is a simpler memory-intensive frankenhash for V1 identity generation.
bool identityV1ProofOfWorkCriteria(const void *in, const unsigned int len)
{
	uint64_t w[ZT_IDENTITY_V1_POW_MEMORY_SIZE / 8];

	// Fill work buffer with pseudorandom bytes using a construction that should be
	// relatively hostile to GPU acceleration. GPUs usually implement branching by
	// executing all branches and then selecting the answer, which means this
	// construction should require a GPU to do ~3X the work of a CPU per iteration.
	SHA512(w, in, len);
	for (unsigned int i = 8, j = 0;i < (ZT_IDENTITY_V1_POW_MEMORY_SIZE / 8);) {
		uint64_t *const ww = w + i;
		const uint64_t *const wp = w + j;
		i += 8;
		j += 8;
		if ((wp[0] & 7U) == 0) {
			SHA512(ww, wp, 64);
		} else if ((wp[1] & 15U) == 0) {
			ww[0] = Utils::hton(Utils::ntoh(wp[0]) % 4503599627370101ULL);
			ww[1] = Utils::hton(Utils::ntoh(wp[1]) % 4503599627370161ULL);
			ww[2] = Utils::hton(Utils::ntoh(wp[2]) % 4503599627370227ULL);
			ww[3] = Utils::hton(Utils::ntoh(wp[3]) % 4503599627370287ULL);
			ww[4] = Utils::hton(Utils::ntoh(wp[4]) % 4503599627370299ULL);
			ww[5] = Utils::hton(Utils::ntoh(wp[5]) % 4503599627370323ULL);
			ww[6] = Utils::hton(Utils::ntoh(wp[6]) % 4503599627370353ULL);
			ww[7] = Utils::hton(Utils::ntoh(wp[7]) % 4503599627370449ULL);
			SHA384(ww, wp, 128);
		} else {
			Salsa20(wp, wp + 4).crypt12(wp, ww, 64);
		}
	}

	// Sort 64-bit integers (little-endian) into ascending order and compute a
	// cryptographic checksum. Sorting makes the order of values dependent on all
	// other values, making a speed competitive implementation that skips on the
	// memory requirement extremely hard.
	std::sort(w, w + (ZT_IDENTITY_V1_POW_MEMORY_SIZE / 8), p_CompareLittleEndian());
	Poly1305::compute(w, w, ZT_IDENTITY_V1_POW_MEMORY_SIZE, w);

	// PoW criteria passed if this is true. The value 1093 was chosen experimentally
	// to yield a good average performance balancing fast setup with intentional
	// identity collision resistance.
	return (Utils::ntoh(w[0]) % 1000U) == 0;
}

} // anonymous namespace

const Identity Identity::NIL;

bool Identity::generate(const Type t)
{
	m_type = t;
	m_hasPrivate = true;

	switch (t) {

		case C25519: {
			// Generate C25519/Ed25519 key pair whose hash satisfies a "hashcash" criterion and generate the
			// address from the last 40 bits of this hash. This is different from the fingerprint hash for V0.
			uint8_t digest[64];
			char *const genmem = new char[ZT_V0_IDENTITY_GEN_MEMORY];
			Address address;
			do {
				C25519::generateSatisfying(identityV0ProofOfWorkCriteria(digest, genmem), m_pub, m_priv);
				address.setTo(digest + 59);
			} while (address.isReserved());
			delete[] genmem;
			m_fp.address = address; // address comes from PoW hash for type 0 identities
			m_computeHash();
		} break;

		case P384: {
			for (;;) {
				// Loop until we pass the PoW criteria. The nonce is only 8 bits, so generate
				// some new key material every time it wraps. The ECC384 generator is slightly
				// faster so use that one.
				m_pub[0] = 0; // zero nonce
				C25519::generateCombined(m_pub + 1, m_priv + 1);
				ECC384GenerateKey(m_pub + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, m_priv + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE);
				for (;;) {
					if (identityV1ProofOfWorkCriteria(m_pub, sizeof(m_pub)))
						break;
					if (++m_pub[0] == 0)
						ECC384GenerateKey(m_pub + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, m_priv + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE);
				}

				// If we passed PoW then check that the address is valid, otherwise loop
				// back around and run the whole process again.
				m_computeHash();
				const Address addr(m_fp.hash);
				if (!addr.isReserved()) {
					m_fp.address = addr;
					break;
				}
			}
		} break;

		default:
			return false;
	}

	return true;
}

bool Identity::locallyValidate() const noexcept
{
	try {
		if ((m_fp) && ((!Address(m_fp.address).isReserved()))) {
			switch (m_type) {
				case C25519: {
					uint8_t digest[64];
					char *const genmem = (char *) malloc(ZT_V0_IDENTITY_GEN_MEMORY);
					if (!genmem)
						return false;
					identityV0ProofOfWorkFrankenhash(m_pub, ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, digest, genmem);
					free(genmem);
					return ((Address(digest + 59) == m_fp.address) && (digest[0] < 17));
				}
				case P384: {
					if (Address(m_fp.hash) != m_fp.address)
						return false;
					return identityV1ProofOfWorkCriteria(m_pub, sizeof(m_pub));
				}
			}
		}
	} catch (...) {}
	return false;
}

void Identity::hashWithPrivate(uint8_t h[ZT_FINGERPRINT_HASH_SIZE]) const
{
	if (m_hasPrivate) {
		switch (m_type) {
			case C25519:
				SHA384(h, m_pub, ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, m_priv, ZT_C25519_COMBINED_PRIVATE_KEY_SIZE);
				break;
			case P384:
				SHA384(h, m_pub, sizeof(m_pub), m_priv, sizeof(m_priv));
				break;
		}
		return;
	}
	Utils::zero<48>(h);
}

unsigned int Identity::sign(const void *data, unsigned int len, void *sig, unsigned int siglen) const
{
	if (m_hasPrivate) {
		switch (m_type) {
			case C25519:
				if (siglen >= ZT_C25519_SIGNATURE_LEN) {
					C25519::sign(m_priv, m_pub, data, len, sig);
					return ZT_C25519_SIGNATURE_LEN;
				}
			case P384:
				if (siglen >= ZT_ECC384_SIGNATURE_SIZE) {
					// SECURITY: signatures also include the public keys to further enforce their coupling.
					uint8_t h[48];
					SHA384(h, data, len, m_pub, sizeof(m_pub));
					ECC384ECDSASign(m_priv + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, h, (uint8_t *) sig);
					return ZT_ECC384_SIGNATURE_SIZE;
				}
		}
	}
	return 0;
}

bool Identity::verify(const void *data, unsigned int len, const void *sig, unsigned int siglen) const
{
	switch (m_type) {
		case C25519:
			return C25519::verify(m_pub, data, len, sig, siglen);
		case P384:
			if (siglen == ZT_ECC384_SIGNATURE_SIZE) {
				uint8_t h[48];
				SHA384(h, data, len, m_pub, sizeof(m_pub));
				return ECC384ECDSAVerify(m_pub + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, h, (const uint8_t *) sig);
			}
			break;
	}
	return false;
}

bool Identity::agree(const Identity &id, uint8_t key[ZT_SYMMETRIC_KEY_SIZE]) const
{
	uint8_t rawkey[128];
	uint8_t h[64];
	if (m_hasPrivate) {
		if (m_type == C25519) {
			if ((id.m_type == C25519) || (id.m_type == P384)) {
				// If we are a C25519 key we can agree with another C25519 key or with only the
				// C25519 portion of a type 1 P-384 key.
				C25519::agree(m_priv, id.m_pub, rawkey);
				SHA512(h, rawkey, ZT_C25519_ECDH_SHARED_SECRET_SIZE);
				Utils::copy<ZT_SYMMETRIC_KEY_SIZE>(key, h);
				return true;
			}
		} else if (m_type == P384) {
			if (id.m_type == P384) {
				// For another P384 identity we execute DH agreement with BOTH keys and then
				// hash the results together. For those (cough FIPS cough) who only consider
				// P384 to be kosher, the C25519 secret can be considered a "salt"
				// or something. For those who don't trust P384 this means the privacy of
				// your traffic is also protected by C25519.
				C25519::agree(m_priv, id.m_pub, rawkey);
				ECC384ECDH(id.m_pub + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, m_priv + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE, rawkey + ZT_C25519_ECDH_SHARED_SECRET_SIZE);
				SHA384(h, rawkey, ZT_C25519_ECDH_SHARED_SECRET_SIZE + ZT_ECC384_SHARED_SECRET_SIZE);
				Utils::copy<ZT_SYMMETRIC_KEY_SIZE>(key, h);
				return true;
			} else if (id.m_type == C25519) {
				// If the other identity is a C25519 identity we can agree using only that type.
				C25519::agree(m_priv, id.m_pub, rawkey);
				SHA512(h, rawkey, ZT_C25519_ECDH_SHARED_SECRET_SIZE);
				Utils::copy<ZT_SYMMETRIC_KEY_SIZE>(key, h);
				return true;
			}
		}
	}
	return false;
}

char *Identity::toString(bool includePrivate, char buf[ZT_IDENTITY_STRING_BUFFER_LENGTH]) const
{
	char *p = buf;
	Address(m_fp.address).toString(p);
	p += 10;
	*(p++) = ':';

	switch (m_type) {
		case C25519: {
			*(p++) = '0';
			*(p++) = ':';
			Utils::hex(m_pub, ZT_C25519_COMBINED_PUBLIC_KEY_SIZE, p);
			p += ZT_C25519_COMBINED_PUBLIC_KEY_SIZE * 2;
			if ((m_hasPrivate) && (includePrivate)) {
				*(p++) = ':';
				Utils::hex(m_priv, ZT_C25519_COMBINED_PRIVATE_KEY_SIZE, p);
				p += ZT_C25519_COMBINED_PRIVATE_KEY_SIZE * 2;
			}
			*p = (char) 0;
			return buf;
		}
		case P384: {
			*(p++) = '1';
			*(p++) = ':';
			int el = Utils::b32e(m_pub, sizeof(m_pub), p, (int) (ZT_IDENTITY_STRING_BUFFER_LENGTH - (uintptr_t) (p - buf)));
			if (el <= 0) return nullptr;
			p += el;
			if ((m_hasPrivate) && (includePrivate)) {
				*(p++) = ':';
				el = Utils::b32e(m_priv, sizeof(m_priv), p, (int) (ZT_IDENTITY_STRING_BUFFER_LENGTH - (uintptr_t) (p - buf)));
				if (el <= 0) return nullptr;
				p += el;
			}
			*p = (char) 0;
			return buf;
		}
	}

	return nullptr;
}

bool Identity::fromString(const char *str)
{
	char tmp[ZT_IDENTITY_STRING_BUFFER_LENGTH];
	memoryZero(this);
	if ((!str) || (!Utils::scopy(tmp, sizeof(tmp), str)))
		return false;

	int fno = 0;
	char *saveptr = nullptr;
	for (char *f = Utils::stok(tmp, ":", &saveptr);((f) && (fno < 4));f = Utils::stok(nullptr, ":", &saveptr)) {
		switch (fno++) {

			case 0:
				m_fp.address = Utils::hexStrToU64(f) & ZT_ADDRESS_MASK;
				if (Address(m_fp.address).isReserved())
					return false;
				break;

			case 1:
				if ((f[0] == '0') && (!f[1])) {
					m_type = C25519;
				} else if ((f[0] == '1') && (!f[1])) {
					m_type = P384;
				} else {
					return false;
				}
				break;

			case 2:
				switch (m_type) {

					case C25519:
						if (Utils::unhex(f, strlen(f), m_pub, ZT_C25519_COMBINED_PUBLIC_KEY_SIZE) != ZT_C25519_COMBINED_PUBLIC_KEY_SIZE)
							return false;
						break;

					case P384:
						if (Utils::b32d(f, m_pub, sizeof(m_pub)) != sizeof(m_pub))
							return false;
						break;

				}
				break;

			case 3:
				if (strlen(f) > 1) {
					switch (m_type) {

						case C25519:
							if (Utils::unhex(f, strlen(f), m_priv, ZT_C25519_COMBINED_PRIVATE_KEY_SIZE) != ZT_C25519_COMBINED_PRIVATE_KEY_SIZE) {
								return false;
							} else {
								m_hasPrivate = true;
							}
							break;

						case P384:
							if (Utils::b32d(f, m_priv, sizeof(m_priv)) != sizeof(m_priv)) {
								return false;
							} else {
								m_hasPrivate = true;
							}
							break;

					}
					break;
				}

		}
	}

	if (fno < 3)
		return false;

	m_computeHash();
	return !((m_type == P384) && (Address(m_fp.hash) != m_fp.address));
}

int Identity::marshal(uint8_t data[ZT_IDENTITY_MARSHAL_SIZE_MAX], const bool includePrivate) const noexcept
{
	Address(m_fp.address).copyTo(data);
	switch (m_type) {

		case C25519:
			data[ZT_ADDRESS_LENGTH] = (uint8_t) C25519;
			Utils::copy<ZT_C25519_COMBINED_PUBLIC_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1, m_pub);
			if ((includePrivate) && (m_hasPrivate)) {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE] = ZT_C25519_COMBINED_PRIVATE_KEY_SIZE;
				Utils::copy<ZT_C25519_COMBINED_PRIVATE_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1, m_priv);
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1 + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE;
			} else {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE] = 0;
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1;
			}

		case P384:
			data[ZT_ADDRESS_LENGTH] = (uint8_t) P384;
			Utils::copy<ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1, m_pub);
			if ((includePrivate) && (m_hasPrivate)) {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE] = ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE;
				Utils::copy<ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1, m_priv);
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1 + ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE;
			} else {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE] = 0;
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1;
			}

	}
	return -1;
}

int Identity::unmarshal(const uint8_t *data, const int len) noexcept
{
	memoryZero(this);

	if (len < (1 + ZT_ADDRESS_LENGTH))
		return -1;
	m_fp.address = Address(data);

	unsigned int privlen;
	switch ((m_type = (Type) data[ZT_ADDRESS_LENGTH])) {

		case C25519:
			if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1))
				return -1;

			Utils::copy<ZT_C25519_COMBINED_PUBLIC_KEY_SIZE>(m_pub, data + ZT_ADDRESS_LENGTH + 1);
			m_computeHash();

			privlen = data[ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE];
			if (privlen == ZT_C25519_COMBINED_PRIVATE_KEY_SIZE) {
				if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1 + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE))
					return -1;
				m_hasPrivate = true;
				Utils::copy<ZT_C25519_COMBINED_PRIVATE_KEY_SIZE>(m_priv, data + ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1);
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1 + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE;
			} else if (privlen == 0) {
				m_hasPrivate = false;
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1;
			}
			break;

		case P384:
			if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1))
				return -1;

			Utils::copy<ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE>(m_pub, data + ZT_ADDRESS_LENGTH + 1);
			m_computeHash(); // this sets the address for P384
			if (Address(m_fp.hash) != m_fp.address) // this sanity check is possible with V1 identities
				return -1;

			privlen = data[ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE];
			if (privlen == ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE) {
				if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1 + ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE))
					return -1;
				m_hasPrivate = true;
				Utils::copy<ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE>(&m_priv, data + ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1);
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1 + ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE;
			} else if (privlen == 0) {
				m_hasPrivate = false;
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1;
			}
			break;

	}

	return -1;
}

void Identity::m_computeHash()
{
	switch (m_type) {
		default:
			m_fp.zero();
			break;
		case C25519:
			SHA384(m_fp.hash, m_pub, ZT_C25519_COMBINED_PUBLIC_KEY_SIZE);
			break;
		case P384:
			SHA384(m_fp.hash, m_pub, ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE);
			break;
	}
}

} // namespace ZeroTier

extern "C" {

ZT_Identity *ZT_Identity_new(enum ZT_IdentityType type)
{
	if ((type != ZT_IDENTITY_TYPE_C25519) && (type != ZT_IDENTITY_TYPE_P384))
		return nullptr;
	try {
		ZeroTier::Identity *const id = new ZeroTier::Identity();
		id->generate((ZeroTier::Identity::Type) type);
		return reinterpret_cast<ZT_Identity *>(id);
	} catch (...) {
		return nullptr;
	}
}

ZT_Identity *ZT_Identity_fromString(const char *idStr)
{
	if (!idStr)
		return nullptr;
	try {
		ZeroTier::Identity *const id = new ZeroTier::Identity();
		if (!id->fromString(idStr)) {
			delete id;
			return nullptr;
		}
		return reinterpret_cast<ZT_Identity *>(id);
	} catch (...) {
		return nullptr;
	}
}

int ZT_Identity_validate(const ZT_Identity *id)
{
	if (!id)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->locallyValidate() ? 1 : 0;
}

unsigned int ZT_Identity_sign(const ZT_Identity *id, const void *data, unsigned int len, void *signature, unsigned int signatureBufferLength)
{
	if (!id)
		return 0;
	if (signatureBufferLength < ZT_SIGNATURE_BUFFER_SIZE)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->sign(data, len, signature, signatureBufferLength);
}

int ZT_Identity_verify(const ZT_Identity *id, const void *data, unsigned int len, const void *signature, unsigned int sigLen)
{
	if ((!id) || (!signature) || (!sigLen))
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->verify(data, len, signature, sigLen) ? 1 : 0;
}

enum ZT_IdentityType ZT_Identity_type(const ZT_Identity *id)
{
	if (!id)
		return (ZT_IdentityType) 0;
	return (enum ZT_IdentityType) reinterpret_cast<const ZeroTier::Identity *>(id)->type();
}

char *ZT_Identity_toString(const ZT_Identity *id, char *buf, int capacity, int includePrivate)
{
	if ((!id) || (!buf) || (capacity < ZT_IDENTITY_STRING_BUFFER_LENGTH))
		return nullptr;
	reinterpret_cast<const ZeroTier::Identity *>(id)->toString(includePrivate != 0, buf);
	return buf;
}

int ZT_Identity_hasPrivate(const ZT_Identity *id)
{
	if (!id)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->hasPrivate() ? 1 : 0;
}

uint64_t ZT_Identity_address(const ZT_Identity *id)
{
	if (!id)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->address();
}

const ZT_Fingerprint *ZT_Identity_fingerprint(const ZT_Identity *id)
{
	if (!id)
		return nullptr;
	return &(reinterpret_cast<const ZeroTier::Identity *>(id)->fingerprint());
}

ZT_SDK_API void ZT_Identity_delete(ZT_Identity *id)
{
	if (id)
		delete reinterpret_cast<ZeroTier::Identity *>(id);
}

}
