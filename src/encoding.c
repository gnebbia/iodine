/*
 * Copyright (c) 2006-2014 Erik Ekman <yarrick@kryo.se>,
 * 2006-2009 Bjorn Andersson <flex@kryo.se>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "common.h"
#include "dns.h"
#include "hmac_md5.h"
#include "encoding.h"
#include "read.h"
#include "base32.h"
#include "base64.h"
#include "base64u.h"
#include "base128.h"

uint8_t
get_codec_from_name(char *encoding)
{
	if (!strcasecmp(encoding, "base32")) return C_BASE32;
	else if (!strcasecmp(encoding, "base64")) return C_BASE64;
	else if (!strcasecmp(encoding, "base64u")) return C_BASE64U;
	else if (!strcasecmp(encoding, "base128")) return C_BASE128;
	else if (!strcasecmp(encoding, "raw")) return C_RAW;
	else return C_UNSET;
}

size_t
get_raw_length_from_dns(size_t dns_hostlen, struct encoder *enc, const uint8_t *topdomain)
/* Returns the maximum length of raw data that can be encoded into enc_bytes */
{
	/* 1 byte for dot before topdomain */
	size_t enc_datalen = dns_hostlen - 1 - HOSTLEN(topdomain);

	/* Number of dots in length of encoded data -
	   Dots are not included in encoded data length */
	enc_datalen -= DNS_NUM_LABELS(enc_datalen) + 1;

	if (enc)
		return enc->get_raw_length(enc_datalen);
	else
		return enc_datalen;
}

size_t
get_encoded_dns_length(size_t raw_bytes, struct encoder *enc, const uint8_t *topdomain)
/* Returns length of encoded data from original data length orig_len; */
{
	size_t dots = 1; /* dot before topdomain */
	size_t len;
	if (enc)
		len = enc->get_encoded_length(raw_bytes);
	else
		len = raw_bytes;

	dots += len / DNS_MAXLABEL; /* number of dots needed in data */
	return len + dots + HOSTLEN(topdomain);
}

size_t
build_hostname(uint8_t *buf, size_t buflen, const uint8_t *data, const size_t datalen,
		const char *topdomain, struct encoder *encoder, size_t maxlen, size_t header_len)
/* Builds DNS-compatible hostname for data using specified encoder and topdomain
 * Encoded data is placed into buf. */
{
	size_t space, enc;
	uint8_t *b;

	buflen -= header_len;
	buf += header_len;
	maxlen -= header_len;
	memset(buf, 0, buflen);

	maxlen = MIN(maxlen, buflen);

	/* 1 byte for dot before topdomain + 1 byte extra for something */
	space = maxlen - strlen(topdomain) - (maxlen / DNS_MAXLABEL) - 2;

	enc = encoder->encode(buf, &space, data, datalen);
//	warnx("build_hostname: enc %lu, predicted %lu; maxlen %lu, header %lu, datalen %lu, space %lu",
//		  encdata_len, encoder->get_encoded_length(datalen), maxlen, header_len, datalen, space);

	enc = inline_dotify(buf - header_len, buflen + header_len) - header_len;

	b = buf + enc;

	/* move b back one step to see if the dot is there */
	b--;
	if (*b != '.')
		*++b = '.';
	b++;
	/* move b ahead of the string so we can copy to it */

	strncpy((char *)b, topdomain, strlen(topdomain)+1);
//	warnx("build_hostname: host '%s' (sl %lu, actual %lu), topdomain '%s'",
//			buf - header_len, strlen(buf - header_len), encdata_len + header_len + strlen(topdomain)+1, b);

	return space;
}

size_t
inline_dotify(uint8_t *buf, size_t buflen)
{
	unsigned dots;
	size_t pos, total;
	uint8_t *reader, *writer;

	total = strlen((char *)buf);
	dots = total / DNS_MAXLABEL;

	writer = buf;
	writer += total;
	writer += dots;

	total += dots;
	if (strlen((char *)buf) + dots > buflen) {
		writer = buf;
		writer += buflen;
		total = buflen;
	}

	reader = writer - dots;
	pos = (reader - buf) + 1;

	while (dots) {
		*writer-- = *reader--;
		pos--;
		if (pos % DNS_MAXLABEL == 0) {
			*writer-- = '.';
			dots--;
		}
	}

	/* return new length of string */
	return total;
}

size_t
inline_undotify(uint8_t *buf, size_t len)
{
	size_t pos;
	unsigned dots;
	uint8_t *reader, *writer;

	writer = buf;
	reader = writer;

	pos = 0;
	dots = 0;

	while (pos < len) {
		if (*reader == '.') {
			reader++;
			pos++;
			dots++;
			continue;
		}
		*writer++ = *reader++;
		pos++;
	}

	/* return new length of string */
	return len - dots;
}

struct encoder *
get_encoder(uint8_t codec)
{
	switch (codec & 0x7) {
	case C_BASE32:
		return b32;
	case C_BASE64:
		return b64;
	case C_BASE64U:
		return b64u;
	case C_BASE128:
		return b128;
	case C_RAW:
	default:
		return NULL;
	}
}

size_t
encode_data(uint8_t *buf, size_t buflen, uint8_t *data, size_t datalen, uint8_t codec)
/* Returns #bytes of data that were encoded */
{
	struct encoder *enc;

	enc = get_encoder(codec);
	if (enc == NULL) {
		memcpy(buf, data, MIN(buflen, datalen));
		return MIN(buflen, datalen);
	}

	buflen--; /* encoders add trailing zero byte which is not counted in buflen */
	return enc->encode(buf, &buflen, data, datalen);
}

size_t
unpack_data(uint8_t *buf, size_t buflen, uint8_t *data, size_t datalen, uint8_t codec)
{
	struct encoder *enc;

	enc = get_encoder(codec);
	if (enc == NULL) {
		memcpy(buf, data, MIN(buflen, datalen));
		return MIN(buflen, datalen);
	}

	return enc->decode(buf, &buflen, data, datalen);
}

int
downstream_encode(uint8_t *out, size_t *outlen, uint8_t *data, size_t datalen,
				uint8_t *hmac_key, uint8_t flags, uint32_t cmc)
/* Adds downstream header (flags+CMC+HMAC) to given data and encode
 * returns 1 on success, 0 on failure */
{
	size_t hmaclen;
	uint32_t len;

	if (flags & DH_ERROR) {
		if (flags & DH_HMAC32) {
			/* always 96-bit HMAC when error flag is set */
			flags ^= DH_HMAC32;
		}
	}
	hmaclen = flags & DH_HMAC32 ? 4 : 12;
	if (*outlen < 5 + hmaclen + datalen) {
		return 0;
	}

	/* construct downstream data header
	 * 4 bytes CMC (network byte order) (random for pre-login responses)
	 * 4 or 12 bytes HMAC (note: HMAC field is 32-bits random for all pre-login responses)
	 * for HMAC calculation (in hmacbuf): length + flags + CMC + hmac + data */
	len = 4 + 1 + 4 + hmaclen + datalen;
	uint8_t hmac[16], hmacbuf[len];

	*(uint32_t *) hmacbuf = htonl(len);
	out[0] = hmacbuf[4] = b32_5to8(flags);
	*(uint32_t *) (hmacbuf + 5) = htonl(cmc);
	memcpy(hmacbuf + 9 + hmaclen, data, datalen);

	memset(hmacbuf + 9, 0, hmaclen);
	if (hmac_key) {
		hmac_md5(hmac, hmac_key, 16, hmacbuf, len);
	} else {
		get_rand_bytes(hmac, sizeof(hmac));
	}
	memcpy(hmacbuf + 9, hmac, hmaclen);

	/* now encode data from hmacbuf (not including flags and length, +0 terminator) */
	*outlen = encode_data(out + 1, *outlen - 2, hmacbuf + 5, len - 5, flags & 7) + 1;
	return 1;
}

int downstream_decode_err;

void
print_downstream_err()
{
	char *s = "unknown";
	if (downstream_decode_err & DDERR_BADHMAC) {
		s = "server reply has bad HMAC!";
	} else if (downstream_decode_err & DDERR_TOOSHORT) {
		s = "server reply was too short!";
	} else if (downstream_decode_err & DDERR_IS_ANS) {
		switch (downstream_decode_err & 7) {
		case E_BADAUTH:
			s = "bad authentication (session likely expired due to inactivity)";
			break;
		case E_BADLEN:
			s = "bad length (query likely truncated)";
			break;
		case E_BADLOGIN:
			s = "bad login (is password correct?)";
			break;
		case E_BADOPTS:
			s = "bad server options (try setting manually)";
			break;
		}
	}
	fprintf(stderr, "%s (%d (0x%02x))\n", s,
			downstream_decode_err & 7, downstream_decode_err);
}

int
downstream_decode(uint8_t *out, size_t *outlen, uint8_t *encdata, size_t encdatalen, uint8_t *hmac_key)
/* validate downstream header + HMAC, decode data
 * note: exact reverse of downstream_encode
 * returns 1 on success, 0 with error and sets downstream_decode_err to error code
 * if decode error occurs or bad HMAC, encdata is simply copied to out */
{
	uint8_t hmac[16], hmac_pkt[16], hmacbuf[encdatalen + 4], *p, flags, error;
	size_t hmaclen;
	uint32_t len;

	if (encdatalen < 2) {
		goto _dderr;
	}

	flags = b32_8to5(encdata[0]);

	hmaclen = flags & DH_HMAC32 ? 4 : 12;

	if (flags & DH_ERROR) {
		DEBUG(1, "got DH_ERROR from server! code=%x (len=%" L "u)", flags & 7, encdatalen);
		/* always 96-bit HMAC when error flag is set */
		error = flags & 7;
		if (hmaclen == 4) {
			DEBUG(2, "server says 32-bit HMAC with error flag set!");
			downstream_decode_err = DDERR_BADHMAC;
			goto _dderr;
		}
		flags = C_BASE32; /* HMAC and CMC are still present with error */
	}

	/* deconstruct downstream data header
	 * 4 bytes CMC (network byte order) (random for pre-login responses)
	 * 4 or 12 bytes HMAC (note: HMAC field is 32-bits random for all pre-login responses)
	 * for HMAC calculation (in hmacbuf): length + flags + CMC + hmac + data */

	/* decode data first */
	len = unpack_data(hmacbuf + 5, encdatalen - 1,
			encdata + 1, encdatalen - 1, flags & 7);
	if (len < 4 + hmaclen) {
		/* packet length must at least match flags */
		downstream_decode_err = DDERR_TOOSHORT;
		goto _dderr;
	}

	if (hmac_key) {
		p = hmacbuf;
		putlong(&p, len); /* 4 bytes length */
		hmacbuf[4] = encdata[0]; /* encoded flags byte */
		memcpy(hmac_pkt, hmacbuf + 9, hmaclen); /* copy packet HMAC */
		memset(hmacbuf + 9, 0, hmaclen); /* clear HMAC field */
		hmac_md5(hmac, hmac_key, 16, hmacbuf, len + 4); /* calculate HMAC */
		if (memcmp(hmac, hmac_pkt, hmaclen) != 0) { /* verify */
			DEBUG(3, "RX: bad HMAC pkt=%s, actual=%s",
					tohexstr(hmac_pkt, hmaclen, 0), tohexstr(hmac, hmaclen, 1));
			downstream_decode_err = DDERR_BADHMAC;
			goto _dderr;
		}
	}
	if (*outlen < len - 4 - hmaclen) {
		goto _dderr;
	}

	if (!(flags & DH_ERROR)) {
		memcpy(out, hmacbuf + 9 + hmaclen, len - 4 - hmaclen);
		*outlen = len - 4 - hmaclen;
		return 1;
	} else {
		downstream_decode_err = error | DDERR_IS_ANS;
	}
_dderr:
	*outlen = MIN(*outlen, encdatalen);
	return memcpy(out, encdata, *outlen), 0;
}


