/* parse-packet.c  - read packets
 *	Copyright (C) 1998 Free Software Foundation, Inc.
 *
 * This file is part of GNUPG.
 *
 * GNUPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GNUPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "packet.h"
#include "iobuf.h"
#include "mpi.h"
#include "util.h"
#include "cipher.h"
#include "memory.h"
#include "filter.h"
#include "options.h"

static int mpi_print_mode = 0;
static int list_mode = 0;

static int  parse( IOBUF inp, PACKET *pkt, int reqtype,
		   ulong *retpos, int *skip, IOBUF out, int do_skip );
static int  copy_packet( IOBUF inp, IOBUF out, int pkttype,
					       unsigned long pktlen );
static void skip_packet( IOBUF inp, int pkttype, unsigned long pktlen );
static void skip_rest( IOBUF inp, unsigned long pktlen );
static int  parse_symkeyenc( IOBUF inp, int pkttype, unsigned long pktlen,
							     PACKET *packet );
static int  parse_pubkeyenc( IOBUF inp, int pkttype, unsigned long pktlen,
							     PACKET *packet );
static int  parse_signature( IOBUF inp, int pkttype, unsigned long pktlen,
							 PKT_signature *sig );
static int  parse_onepass_sig( IOBUF inp, int pkttype, unsigned long pktlen,
							PKT_onepass_sig *ops );
static int  parse_certificate( IOBUF inp, int pkttype, unsigned long pktlen,
				      byte *hdr, int hdrlen, PACKET *packet );
static int  parse_user_id( IOBUF inp, int pkttype, unsigned long pktlen,
							   PACKET *packet );
static int  parse_comment( IOBUF inp, int pkttype, unsigned long pktlen,
							   PACKET *packet );
static void parse_trust( IOBUF inp, int pkttype, unsigned long pktlen );
static int  parse_plaintext( IOBUF inp, int pkttype, unsigned long pktlen,
								PACKET *pkt );
static int  parse_compressed( IOBUF inp, int pkttype, unsigned long pktlen,
							   PACKET *packet );
static int  parse_encrypted( IOBUF inp, int pkttype, unsigned long pktlen,
							   PACKET *packet );

static unsigned short
read_16(IOBUF inp)
{
    unsigned short a;
    a = iobuf_get_noeof(inp) << 8;
    a |= iobuf_get_noeof(inp);
    return a;
}

static unsigned long
read_32(IOBUF inp)
{
    unsigned long a;
    a =  iobuf_get_noeof(inp) << 24;
    a |= iobuf_get_noeof(inp) << 16;
    a |= iobuf_get_noeof(inp) << 8;
    a |= iobuf_get_noeof(inp);
    return a;
}

static unsigned long
buffer_to_u32( const byte *buffer )
{
    unsigned long a;
    a =  *buffer << 24;
    a |= buffer[1] << 16;
    a |= buffer[2] << 8;
    a |= buffer[3];
    return a;
}

int
set_packet_list_mode( int mode )
{
    int old = list_mode;
    list_mode = mode;
    mpi_print_mode = DBG_MPI;
    return old;
}

/****************
 * Parse a Packet and return it in packet
 * Returns: 0 := valid packet in pkt
 *	   -1 := no more packets
 *	   >0 := error
 * Note: The function may return an error and a partly valid packet;
 * caller must free this packet.
 */
int
parse_packet( IOBUF inp, PACKET *pkt )
{
    int skip, rc;

    do {
	rc = parse( inp, pkt, 0, NULL, &skip, NULL, 0 );
    } while( skip );
    return rc;
}

/****************
 * Like parse packet, but only return packets of the given type.
 */
int
search_packet( IOBUF inp, PACKET *pkt, int pkttype, ulong *retpos )
{
    int skip, rc;

    do {
	rc = parse( inp, pkt, pkttype, retpos, &skip, NULL, 0 );
    } while( skip );
    return rc;
}

/****************
 * Copy all packets from INP to OUT, thereby removing unused spaces.
 */
int
copy_all_packets( IOBUF inp, IOBUF out )
{
    PACKET pkt;
    int skip, rc=0;
    do {
	init_packet(&pkt);
    } while( !(rc = parse( inp, &pkt, 0, NULL, &skip, out, 0 )));
    return rc;
}

/****************
 * Copy some packets from INP to OUT, thereby removing unused spaces.
 * Stop at offset STOPoff (i.e. don't copy packets at this or later offsets)
 */
int
copy_some_packets( IOBUF inp, IOBUF out, ulong stopoff )
{
    PACKET pkt;
    int skip, rc=0;
    do {
	if( iobuf_tell(inp) >= stopoff )
	    return 0;
	init_packet(&pkt);
    } while( !(rc = parse( inp, &pkt, 0, NULL, &skip, out, 0 )) );
    return rc;
}

/****************
 * Skip over N packets
 */
int
skip_some_packets( IOBUF inp, unsigned n )
{
    int skip, rc=0;
    PACKET pkt;

    for( ;n && !rc; n--) {
	init_packet(&pkt);
	rc = parse( inp, &pkt, 0, NULL, &skip, NULL, 1 );
    }
    return rc;
}

/****************
 * Parse packet. Set the variable skip points to to 1 if the packet
 * should be skipped; this is the case if either there is a
 * requested packet type and the parsed packet doesn't match or the
 * packet-type is 0, indicating deleted stuff.
 * if OUT is not NULL, a special copymode is used.
 */
static int
parse( IOBUF inp, PACKET *pkt, int reqtype, ulong *retpos,
       int *skip, IOBUF out, int do_skip )
{
    int rc, c, ctb, pkttype, lenbytes;
    unsigned long pktlen;
    byte hdr[8];
    int hdrlen;
    int pgp3 = 0;

    *skip = 0;
    assert( !pkt->pkt.generic );
    if( retpos )
	*retpos = iobuf_tell(inp);
    if( (ctb = iobuf_get(inp)) == -1 )
	return -1;
    hdrlen=0;
    hdr[hdrlen++] = ctb;
    if( !(ctb & 0x80) ) {
	log_error("%s: invalid packet (ctb=%02x)\n", iobuf_where(inp), ctb );
	return G10ERR_INVALID_PACKET;
    }
    pktlen = 0;
    pgp3 = !!(ctb & 0x40);
    if( pgp3 ) {
	pkttype =  ctb & 0x3f;
	if( (c = iobuf_get(inp)) == -1 ) {
	    log_error("%s: 1st length byte missing\n", iobuf_where(inp) );
	    return G10ERR_INVALID_PACKET;
	}
	hdr[hdrlen++] = c;
	if( c < 192 )
	    pktlen = c;
	else if( c < 224 ) {
	    pktlen = (c - 192) * 256;
	    if( (c = iobuf_get(inp)) == -1 ) {
		log_error("%s: 2nd length byte missing\n", iobuf_where(inp) );
		return G10ERR_INVALID_PACKET;
	    }
	    hdr[hdrlen++] = c;
	    pktlen += c + 192;
	}
	else if( c < 255 ) {
	    pktlen  = (hdr[hdrlen++] = iobuf_get_noeof(inp)) << 24;
	    pktlen |= (hdr[hdrlen++] = iobuf_get_noeof(inp)) << 16;
	    pktlen |= (hdr[hdrlen++] = iobuf_get_noeof(inp)) << 8;
	    if( (c = iobuf_get(inp)) == -1 ) {
		log_error("%s: 4 byte length invalid\n", iobuf_where(inp) );
		return G10ERR_INVALID_PACKET;
	    }
	    pktlen |= (hdr[hdrlen++] = c );
	}
	else { /* partial body length */
	    log_debug("partial body length of %lu bytes\n", pktlen );
	    iobuf_set_partial_block_mode(inp, pktlen);
	    pktlen = 0;/* to indicate partial length */
	}
    }
    else {
	pkttype = (ctb>>2)&0xf;
	lenbytes = ((ctb&3)==3)? 0 : (1<<(ctb & 3));
	if( !lenbytes ) {
	    pktlen = 0; /* don't know the value */
	    if( pkttype != PKT_COMPRESSED )
		iobuf_set_block_mode(inp, 1);
	}
	else {
	    for( ; lenbytes; lenbytes-- ) {
		pktlen <<= 8;
		pktlen |= hdr[hdrlen++] = iobuf_get_noeof(inp);
	    }
	}
    }

    if( out && pkttype	) {
	if( iobuf_write( out, hdr, hdrlen ) == -1 )
	    rc = G10ERR_WRITE_FILE;
	else
	    rc = copy_packet(inp, out, pkttype, pktlen );
	return rc;
    }

    if( do_skip || !pkttype || (reqtype && pkttype != reqtype) ) {
	skip_packet(inp, pkttype, pktlen);
	*skip = 1;
	return 0;
    }

    if( DBG_PACKET )
	log_debug("parse_packet(iob=%d): type=%d length=%lu%s\n",
		   iobuf_id(inp), pkttype, pktlen, pgp3?" (pgp3)":"" );
    pkt->pkttype = pkttype;
    rc = G10ERR_UNKNOWN_PACKET; /* default error */
    switch( pkttype ) {
      case PKT_PUBLIC_CERT:
      case PKT_PUBKEY_SUBCERT:
	pkt->pkt.public_cert = m_alloc_clear(sizeof *pkt->pkt.public_cert );
	rc = parse_certificate(inp, pkttype, pktlen, hdr, hdrlen, pkt );
	break;
      case PKT_SECRET_CERT:
      case PKT_SECKEY_SUBCERT:
	pkt->pkt.secret_cert = m_alloc_clear(sizeof *pkt->pkt.secret_cert );
	rc = parse_certificate(inp, pkttype, pktlen, hdr, hdrlen, pkt );
	break;
      case PKT_SYMKEY_ENC:
	rc = parse_symkeyenc( inp, pkttype, pktlen, pkt );
	break;
      case PKT_PUBKEY_ENC:
	rc = parse_pubkeyenc(inp, pkttype, pktlen, pkt );
	break;
      case PKT_SIGNATURE:
	pkt->pkt.signature = m_alloc_clear(sizeof *pkt->pkt.signature );
	rc = parse_signature(inp, pkttype, pktlen, pkt->pkt.signature );
	break;
      case PKT_ONEPASS_SIG:
	pkt->pkt.onepass_sig = m_alloc_clear(sizeof *pkt->pkt.onepass_sig );
	rc = parse_onepass_sig(inp, pkttype, pktlen, pkt->pkt.onepass_sig );
	break;
      case PKT_USER_ID:
	rc = parse_user_id(inp, pkttype, pktlen, pkt );
	break;
      case PKT_OLD_COMMENT:
      case PKT_COMMENT:
	rc = parse_comment(inp, pkttype, pktlen, pkt);
	break;
      case PKT_RING_TRUST:
	parse_trust(inp, pkttype, pktlen);
	break;
      case PKT_PLAINTEXT:
	rc = parse_plaintext(inp, pkttype, pktlen, pkt );
	break;
      case PKT_COMPRESSED:
	rc = parse_compressed(inp, pkttype, pktlen, pkt );
	break;
      case PKT_ENCRYPTED:
	rc = parse_encrypted(inp, pkttype, pktlen, pkt );
	break;
      default:
	skip_packet(inp, pkttype, pktlen);
	break;
    }

    return rc;
}

static void
dump_hex_line( int c, int *i )
{
    if( *i && !(*i%8) ) {
	if( *i && !(*i%24) )
	    printf("\n%4d:", *i );
	else
	    putchar(' ');
    }
    if( c == -1 )
	printf(" EOF" );
    else
	printf(" %02x", c );
    ++*i;
}


static int
copy_packet( IOBUF inp, IOBUF out, int pkttype, unsigned long pktlen )
{
    int n;
    char buf[100];

    if( iobuf_in_block_mode(inp) ) {
	while( (n = iobuf_read( inp, buf, 100 )) != -1 )
	    if( iobuf_write(out, buf, n ) )
		return G10ERR_WRITE_FILE; /* write error */
    }
    else if( !pktlen && pkttype == PKT_COMPRESSED ) {
	/* compressed packet, copy till EOF */
	while( (n = iobuf_read( inp, buf, 100 )) != -1 )
	    if( iobuf_write(out, buf, n ) )
		return G10ERR_WRITE_FILE; /* write error */
    }
    else {
	for( ; pktlen; pktlen -= n ) {
	    n = pktlen > 100 ? 100 : pktlen;
	    n = iobuf_read( inp, buf, n );
	    if( n == -1 )
		return G10ERR_READ_FILE;
	    if( iobuf_write(out, buf, n ) )
		return G10ERR_WRITE_FILE; /* write error */
	}
    }
    return 0;
}


static void
skip_packet( IOBUF inp, int pkttype, unsigned long pktlen )
{
    if( list_mode ) {
	printf(":unknown packet: type %2d, length %lu\n", pkttype, pktlen );
	if( pkttype ) {
	    int c, i=0 ;
	    printf("dump:");
	    if( iobuf_in_block_mode(inp) ) {
		while( (c=iobuf_get(inp)) != -1 )
		    dump_hex_line(c, &i);
	    }
	    else {
		for( ; pktlen; pktlen-- )
		    dump_hex_line(iobuf_get(inp), &i);
	    }
	    putchar('\n');
	    return;
	}
    }
    skip_rest(inp,pktlen);
}

static void
skip_rest( IOBUF inp, unsigned long pktlen )
{
    if( iobuf_in_block_mode(inp) ) {
	while( iobuf_get(inp) != -1 )
		;
    }
    else {
	for( ; pktlen; pktlen-- )
	    iobuf_get(inp);
    }
}


static int
parse_symkeyenc( IOBUF inp, int pkttype, unsigned long pktlen, PACKET *packet )
{
    PKT_symkey_enc *k;
    int i, version, s2kmode, cipher_algo, hash_algo, seskeylen, minlen;

    if( pktlen < 4 ) {
	log_error("packet(%d) too short\n", pkttype);
	goto leave;
    }
    version = iobuf_get_noeof(inp); pktlen--;
    if( version != 4 ) {
	log_error("packet(%d) with unknown version %d\n", pkttype, version);
	goto leave;
    }
    if( pktlen > 200 ) { /* (we encode the seskeylen in a byte) */
	log_error("packet(%d) too large\n", pkttype);
	goto leave;
    }
    cipher_algo = iobuf_get_noeof(inp); pktlen--;
    s2kmode = iobuf_get_noeof(inp); pktlen--;
    hash_algo = iobuf_get_noeof(inp); pktlen--;
    switch( s2kmode ) {
      case 0:  /* simple s2k */
	minlen = 0;
	break;
      case 1:  /* salted s2k */
	minlen = 8;
	break;
      case 4:  /* iterated+salted s2k */
	minlen = 12;
	break;
      default:
	log_error("unknown S2K %d\n", s2kmode );
	goto leave;
    }
    if( minlen > pktlen ) {
	log_error("packet with S2K %d too short\n", s2kmode );
	goto leave;
    }
    seskeylen = pktlen - minlen;
    k = packet->pkt.symkey_enc = m_alloc_clear( sizeof *packet->pkt.symkey_enc
						+ seskeylen - 1 );
    k->version = version;
    k->cipher_algo = cipher_algo;
    k->s2k.mode = s2kmode;
    k->s2k.hash_algo = hash_algo;
    if( s2kmode == 1 || s2kmode == 4 ) {
	for(i=0; i < 8 && pktlen; i++, pktlen-- )
	    k->s2k.salt[i] = iobuf_get_noeof(inp);
    }
    if( s2kmode == 4 ) {
	k->s2k.count = read_32(inp); pktlen -= 4;
    }
    k->seskeylen = seskeylen;
    for(i=0; i < seskeylen && pktlen; i++, pktlen-- )
	k->seskey[i] = iobuf_get_noeof(inp);
    assert( !pktlen );

    if( list_mode ) {
	printf(":symkey enc packet: version %d, cipher %d, s2k %d, hash %d\n",
			    version, cipher_algo, s2kmode, hash_algo);
	if( s2kmode == 1  || s2kmode == 4 ) {
	    printf("\tsalt ");
	    for(i=0; i < 8; i++ )
		printf("%02x", k->s2k.salt[i]);
	    if( s2kmode == 4 )
		printf(", count %lu\n", (ulong)k->s2k.count );
	    printf("\n");
	}
    }

  leave:
    skip_rest(inp, pktlen);
    return 0;
}

static int
parse_pubkeyenc( IOBUF inp, int pkttype, unsigned long pktlen, PACKET *packet )
{
    unsigned n;
    PKT_pubkey_enc *k;

    k = packet->pkt.pubkey_enc = m_alloc(sizeof *packet->pkt.pubkey_enc );
    if( pktlen < 12 ) {
	log_error("packet(%d) too short\n", pkttype);
	goto leave;
    }
    k->version = iobuf_get_noeof(inp); pktlen--;
    if( k->version != 2 && k->version != 3 ) {
	log_error("packet(%d) with unknown version %d\n", pkttype, k->version);
	goto leave;
    }
    k->keyid[0] = read_32(inp); pktlen -= 4;
    k->keyid[1] = read_32(inp); pktlen -= 4;
    k->pubkey_algo = iobuf_get_noeof(inp); pktlen--;
    if( list_mode )
	printf(":pubkey enc packet: version %d, algo %d, keyid %08lX%08lX\n",
	  k->version, k->pubkey_algo, (ulong)k->keyid[0], (ulong)k->keyid[1]);
    if( is_ELGAMAL(k->pubkey_algo) ) {
	n = pktlen;
	k->d.elg.a = mpi_read(inp, &n, 0); pktlen -=n;
	n = pktlen;
	k->d.elg.b = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf("\telg a: ");
	    mpi_print(stdout, k->d.elg.a, mpi_print_mode );
	    printf("\n\telg b: ");
	    mpi_print(stdout, k->d.elg.b, mpi_print_mode );
	    putchar('\n');
	}
    }
    else if( is_RSA(k->pubkey_algo) ) {
	n = pktlen;
	k->d.rsa.rsa_integer = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf("\trsa integer: ");
	    mpi_print(stdout, k->d.rsa.rsa_integer, mpi_print_mode );
	    putchar('\n');
	}
    }
    else if( list_mode )
	printf("\tunknown algorithm %d\n", k->pubkey_algo );


  leave:
    skip_rest(inp, pktlen);
    return 0;
}


const byte *
parse_sig_subpkt( const byte *buffer, sigsubpkttype_t reqtype, size_t *ret_n )
{
    int buflen;
    int type;
    int critical;
    size_t n;

    if( !buffer )
	return NULL;
    buflen = (*buffer << 8) | buffer[1];
    buffer += 2;
    for(;;) {
	if( !buflen )
	    return NULL; /* end of packets; not found */
	n = *buffer++; buflen--;
	if( n == 255 ) {
	    if( buflen < 4 )
		goto too_short;
	    n = (buffer[0] << 24) | (buffer[1] << 16)
				  | (buffer[2] << 8) | buffer[3];
	    buffer += 4;
	    buflen -= 4;

	}
	else if( n >= 192 ) {
	    if( buflen < 2 )
		goto too_short;
	    n = (( n - 192 ) << 8) + *buffer + 192;
	    buflen--;
	}
	if( buflen < n )
	    goto too_short;
	type = *buffer;
	if( type & 0x80 ) {
	    type &= 0x7f;
	    critical = 1;
	}
	else
	    critical = 0;
	if( reqtype < 0 ) { /* list packets */
	    printf("\t%ssubpacket %d of length %u (%s)\n",
	    reqtype == SIGSUBPKT_LIST_HASHED ? "hashed ":"", type, (unsigned)n,
	     type == SIGSUBPKT_SIG_CREATED ? "signature creation time"
	   : type == SIGSUBPKT_SIG_EXPIRE  ? "signature expiration time"
	   : type == SIGSUBPKT_EXPORTABLE  ? "exportable"
	   : type == SIGSUBPKT_TRUST	   ? "trust signature"
	   : type == SIGSUBPKT_REGEXP	   ? "regular expression"
	   : type == SIGSUBPKT_REVOCABLE   ? "revocable"
	   : type == SIGSUBPKT_KEY_EXPIRE  ? "key expiration time"
	   : type == SIGSUBPKT_ARR	   ? "additional recipient request"
	   : type == SIGSUBPKT_PREF_SYM    ? "preferred symmetric algorithms"
	   : type == SIGSUBPKT_REV_KEY	   ? "revocation key"
	   : type == SIGSUBPKT_ISSUER	   ? "issuer key ID"
	   : type == SIGSUBPKT_NOTATION    ? "notation data"
	   : type == SIGSUBPKT_PREF_HASH   ? "preferred hash algorithms"
	   : type == SIGSUBPKT_PREF_COMPR  ? "preferred compression algorithms"
	   : type == SIGSUBPKT_KS_FLAGS    ? "key server preferences"
	   : type == SIGSUBPKT_PREF_KS	   ? "preferred key server"
	   : type == SIGSUBPKT_PRIMARY_UID ? "primary user id"
	   : type == SIGSUBPKT_POLICY	   ? "policy URL"
	   : type == SIGSUBPKT_KEY_FLAGS   ? "key flags"
	   : type == SIGSUBPKT_SIGNERS_UID ? "signer's user id"
			      : "?");
	}
	else if( type == reqtype )
	    break; /* found */
	buffer += n; buflen -=n;
    }
    buffer++;
    n--;
    if( n > buflen )
	goto too_short;
    if( ret_n )
	*ret_n = n;
    switch( type ) {
      case SIGSUBPKT_SIG_CREATED:
	if( n < 4 )
	    break;
	return buffer;
      case SIGSUBPKT_ISSUER:/* issuer key ID */
	if( n < 8 )
	    break;
	return buffer;
      default: BUG(); /* not yet needed */
    }
    log_error("subpacket of type %d too short\n", type);
    return NULL;

  too_short:
    log_error("buffer shorter than subpacket\n");
    return NULL;
}


static int
parse_signature( IOBUF inp, int pkttype, unsigned long pktlen,
					  PKT_signature *sig )
{
    int md5_len=0;
    unsigned n;
    int is_v4=0;
    int rc=0;

    if( pktlen < 16 ) {
	log_error("packet(%d) too short\n", pkttype);
	goto leave;
    }
    sig->version = iobuf_get_noeof(inp); pktlen--;
    if( sig->version == 4 )
	is_v4=1;
    else if( sig->version != 2 && sig->version != 3 ) {
	log_error("packet(%d) with unknown version %d\n", pkttype, sig->version);
	goto leave;
    }

    if( !is_v4 ) {
	md5_len = iobuf_get_noeof(inp); pktlen--;
    }
    sig->sig_class = iobuf_get_noeof(inp); pktlen--;
    if( !is_v4 ) {
	sig->timestamp = read_32(inp); pktlen -= 4;
	sig->keyid[0] = read_32(inp); pktlen -= 4;
	sig->keyid[1] = read_32(inp); pktlen -= 4;
    }
    sig->pubkey_algo = iobuf_get_noeof(inp); pktlen--;
    sig->digest_algo = iobuf_get_noeof(inp); pktlen--;
    if( is_v4 ) { /* read subpackets */
	n = read_16(inp); pktlen -= 2; /* length of hashed data */
	if( n > 10000 ) {
	    log_error("signature packet: hashed data too long\n");
	    rc = G10ERR_INVALID_PACKET;
	    goto leave;
	}
	if( n ) {
	    sig->hashed_data = m_alloc( n + 2 );
	    sig->hashed_data[0] = n << 8;
	    sig->hashed_data[1] = n;
	    if( iobuf_read(inp, sig->hashed_data+2, n ) != n ) {
		log_error("premature eof while reading hashed signature data\n");
		rc = -1;
		goto leave;
	    }
	    pktlen -= n;
	}
	n = read_16(inp); pktlen -= 2; /* length of unhashed data */
	if( n > 10000 ) {
	    log_error("signature packet: unhashed data too long\n");
	    rc = G10ERR_INVALID_PACKET;
	    goto leave;
	}
	if( n ) {
	    sig->unhashed_data = m_alloc( n + 2 );
	    sig->unhashed_data[0] = n << 8;
	    sig->unhashed_data[1] = n;
	    if( iobuf_read(inp, sig->unhashed_data+2, n ) != n ) {
		log_error("premature eof while reading unhashed signature data\n");
		rc = -1;
		goto leave;
	    }
	    pktlen -= n;
	}
    }

    if( pktlen < 5 ) { /* sanity check */
	log_error("packet(%d) too short\n", pkttype);
	rc = G10ERR_INVALID_PACKET;
	goto leave;
    }

    sig->digest_start[0] = iobuf_get_noeof(inp); pktlen--;
    sig->digest_start[1] = iobuf_get_noeof(inp); pktlen--;

    if( is_v4 ) { /*extract required information */
	const byte *p;
	p = parse_sig_subpkt( sig->hashed_data, SIGSUBPKT_SIG_CREATED, NULL );
	if( !p )
	    log_error("signature packet without timestamp\n");
	else
	    sig->timestamp = buffer_to_u32(p);
	p = parse_sig_subpkt( sig->unhashed_data, SIGSUBPKT_ISSUER, NULL );
	if( !p )
	    log_error("signature packet without keyid\n");
	else {
	    sig->keyid[0] = buffer_to_u32(p);
	    sig->keyid[1] = buffer_to_u32(p+4);
	}
    }

    if( list_mode ) {
	printf(":signature packet: algo %d, keyid %08lX%08lX\n"
	       "\tversion %d, created %lu, md5len %d, sigclass %02x\n"
	       "\tdigest algo %d, begin of digest %02x %02x\n",
		sig->pubkey_algo,
		(ulong)sig->keyid[0], (ulong)sig->keyid[1],
		sig->version, (ulong)sig->timestamp, md5_len, sig->sig_class,
		sig->digest_algo,
		sig->digest_start[0], sig->digest_start[1] );
	if( is_v4 ) {
	    parse_sig_subpkt( sig->hashed_data,  SIGSUBPKT_LIST_HASHED, NULL );
	    parse_sig_subpkt( sig->unhashed_data,SIGSUBPKT_LIST_UNHASHED, NULL);
	}
    }
    if( is_ELGAMAL(sig->pubkey_algo) ) {
	n = pktlen;
	sig->d.elg.a = mpi_read(inp, &n, 0 ); pktlen -=n;
	n = pktlen;
	sig->d.elg.b = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf("\telg a: ");
	    mpi_print(stdout, sig->d.elg.a, mpi_print_mode );
	    printf("\n\telg b: ");
	    mpi_print(stdout, sig->d.elg.b, mpi_print_mode );
	    putchar('\n');
	}
    }
    else if( sig->pubkey_algo == PUBKEY_ALGO_DSA ) {
	n = pktlen;
	sig->d.dsa.r = mpi_read(inp, &n, 0 ); pktlen -=n;
	n = pktlen;
	sig->d.dsa.s = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf("\tdsa r: ");
	    mpi_print(stdout, sig->d.elg.a, mpi_print_mode );
	    printf("\n\tdsa s: ");
	    mpi_print(stdout, sig->d.elg.b, mpi_print_mode );
	    putchar('\n');
	}
    }
    else if( is_RSA(sig->pubkey_algo) ) {
	n = pktlen;
	sig->d.rsa.rsa_integer = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf("\trsa integer: ");
	    mpi_print(stdout, sig->d.rsa.rsa_integer, mpi_print_mode );
	    putchar('\n');
	}
    }
    else if( list_mode )
	printf("\tunknown algorithm %d\n", sig->pubkey_algo );


  leave:
    skip_rest(inp, pktlen);
    return rc;
}


static int
parse_onepass_sig( IOBUF inp, int pkttype, unsigned long pktlen,
					     PKT_onepass_sig *ops )
{
    int version;

    if( pktlen < 13 ) {
	log_error("packet(%d) too short\n", pkttype);
	goto leave;
    }
    version = iobuf_get_noeof(inp); pktlen--;
    if( version != 3 ) {
	log_error("onepass_sig with unknown version %d\n", version);
	goto leave;
    }
    ops->sig_class = iobuf_get_noeof(inp); pktlen--;
    ops->digest_algo = iobuf_get_noeof(inp); pktlen--;
    ops->pubkey_algo = iobuf_get_noeof(inp); pktlen--;
    ops->keyid[0] = read_32(inp); pktlen -= 4;
    ops->keyid[1] = read_32(inp); pktlen -= 4;
    ops->last = iobuf_get_noeof(inp); pktlen--;
    if( list_mode )
	printf(":onepass_sig packet: keyid %08lX%08lX\n"
	       "\tversion %d, sigclass %02x, digest %d, pubkey %d, last=%d\n",
		(ulong)ops->keyid[0], (ulong)ops->keyid[1],
		version, ops->sig_class,
		ops->digest_algo, ops->pubkey_algo, ops->last );


  leave:
    skip_rest(inp, pktlen);
    return 0;
}




static int
parse_certificate( IOBUF inp, int pkttype, unsigned long pktlen,
			      byte *hdr, int hdrlen, PACKET *pkt )
{
    int i, version, algorithm;
    unsigned n;
    unsigned long timestamp;
    unsigned short valid_period;
    int is_v4=0;
    int rc=0;

    version = iobuf_get_noeof(inp); pktlen--;
    if( pkttype == PKT_PUBKEY_SUBCERT && version == '#' ) {
	/* early versions of G10 use old PGP comments packets;
	 * luckily all those comments are started by a hash */
	if( list_mode ) {
	    printf(":rfc1991 comment packet: \"" );
	    for( ; pktlen; pktlen-- ) {
		int c;
		c = iobuf_get_noeof(inp);
		if( c >= ' ' && c <= 'z' )
		    putchar(c);
		else
		    printf("\\x%02x", c );
	    }
	    printf("\"\n");
	}
	skip_rest(inp, pktlen);
	return 0;
    }
    else if( version == 4 )
	is_v4=1;
    else if( version != 2 && version != 3 ) {
	log_error("packet(%d) with unknown version %d\n", pkttype, version);
	goto leave;
    }

    if( pktlen < 11 ) {
	log_error("packet(%d) too short\n", pkttype);
	goto leave;
    }

    timestamp = read_32(inp); pktlen -= 4;
    if( is_v4 )
	valid_period = 0;
    else {
	valid_period = read_16(inp); pktlen -= 2;
    }
    algorithm = iobuf_get_noeof(inp); pktlen--;
    if( list_mode )
	printf(":%s key packet:\n"
	       "\tversion %d, algo %d, created %lu, valid for %hu days\n",
		pkttype == PKT_PUBLIC_CERT? "public" :
		pkttype == PKT_SECRET_CERT? "secret" :
		pkttype == PKT_PUBKEY_SUBCERT? "public sub" :
		pkttype == PKT_SECKEY_SUBCERT? "secret sub" : "??",
		version, algorithm, timestamp, valid_period );
    if( pkttype == PKT_SECRET_CERT || pkttype == PKT_SECKEY_SUBCERT )  {
	pkt->pkt.secret_cert->timestamp = timestamp;
	pkt->pkt.secret_cert->valid_days = valid_period;
	pkt->pkt.secret_cert->hdrbytes = hdrlen;
	pkt->pkt.secret_cert->version = version;
	pkt->pkt.secret_cert->pubkey_algo = algorithm;
    }
    else {
	pkt->pkt.public_cert->timestamp = timestamp;
	pkt->pkt.public_cert->valid_days = valid_period;
	pkt->pkt.public_cert->hdrbytes	  = hdrlen;
	pkt->pkt.public_cert->version	  = version;
	pkt->pkt.public_cert->pubkey_algo = algorithm;
    }

    if( is_ELGAMAL(algorithm) ) {
	MPI elg_p, elg_g, elg_y;
	n = pktlen; elg_p = mpi_read(inp, &n, 0 ); pktlen -=n;
	n = pktlen; elg_g = mpi_read(inp, &n, 0 ); pktlen -=n;
	n = pktlen; elg_y = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf(  "\telg p: ");
	    mpi_print(stdout, elg_p, mpi_print_mode  );
	    printf("\n\telg g: ");
	    mpi_print(stdout, elg_g, mpi_print_mode  );
	    printf("\n\telg y: ");
	    mpi_print(stdout, elg_y, mpi_print_mode  );
	    putchar('\n');
	}
	if( pkttype == PKT_PUBLIC_CERT || pkttype == PKT_PUBKEY_SUBCERT ) {
	    pkt->pkt.public_cert->d.elg.p = elg_p;
	    pkt->pkt.public_cert->d.elg.g = elg_g;
	    pkt->pkt.public_cert->d.elg.y = elg_y;
	}
	else {
	    PKT_secret_cert *cert = pkt->pkt.secret_cert;
	    byte temp[8];

	    pkt->pkt.secret_cert->d.elg.p = elg_p;
	    pkt->pkt.secret_cert->d.elg.g = elg_g;
	    pkt->pkt.secret_cert->d.elg.y = elg_y;
	    cert->protect.algo = iobuf_get_noeof(inp); pktlen--;
	    if( cert->protect.algo ) {
		cert->is_protected = 1;
		cert->protect.s2k.count = 0;
		if( cert->protect.algo == 255 ) {
		    if( pktlen < 3 ) {
			rc = G10ERR_INVALID_PACKET;
			goto leave;
		    }
		    cert->protect.algo = iobuf_get_noeof(inp); pktlen--;
		    cert->protect.s2k.mode  = iobuf_get_noeof(inp); pktlen--;
		    cert->protect.s2k.hash_algo = iobuf_get_noeof(inp); pktlen--;
		    switch( cert->protect.s2k.mode ) {
		      case 1:
		      case 4:
			for(i=0; i < 8 && pktlen; i++, pktlen-- )
			    temp[i] = iobuf_get_noeof(inp);
			memcpy(cert->protect.s2k.salt, temp, 8 );
			break;
		    }
		    switch( cert->protect.s2k.mode ) {
		      case 0: if( list_mode ) printf(  "\tsimple S2K" );
			break;
		      case 1: if( list_mode ) printf(  "\tsalted S2K" );
			break;
		      case 4: if( list_mode ) printf(  "\titer+salt S2K" );
			break;
		      default:
			if( list_mode )
			    printf(  "\tunknown S2K %d\n",
						cert->protect.s2k.mode );
			rc = G10ERR_INVALID_PACKET;
			goto leave;
		    }

		    if( list_mode ) {
			printf(", algo: %d, hash: %d",
					 cert->protect.algo,
					 cert->protect.s2k.hash_algo );
			if( cert->protect.s2k.mode == 1
			    || cert->protect.s2k.mode == 4 ) {
			    printf(", salt: ");
			    for(i=0; i < 8; i++ )
				printf("%02x", cert->protect.s2k.salt[i]);
			}
			putchar('\n');
		    }

		    if( cert->protect.s2k.mode == 4 ) {
			if( pktlen < 4 ) {
			    rc = G10ERR_INVALID_PACKET;
			    goto leave;
			}
			cert->protect.s2k.count = read_32(inp);
			pktlen -= 4;
		    }

		}
		else {
		    if( list_mode )
			printf(  "\tprotect algo: %d\n",
						cert->protect.algo);
		    /* old version, we don't have a S2K, so we fake one */
		    cert->protect.s2k.mode = 0;
		    /* We need this kludge to cope with old GNUPG versions */
		    cert->protect.s2k.hash_algo =
			 cert->protect.algo == CIPHER_ALGO_BLOWFISH160?
				      DIGEST_ALGO_RMD160 : DIGEST_ALGO_MD5;
		}
		if( pktlen < 8 ) {
		    rc = G10ERR_INVALID_PACKET;
		    goto leave;
		}
		for(i=0; i < 8 && pktlen; i++, pktlen-- )
		    temp[i] = iobuf_get_noeof(inp);
		if( list_mode ) {
		    printf(  "\tprotect IV: ");
		    for(i=0; i < 8; i++ )
			printf(" %02x", temp[i] );
		    putchar('\n');
		}
		memcpy(cert->protect.iv, temp, 8 );
	    }
	    else
		cert->is_protected = 0;
	    /* It does not make sense to read it into secure memory.
	     * If the user is so careless, not to protect his secret key,
	     * we can assume, that he operates an open system :=(.
	     * So we put the key into secure memory when we unprotect it. */
	    n = pktlen; cert->d.elg.x = mpi_read(inp, &n, 0 ); pktlen -=n;

	    cert->csum = read_16(inp); pktlen -= 2;
	    if( list_mode ) {
	    printf("\telg x: ");
	    mpi_print(stdout, cert->d.elg.x, mpi_print_mode  );
	    putchar('\n');
		printf("\t[secret value x is not shown]\n"
		       "\tchecksum: %04hx\n", cert->csum);
	    }
	  /*log_mpidump("elg p=", cert->d.elg.p );
	    log_mpidump("elg g=", cert->d.elg.g );
	    log_mpidump("elg y=", cert->d.elg.y );
	    log_mpidump("elg x=", cert->d.elg.x ); */
	}
    }
    else if( algorithm == PUBKEY_ALGO_DSA ) {
	MPI dsa_p, dsa_q, dsa_g, dsa_y;
	n = pktlen; dsa_p = mpi_read(inp, &n, 0 ); pktlen -=n;
	n = pktlen; dsa_q = mpi_read(inp, &n, 0 ); pktlen -=n;
	n = pktlen; dsa_g = mpi_read(inp, &n, 0 ); pktlen -=n;
	n = pktlen; dsa_y = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf(  "\tdsa p: ");
	    mpi_print(stdout, dsa_p, mpi_print_mode  );
	    printf("\n\tdsa q: ");
	    mpi_print(stdout, dsa_q, mpi_print_mode  );
	    printf("\n\tdsa g: ");
	    mpi_print(stdout, dsa_g, mpi_print_mode  );
	    printf("\n\tdsa y: ");
	    mpi_print(stdout, dsa_y, mpi_print_mode  );
	    putchar('\n');
	}
	if( pkttype == PKT_PUBLIC_CERT || pkttype == PKT_PUBKEY_SUBCERT ) {
	    pkt->pkt.public_cert->d.dsa.p = dsa_p;
	    pkt->pkt.public_cert->d.dsa.q = dsa_q;
	    pkt->pkt.public_cert->d.dsa.g = dsa_g;
	    pkt->pkt.public_cert->d.dsa.y = dsa_y;
	}
	else {
	    PKT_secret_cert *cert = pkt->pkt.secret_cert;
	    byte temp[8];

	    pkt->pkt.secret_cert->d.dsa.p = dsa_p;
	    pkt->pkt.secret_cert->d.dsa.q = dsa_q;
	    pkt->pkt.secret_cert->d.dsa.g = dsa_g;
	    pkt->pkt.secret_cert->d.dsa.y = dsa_y;
	    cert->protect.algo = iobuf_get_noeof(inp); pktlen--;
	    if( cert->protect.algo ) {
		cert->is_protected = 1;
		cert->protect.s2k.count = 0;
		if( cert->protect.algo == 255 ) {
		    if( pktlen < 3 ) {
			rc = G10ERR_INVALID_PACKET;
			goto leave;
		    }
		    cert->protect.algo = iobuf_get_noeof(inp); pktlen--;
		    cert->protect.s2k.mode  = iobuf_get_noeof(inp); pktlen--;
		    cert->protect.s2k.hash_algo = iobuf_get_noeof(inp); pktlen--;
		    switch( cert->protect.s2k.mode ) {
		      case 1:
		      case 4:
			for(i=0; i < 8 && pktlen; i++, pktlen-- )
			    temp[i] = iobuf_get_noeof(inp);
			memcpy(cert->protect.s2k.salt, temp, 8 );
			break;
		    }
		    switch( cert->protect.s2k.mode ) {
		      case 0: if( list_mode ) printf(  "\tsimple S2K" );
			break;
		      case 1: if( list_mode ) printf(  "\tsalted S2K" );
			break;
		      case 4: if( list_mode ) printf(  "\titer+salt S2K" );
			break;
		      default:
			if( list_mode )
			    printf(  "\tunknown S2K %d\n",
						    cert->protect.s2k.mode );
			rc = G10ERR_INVALID_PACKET;
			goto leave;
		    }

		    if( list_mode ) {
			printf(", algo: %d, hash: %d",
					 cert->protect.algo,
					 cert->protect.s2k.hash_algo );
			if( cert->protect.s2k.mode == 1
			    || cert->protect.s2k.mode == 4 ){
			    printf(", salt: ");
			    for(i=0; i < 8; i++ )
				printf("%02x", cert->protect.s2k.salt[i]);
			}
			putchar('\n');
		    }

		    if( cert->protect.s2k.mode == 4 ) {
			if( pktlen < 4 ) {
			    rc = G10ERR_INVALID_PACKET;
			    goto leave;
			}
			cert->protect.s2k.count = read_32(inp);
			pktlen -= 4;
		    }

		}
		else {
		    if( list_mode )
			printf(  "\tprotect algo: %d\n", cert->protect.algo);
		    /* old version, we don't have a S2K, so we fake one */
		    cert->protect.s2k.mode = 0;
		    cert->protect.s2k.hash_algo = DIGEST_ALGO_MD5;
		}
		if( pktlen < 8 ) {
		    rc = G10ERR_INVALID_PACKET;
		    goto leave;
		}
		for(i=0; i < 8 && pktlen; i++, pktlen-- )
		    temp[i] = iobuf_get_noeof(inp);
		if( list_mode ) {
		    printf(  "\tprotect IV: ");
		    for(i=0; i < 8; i++ )
			printf(" %02x", temp[i] );
		    putchar('\n');
		}
		memcpy(cert->protect.iv, temp, 8 );
	    }
	    else
		cert->is_protected = 0;
	    /* It does not make sense to read it into secure memory.
	     * If the user is so careless, not to protect his secret key,
	     * we can assume, that he operates an open system :=(.
	     * So we put the key into secure memory when we unprotect it. */
	    n = pktlen; cert->d.dsa.x = mpi_read(inp, &n, 0 ); pktlen -=n;

	    cert->csum = read_16(inp); pktlen -= 2;
	    if( list_mode ) {
		printf("\t[secret value x is not shown]\n"
		       "\tchecksum: %04hx\n", cert->csum);
	    }
	  /*log_mpidump("dsa p=", cert->d.dsa.p );
	    log_mpidump("dsa q=", cert->d.dsa.q );
	    log_mpidump("dsa g=", cert->d.dsa.g );
	    log_mpidump("dsa y=", cert->d.dsa.y );
	    log_mpidump("dsa x=", cert->d.dsa.x ); */
	}
    }
    else if( is_RSA(algorithm) ) {
	MPI rsa_pub_mod, rsa_pub_exp;

	n = pktlen; rsa_pub_mod = mpi_read(inp, &n, 0); pktlen -=n;
	n = pktlen; rsa_pub_exp = mpi_read(inp, &n, 0 ); pktlen -=n;
	if( list_mode ) {
	    printf(  "\tpublic modulus  n:  ");
	    mpi_print(stdout, rsa_pub_mod, mpi_print_mode  );
	    printf("\n\tpublic exponent e: ");
	    mpi_print(stdout, rsa_pub_exp, mpi_print_mode  );
	    putchar('\n');
	}
	if( pkttype == PKT_PUBLIC_CERT || pkttype == PKT_PUBKEY_SUBCERT ) {
	    pkt->pkt.public_cert->d.rsa.n = rsa_pub_mod;
	    pkt->pkt.public_cert->d.rsa.e = rsa_pub_exp;
	}
	else {
	    PKT_secret_cert *cert = pkt->pkt.secret_cert;
	    byte temp[8];

	    pkt->pkt.secret_cert->d.rsa.n = rsa_pub_mod;
	    pkt->pkt.secret_cert->d.rsa.e = rsa_pub_exp;
	    cert->protect.algo = iobuf_get_noeof(inp); pktlen--;
	    if( list_mode )
		printf(  "\tprotect algo: %d\n", cert->protect.algo);
	    if( cert->protect.algo ) {
		cert->is_protected = 1;
		for(i=0; i < 8 && pktlen; i++, pktlen-- )
		    temp[i] = iobuf_get_noeof(inp);
		if( list_mode ) {
		    printf(  "\tprotect IV: ");
		    for(i=0; i < 8; i++ )
			printf(" %02x", temp[i] );
		    putchar('\n');
		}
		if( cert->protect.algo == CIPHER_ALGO_BLOWFISH160 )
		    memcpy(cert->protect.iv, temp, 8 );
	    }
	    else
		cert->is_protected = 0;
	    /* (See comments at the code for elg keys) */
	    n = pktlen; cert->d.rsa.d = mpi_read(inp, &n, 0 ); pktlen -=n;
	    n = pktlen; cert->d.rsa.p = mpi_read(inp, &n, 0 ); pktlen -=n;
	    n = pktlen; cert->d.rsa.q = mpi_read(inp, &n, 0 ); pktlen -=n;
	    n = pktlen; cert->d.rsa.u = mpi_read(inp, &n, 0 ); pktlen -=n;

	    cert->csum = read_16(inp); pktlen -= 2;
	    if( list_mode ) {
		printf("\t[secret values d,p,q,u are not shown]\n"
		       "\tchecksum: %04hx\n", cert->csum);
	    }
	 /* log_mpidump("rsa n=", cert->d.rsa.n );
	    log_mpidump("rsa e=", cert->d.rsa.e );
	    log_mpidump("rsa d=", cert->d.rsa.d );
	    log_mpidump("rsa p=", cert->d.rsa.p );
	    log_mpidump("rsa q=", cert->d.rsa.q );
	    log_mpidump("rsa u=", cert->d.rsa.u ); */
	}
    }
    else if( list_mode )
	printf("\tunknown algorithm %d\n", algorithm );


  leave:
    skip_rest(inp, pktlen);
    return rc;
}


static int
parse_user_id( IOBUF inp, int pkttype, unsigned long pktlen, PACKET *packet )
{
    byte *p;

    packet->pkt.user_id = m_alloc(sizeof *packet->pkt.user_id  + pktlen - 1);
    packet->pkt.user_id->len = pktlen;
    p = packet->pkt.user_id->name;
    for( ; pktlen; pktlen--, p++ )
	*p = iobuf_get_noeof(inp);

    if( list_mode ) {
	int n = packet->pkt.user_id->len;
	printf(":user id packet: \"");
	for(p=packet->pkt.user_id->name; n; p++, n-- ) {
	    if( *p >= ' ' && *p <= 'z' )
		putchar(*p);
	    else
		printf("\\x%02x", *p );
	}
	printf("\"\n");
    }
    return 0;
}



static int
parse_comment( IOBUF inp, int pkttype, unsigned long pktlen, PACKET *packet )
{
    byte *p;

    packet->pkt.comment = m_alloc(sizeof *packet->pkt.comment + pktlen - 1);
    packet->pkt.comment->len = pktlen;
    p = packet->pkt.comment->data;
    for( ; pktlen; pktlen--, p++ )
	*p = iobuf_get_noeof(inp);

    if( list_mode ) {
	int n = packet->pkt.comment->len;
	printf(":%scomment packet: \"", pkttype == PKT_OLD_COMMENT?
					 "OpenPGP draft " : "" );
	for(p=packet->pkt.comment->data; n; p++, n-- ) {
	    if( *p >= ' ' && *p <= 'z' )
		putchar(*p);
	    else
		printf("\\x%02x", *p );
	}
	printf("\"\n");
    }
    return 0;
}


static void
parse_trust( IOBUF inp, int pkttype, unsigned long pktlen )
{
    int c;

    c = iobuf_get_noeof(inp);
    if( list_mode )
	printf(":trust packet: flag=%02x\n", c );
}


static int
parse_plaintext( IOBUF inp, int pkttype, unsigned long pktlen, PACKET *pkt )
{
    int mode, namelen;
    PKT_plaintext *pt;
    byte *p;
    int c, i;

    if( pktlen && pktlen < 6 ) {
	log_error("packet(%d) too short (%lu)\n", pkttype, (ulong)pktlen);
	goto leave;
    }
    mode = iobuf_get_noeof(inp); if( pktlen ) pktlen--;
    namelen = iobuf_get_noeof(inp); if( pktlen ) pktlen--;
    pt = pkt->pkt.plaintext = m_alloc(sizeof *pkt->pkt.plaintext + namelen -1);
    pt->mode = mode;
    pt->namelen = namelen;
    if( pktlen ) {
	for( i=0; pktlen > 4 && i < namelen; pktlen--, i++ )
	    pt->name[i] = iobuf_get_noeof(inp);
    }
    else {
	for( i=0; i < namelen; i++ )
	    if( (c=iobuf_get(inp)) == -1 )
		break;
	    else
		pt->name[i] = c;
    }
    pt->timestamp = read_32(inp); if( pktlen) pktlen -= 4;
    pt->len = pktlen;
    pt->buf = inp;
    pktlen = 0;

    if( list_mode ) {
	printf(":literal data packet:\n"
	       "\tmode %c, created %lu, name=\"",
		    mode >= ' ' && mode <'z'? mode : '?',
		    (ulong)pt->timestamp );
	for(p=pt->name,i=0; i < namelen; p++, i++ ) {
	    if( *p >= ' ' && *p <= 'z' )
		putchar(*p);
	    else
		printf("\\x%02x", *p );
	}
	printf("\",\n\traw data: %lu bytes\n", (ulong)pt->len );
    }

  leave:
    return 0;
}


static int
parse_compressed( IOBUF inp, int pkttype, unsigned long pktlen, PACKET *pkt )
{
    PKT_compressed *zd;

    /* pktlen is here 0, but data follows
     * (this should be the last object in a file or
     *	the compress algorithm should know the length)
     */
    zd = pkt->pkt.compressed =	m_alloc(sizeof *pkt->pkt.compressed );
    zd->len = 0; /* not yet used */
    zd->algorithm = iobuf_get_noeof(inp);
    zd->buf = inp;
    if( list_mode )
	printf(":compressed packet: algo=%d\n", zd->algorithm);
    return 0;
}


static int
parse_encrypted( IOBUF inp, int pkttype, unsigned long pktlen, PACKET *pkt )
{
    PKT_encrypted *ed;

    ed = pkt->pkt.encrypted =  m_alloc(sizeof *pkt->pkt.encrypted );
    ed->len = pktlen;
    ed->buf = NULL;
    if( pktlen && pktlen < 10 ) {
	log_error("packet(%d) too short\n", pkttype);
	skip_rest(inp, pktlen);
	goto leave;
    }
    if( list_mode ) {
	if( pktlen )
	    printf(":encrypted data packet:\n\tlength: %lu\n", pktlen-10);
	else
	    printf(":encrypted data packet:\n\tlength: unknown\n");
    }

    ed->buf = inp;
    pktlen = 0;

  leave:
    return 0;
}



