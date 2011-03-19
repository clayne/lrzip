/*
   Copyright (C) 2006-2011 Con Kolivas
   Copyright (C) 2008, 2011 Peter Hyman
   Copyright (C) 1998 Andrew Tridgell

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
  Utilities used in rzip

  tridge, June 1996
  */

/*
 * Realloc removed
 * Functions added
 *    read_config()
 * Peter Hyman, December 2008
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <termios.h>

#ifdef _SC_PAGE_SIZE
# define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#else
# define PAGE_SIZE (4096)
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "lrzip_private.h"
#include "liblrzip.h"
#include "sha4.h"
#include "aes.h"

#define LRZ_DECRYPT	(0)
#define LRZ_ENCRYPT	(1)

static const char *infile = NULL;
static char delete_infile = 0;
static const char *outfile = NULL;
static char delete_outfile = 0;
static FILE *outputfile = NULL;

void register_infile(const char *name, char delete)
{
	infile = name;
	delete_infile = delete;
}

void register_outfile(const char *name, char delete)
{
	outfile = name;
	delete_outfile = delete;
}

void register_outputfile(FILE *f)
{
	outputfile = f;
}

void unlink_files(void)
{
	/* Delete temporary files generated for testing or faking stdio */
	if (outfile && delete_outfile)
		unlink(outfile);

	if (infile && delete_infile)
		unlink(infile);
}

static void fatal_exit(void)
{
	struct termios termios_p;

	/* Make sure we haven't died after disabling stdin echo */
	tcgetattr(fileno(stdin), &termios_p);
	termios_p.c_lflag |= ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);

	unlink_files();
	fprintf(outputfile, "Fatal error - exiting\n");
	fflush(outputfile);
	exit(1);
}

/* Failure when there is likely to be a meaningful error in perror */
void fatal(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}

	perror(NULL);
	fatal_exit();
}

void failure(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}

	fatal_exit();
}

void round_to_page(i64 *size)
{
	*size -= *size % PAGE_SIZE;
	if (unlikely(!*size))
		*size = PAGE_SIZE;
}

void get_rand(uchar *buf, int len)
{
	int fd, i;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1) {
		for (i = 0; i < len; i++)
			buf[i] = (uchar)random();
	} else {
		if (unlikely(read(fd, buf, len) != len))
			fatal("Failed to read fd in get_rand\n");
		if (unlikely(close(fd)))
			fatal("Failed to close fd in get_rand\n");
	}
}

static void xor128 (void *pa, const void *pb)
{
	i64 *a = pa;
	const i64 *b = pb;

	a [0] ^= b [0];
	a [1] ^= b [1];
}

static void lrz_crypt(rzip_control *control, uchar *buf, i64 len, uchar *salt, int encrypt)
{
	/* Encryption requires CBC_LEN blocks so we can use ciphertext
	* stealing to not have to pad the block */
	uchar key[HASH_LEN + BLOCKSALT_LEN], iv[HASH_LEN + BLOCKSALT_LEN];
	uchar tmp0[CBC_LEN], tmp1[CBC_LEN];
	aes_context aes_ctx;
	i64 N, M;
	int i;

	/* Generate unique key and IV for each block of data based on salt */
	mlock(&aes_ctx, sizeof(aes_ctx));
	mlock(key, HASH_LEN + BLOCKSALT_LEN);
	mlock(iv, HASH_LEN + BLOCKSALT_LEN);
	for (i = 0; i < HASH_LEN; i++)
		key[i] = control->pass_hash[i] ^ control->hash[i];
	memcpy(key + HASH_LEN, salt, BLOCKSALT_LEN);
	sha4(key, HASH_LEN + BLOCKSALT_LEN, key, 0);
	for (i = 0; i < HASH_LEN; i++)
		iv[i] = key[i] ^ control->pass_hash[i];
	memcpy(iv + HASH_LEN, salt, BLOCKSALT_LEN);
	sha4(iv, HASH_LEN + BLOCKSALT_LEN, iv, 0);

	M = len % CBC_LEN;
	N = len - M;

	if (encrypt == LRZ_ENCRYPT) {
		print_maxverbose("Encrypting data        \n");
		if (unlikely(aes_setkey_enc(&aes_ctx, key, 128)))
			failure("Failed to aes_setkey_enc in lrz_crypt\n");
		aes_crypt_cbc(&aes_ctx, AES_ENCRYPT, N, iv, buf, buf);
		
		if (M) {
			memset(tmp0, 0, CBC_LEN);
			memcpy(tmp0, buf + N, M);
			aes_crypt_cbc(&aes_ctx, AES_ENCRYPT, CBC_LEN,
				iv, tmp0, tmp1);
			memcpy(buf + N, buf + N - CBC_LEN, M);
			memcpy(buf + N - CBC_LEN, tmp1, CBC_LEN);
		}
	} else {
		if (unlikely(aes_setkey_dec(&aes_ctx, key, 128)))
			failure("Failed to aes_setkey_dec in lrz_crypt\n");
		print_maxverbose("Decrypting data        \n");
		if (M) {
			aes_crypt_cbc(&aes_ctx, AES_DECRYPT, N - CBC_LEN,
				      iv, buf, buf);
			aes_crypt_ecb(&aes_ctx, AES_DECRYPT,
				      buf + N - CBC_LEN, tmp0);
			memset(tmp1, 0, CBC_LEN);
			memcpy(tmp1, buf + N, M);
			xor128(tmp0, tmp1);
			memcpy(buf + N, tmp0, M);
			memcpy(tmp1 + M, tmp0 + M, CBC_LEN - M);
			aes_crypt_ecb(&aes_ctx, AES_DECRYPT, tmp1,
				      buf + N - CBC_LEN);
			xor128(buf + N - CBC_LEN, iv);
		} else
			aes_crypt_cbc(&aes_ctx, AES_DECRYPT, len,
				      iv, buf, buf);
	}

	memset(&aes_ctx, 0, sizeof(aes_ctx));
	memset(iv, 0, HASH_LEN + BLOCKSALT_LEN);
	memset(key, 0, HASH_LEN + BLOCKSALT_LEN);
	munlock(&aes_ctx, sizeof(aes_ctx));
	munlock(iv, HASH_LEN + BLOCKSALT_LEN);
	munlock(key, HASH_LEN + BLOCKSALT_LEN);
}

inline void lrz_encrypt(rzip_control *control, uchar *buf, i64 len, uchar *salt)
{
	lrz_crypt(control, buf, len, salt, LRZ_ENCRYPT);
}

inline void lrz_decrypt(rzip_control *control, uchar *buf, i64 len, uchar *salt)
{
	lrz_crypt(control, buf, len, salt, LRZ_DECRYPT);
}

void lrz_keygen(rzip_control *control, const uchar *passphrase)
{
	int i, j;

	sha4(passphrase, PASS_LEN, control->pass_hash, 0);

	print_maxverbose("Hashing passphrase %lld times\n", control->encloops);
	for (i = 0; i < control->encloops; i++) {
		for (j = 0; j < HASH_LEN; j++)
			control->hash[j] ^= control->pass_hash[j];
		sha4(control->hash, HASH_LEN, control->hash, 0);
	}
}
