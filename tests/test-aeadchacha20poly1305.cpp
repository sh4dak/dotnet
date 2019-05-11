#include <cassert>
#include <inttypes.h>
#include <string.h>

#include "Crypto.h"

char text[] = "Ladies and Gentlemen of the class of '99: If I could offer you "
"only one tip for the future, sunscreen would be it."; // 114 bytes

uint8_t key[32] = 
{
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
};

uint8_t ad[12] = 
{
    0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7
};

uint8_t nonce[12] = 
{
	0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
};

uint8_t tag[16] = 
{
	0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
};

uint8_t encrypted[114] = 
{
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
    0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe, 0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6,
    0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
    0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36,
    0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c, 0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58,
    0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
    0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b,
    0x61, 0x16
};

int main ()
{
	uint8_t buf[114+16];
	// test encryption
	dotnet::crypto::AEADChaCha20Poly1305 ((uint8_t *)text, 114, ad, 12, key, nonce, buf, 114 + 16, true);
	assert (memcmp (buf, encrypted, 114) == 0);
	assert (memcmp (buf + 114, tag, 16) == 0);
	// test decryption
	uint8_t buf1[114];
	assert (dotnet::crypto::AEADChaCha20Poly1305 (buf, 114, ad, 12, key, nonce, buf1, 114, false));
	assert (memcmp (buf1, text, 114) == 0);
	// test encryption of multiple buffers
	memcpy (buf, text, 114);
	std::vector<std::pair<uint8_t*, std::size_t> > bufs{ std::make_pair (buf, 20), std::make_pair (buf + 20, 10), std::make_pair (buf + 30, 70), std::make_pair (buf + 100, 14) };  
	dotnet::crypto::AEADChaCha20Poly1305Encrypt (bufs, key, nonce, buf + 114);
	dotnet::crypto::AEADChaCha20Poly1305 (buf, 114, nullptr, 0, key, nonce, buf1, 114, false);
	assert (memcmp (buf1, text, 114) == 0);
}
