/*
Copyright (c) 1998-2001, Robert O'Callahan
Copyright (c) 2004-2005, Yutaka Hirata
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of
conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright notice, this list
of conditions and the following disclaimer in the documentation and/or other materials
provided with the distribution.

The name of Robert O'Callahan may not be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "ttxssh.h"
#include "util.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/dh.h>
#include <openssl/engine.h>
#include <openssl/rsa.h>
#include <openssl/dsa.h>
#include <limits.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <process.h>
#include <time.h>
#include "buffer.h"
#include "ssh.h"
#include "crypt.h"

// SSH2 macro
#ifdef _DEBUG
#define SSH2_DEBUG
#endif

#define DONT_WANTCONFIRM 1  // (2005.3.28 yutaka)
#define INTBLOB_LEN 20
#define SIGBLOB_LEN (2*INTBLOB_LEN)

static char ssh_ttymodes[] = "\x01\x03\x02\x1c\x03\x08\x04\x15\x05\x04";

static void try_send_credentials(PTInstVar pvar);
static void prep_compression(PTInstVar pvar);

// 関数プロトタイプ宣言
void SSH2_send_kexinit(PTInstVar pvar);
static BOOL handle_SSH2_kexinit(PTInstVar pvar);
static void SSH2_dh_kex_init(PTInstVar pvar);
static void SSH2_dh_gex_kex_init(PTInstVar pvar);
static BOOL handle_SSH2_dh_common_reply(PTInstVar pvar);
static BOOL handle_SSH2_dh_gex_reply(PTInstVar pvar);
static BOOL handle_SSH2_newkeys(PTInstVar pvar);
static BOOL handle_SSH2_authrequest(PTInstVar pvar);
static BOOL handle_SSH2_userauth_success(PTInstVar pvar);
static BOOL handle_SSH2_userauth_failure(PTInstVar pvar);
static BOOL handle_SSH2_userauth_banner(PTInstVar pvar);
static BOOL handle_SSH2_open_confirm(PTInstVar pvar);
static BOOL handle_SSH2_request_success(PTInstVar pvar);
static BOOL handle_SSH2_channel_success(PTInstVar pvar);
static BOOL handle_SSH2_channel_data(PTInstVar pvar);
static BOOL handle_SSH2_channel_extended_data(PTInstVar pvar);
static BOOL handle_SSH2_channel_eof(PTInstVar pvar);
static BOOL handle_SSH2_channel_close(PTInstVar pvar);
static BOOL handle_SSH2_window_adjust(PTInstVar pvar);
static BOOL handle_SSH2_channel_request(PTInstVar pvar);
void SSH2_dispatch_init(int stage);
int SSH2_dispatch_enabled_check(unsigned char message);
void SSH2_dispatch_add_message(unsigned char message);
void SSH2_dispatch_add_range_message(unsigned char begin, unsigned char end);
int dh_pub_is_valid(DH *dh, BIGNUM *dh_pub);
static void start_ssh_heartbeat_thread(PTInstVar pvar);


//
// SSH2 data structure
//

/* default window/packet sizes for tcp/x11-fwd-channel */
#define CHAN_SES_PACKET_DEFAULT (32*1024)
#define CHAN_SES_WINDOW_DEFAULT (2*CHAN_SES_PACKET_DEFAULT) // READAMOUNT @ pkt.cと同期を取ること

// channel data structure
#define CHANNEL_MAX 100

enum channel_type {
	TYPE_SHELL, TYPE_PORTFWD, 
};

typedef struct channel {
	int used;
	int self_id;
	int remote_id;
	unsigned int local_window;
	unsigned int local_window_max;
	unsigned int local_consumed;
	unsigned int local_maxpacket;
	unsigned int remote_window;
	unsigned int remote_maxpacket;
	enum channel_type type;
	int local_num;
} Channel_t;

static Channel_t channels[CHANNEL_MAX];

//
// channel function
//
static Channel_t *ssh2_channel_new(unsigned int window, unsigned int maxpack, enum confirm_type type, int local_num)
{
	int i, found;
	Channel_t *c;

	found = -1;
	for (i = 0 ; i < CHANNEL_MAX ; i++) {
		if (channels[i].used == 0) { // free channel
			found = i;
			break;
		}
	}
	if (found == -1) { // not free channel
		return (NULL);
	}

	// setup
	c = &channels[found];
	c->used = 1;
	c->self_id = i;
	c->remote_id = -1;
	c->local_window = window;
	c->local_window_max = window;
	c->local_consumed = 0;
	c->local_maxpacket = maxpack;
	c->remote_window = 0;
	c->remote_maxpacket = 0;
	c->type = type;
	c->local_num = local_num;

	return (c);
}


// connection close時に呼ばれる
void ssh2_channel_free(void)
{
	int i;
	Channel_t *c;

	for (i = 0 ; i < CHANNEL_MAX ; i++) {
		c = &channels[i];
		memset(c, 0, sizeof(Channel_t));
	}

}

static Channel_t *ssh2_channel_lookup(int id)
{
	Channel_t *c;

	if (id < 0 || id >= CHANNEL_MAX) {
		return (NULL);
	}
	c = &channels[id];
	if (c->used == 0) { // already freed
		return (NULL);
	}
	return (c);
}

// SSH1で管理しているchannel構造体から、SSH2向けのChannel_tへ変換する。
// TODO: 将来的にはチャネル構造体は1つに統合する。
// (2005.6.12 yutaka)
static Channel_t *ssh2_local_channel_lookup(int local_num)
{
	int i;
	Channel_t *c;

	for (i = 0 ; i < CHANNEL_MAX ; i++) {
		c = &channels[i];
		if (c->local_num == local_num)
			return (c);
	}
	return (NULL);
}


//
// SSH heartbeat mutex
//
static CRITICAL_SECTION g_ssh_heartbeat_lock;   /* ロック用変数 */

void ssh_heartbeat_lock_initialize(void)
{
	InitializeCriticalSection(&g_ssh_heartbeat_lock);
}

void ssh_heartbeat_lock_finalize(void)
{
	DeleteCriticalSection(&g_ssh_heartbeat_lock);
}

void ssh_heartbeat_lock(void)
{
	EnterCriticalSection(&g_ssh_heartbeat_lock);
}

void ssh_heartbeat_unlock(void)
{
	LeaveCriticalSection(&g_ssh_heartbeat_lock);
}


//
// SSH memory dump (for debug)
//
// (2005.3.7 yutaka)
//
#define MEMTAG_MAX 100
#define LOGDUMP "ssh2dump.log"
#define SENDTOME "Please send '"LOGDUMP"' file to TeraTerm developer team."

typedef struct memtag {
	char *name;
	char *desc;
	int len;
	char *data;
} memtag_t;

static memtag_t memtags[MEMTAG_MAX];
static int memtag_count = 0;
static int memtag_use = 0; 

/* ダンプラインをフォーマット表示する */
static void displine_memdump(FILE *fp, int addr, int *bytes, int byte_cnt)
{
	int i, c;

	/* 先頭のアドレス表示 */
	fprintf(fp, "%08X : ", addr);

	/* バイナリ表示（4バイトごとに空白を挿入）*/
	for (i = 0 ; i < byte_cnt ; i++) {
		if (i > 0 && i % 4 == 0)
			fprintf(fp, " ");

		fprintf(fp, "%02X", bytes[i]);
	}

	/* ASCII表示部分までの空白を補う */
	fprintf(fp, "   %*s%*s", (16-byte_cnt)*2+1, " ", (16-byte_cnt+3)/4, " ");

	/* ASCII表示 */
	for (i = 0 ; i < byte_cnt ; i++) {
		c = bytes[i];
		if (c >= 0x20 && c <= 0x7f) {
			fprintf(fp, "%c", c);
		} else {
			fprintf(fp, ".");
		}
	}

	fprintf(fp, "\n");
}


/* ダンプルーチン */
static void dump_memdump(FILE *fp, char *data, int len)
{
	int c, addr;
	int bytes[16], *ptr;
	int byte_cnt;
	int i;

	addr = 0;
	byte_cnt = 0;
	ptr = bytes;
	for (i = 0 ; i < len ; i++) {
		c = data[i];
		*ptr++ = c & 0xff;
		byte_cnt++;

		if (byte_cnt == 16) {
			displine_memdump(fp, addr, bytes, byte_cnt);

			addr += 16;
			byte_cnt = 0;
			ptr = bytes;
		}
	}

	if (byte_cnt > 0) {
		displine_memdump(fp, addr, bytes, byte_cnt);
	}
}

void init_memdump(void)
{
	int i;

	for (i = 0 ; i < MEMTAG_MAX ; i++) {
		memtags[i].name = NULL;
		memtags[i].desc = NULL;
		memtags[i].data = NULL;
		memtags[i].len = 0;
	}
	memtag_use++;
}

void finish_memdump(void)
{
	int i;

	// initializeされてないときは何もせずに戻る。(2005.4.3 yutaka)
	if (memtag_use <= 0)
		return;
	memtag_use--;

	for (i = 0 ; i < MEMTAG_MAX ; i++) {
		free(memtags[i].name);
		free(memtags[i].desc);
		free(memtags[i].data);
		memtags[i].len = 0;
	}
}

void save_memdump(char *filename)
{
	FILE *fp;
	int i;
	time_t t;
	struct tm *tm;

	fp = fopen(filename, "w");
	if (fp == NULL)
		return;

	t = time(NULL);
	tm = localtime(&t);

	fprintf(fp, "<<< TeraTerm SSH2 log dump >>>\n");
	fprintf(fp, "saved time: %04d/%02d/%02d %02d:%02d:%02d\n",
			tm->tm_year + 1900,
			tm->tm_mon + 1,
			tm->tm_mday,
			tm->tm_hour,
			tm->tm_min,
			tm->tm_sec);
	fprintf(fp, "\n");

	for (i = 0 ; i < memtag_count ; i++) {
		fprintf(fp, "============================================\n");
		fprintf(fp, "name: %s\n", memtags[i].name);
		fprintf(fp, "--------------------------------------------\n");
		fprintf(fp, "description: %s\n", memtags[i].desc);
		fprintf(fp, "============================================\n");
		dump_memdump(fp, memtags[i].data, memtags[i].len);
		fprintf(fp, "\n\n\n");
	}

	fprintf(fp, "[EOF]\n");

	fclose(fp);
}

void push_memdump(char *name, char *desc, char *data, int len)
{
	memtag_t *ptr;
	char *dp;

	dp = malloc(len);
	if (dp == NULL)
		return;
	memcpy(dp, data, len);

	if (memtag_count >= MEMTAG_MAX)
		return;

	ptr = &memtags[memtag_count];
	memtag_count++;
	ptr->name = strdup(name);
	ptr->desc = strdup(desc);
	ptr->data = dp;
	ptr->len = len;
}

void push_bignum_memdump(char *name, char *desc, BIGNUM *bignum)
{
	int len;
	char *buf;

	len = BN_num_bytes(bignum);
	buf = malloc(len); // allocate
	if (buf == NULL)
		return;
	BN_bn2bin(bignum, buf);
	push_memdump(name, desc, buf, len); // at push_bignum_memdump()
	free(buf); // free
}


//
//
//


static int get_predecryption_amount(PTInstVar pvar)
{
	static int small_block_decryption_sizes[] = { 5, 5, 6, 6, 8 };

	if (SSHv1(pvar)) {
		return 0;
	} else {
		int block_size = CRYPT_get_decryption_block_size(pvar);

		if (block_size < 5) {
			return small_block_decryption_sizes[block_size];
		} else {
			return block_size;
		}
	}
}

/* Get up to 'limit' bytes into the payload buffer.
   'limit' is counted from the start of the payload data.
   Returns the amount of data in the payload buffer, or
   -1 if there is an error.
   We can return more than limit in some cases. */
static int buffer_packet_data(PTInstVar pvar, int limit)
{
	if (pvar->ssh_state.payloadlen >= 0) {
		return pvar->ssh_state.payloadlen;
	} else {
		int cur_decompressed_bytes =
			pvar->ssh_state.decompress_stream.next_out -
			pvar->ssh_state.postdecompress_inbuf;

		while (limit > cur_decompressed_bytes) {
			int result;

			pvar->ssh_state.payload =
				pvar->ssh_state.postdecompress_inbuf + 1;
			if (pvar->ssh_state.postdecompress_inbuflen ==
				cur_decompressed_bytes) {
				buf_ensure_size(&pvar->ssh_state.postdecompress_inbuf,
								&pvar->ssh_state.postdecompress_inbuflen,
								min(limit, cur_decompressed_bytes * 2));
			}

			pvar->ssh_state.decompress_stream.next_out
				=
				pvar->ssh_state.postdecompress_inbuf +
				cur_decompressed_bytes;
			pvar->ssh_state.decompress_stream.avail_out =
				min(limit, pvar->ssh_state.postdecompress_inbuflen)
				- cur_decompressed_bytes;

			result =
				inflate(&pvar->ssh_state.decompress_stream, Z_SYNC_FLUSH);
			cur_decompressed_bytes =
				pvar->ssh_state.decompress_stream.next_out -
				pvar->ssh_state.postdecompress_inbuf;

			switch (result) {
			case Z_OK:
				break;
			case Z_BUF_ERROR:
				pvar->ssh_state.payloadlen = cur_decompressed_bytes;
				return cur_decompressed_bytes;
			default:
				notify_fatal_error(pvar,
								   "Invalid compressed data in received packet");
				return -1;
			}
		}

		return cur_decompressed_bytes;
	}
}

/* For use by the protocol processing code.
   Gets N bytes of uncompressed payload. Returns FALSE if data not available
   and a fatal error has been signaled.
   The data is available in the payload buffer. This buffer address
   can change during a call to grab_payload, so take care!
   The payload pointer is set to point to the first byte of the actual data
   (after the packet type byte).
*/
static BOOL grab_payload(PTInstVar pvar, int num_bytes)
{
	/* Accept maximum of 4MB of payload data */
	int in_buffer = buffer_packet_data(pvar, 4 * 1024 * 1024);

	if (in_buffer < 0) {
		return FALSE;
	} else {
		pvar->ssh_state.payload_grabbed += num_bytes;
		if (pvar->ssh_state.payload_grabbed > in_buffer) {
			notify_fatal_error(pvar, "Received truncated packet");
			return FALSE;
		} else {
			return TRUE;
		}
	}
}

static BOOL grab_payload_limited(PTInstVar pvar, int num_bytes)
{
	int in_buffer;

	pvar->ssh_state.payload_grabbed += num_bytes;
	in_buffer = buffer_packet_data(pvar, pvar->ssh_state.payload_grabbed);

	if (in_buffer < 0) {
		return FALSE;
	} else {
		if (pvar->ssh_state.payload_grabbed > in_buffer) {
			notify_fatal_error(pvar, "Received truncated packet");
			return FALSE;
		} else {
			return TRUE;
		}
	}
}

#define get_payload_uint32(pvar, offset) get_uint32_MSBfirst((pvar)->ssh_state.payload + (offset))
#define get_uint32(buf) get_uint32_MSBfirst((buf))
#define set_uint32(buf, v) set_uint32_MSBfirst((buf), (v))
#define get_mpint_len(pvar, offset) ((get_ushort16_MSBfirst((pvar)->ssh_state.payload + (offset)) + 7) >> 3)
#define get_ushort16(buf) get_ushort16_MSBfirst((buf))

#define do_crc(buf, len) (~(uint32)crc32(0xFFFFFFFF, (buf), (len)))

/* Decrypt the payload, checksum it, eat the padding, get the packet type
   and return it.
   'data' points to the start of the packet --- its length field.
   'len' is the length of the
   payload + padding (+ length of CRC for SSHv1). 'padding' is the length
   of the padding alone. */
static int prep_packet(PTInstVar pvar, char FAR * data, int len,
					   int padding)
{
	pvar->ssh_state.payload = data + 4;
	pvar->ssh_state.payloadlen = len;

	if (SSHv1(pvar)) {
		if (CRYPT_detect_attack(pvar, pvar->ssh_state.payload, len)) {
			notify_fatal_error(pvar,
							   "'CORE insertion attack' detected.  Aborting connection.");
			return SSH_MSG_NONE;
		}

		CRYPT_decrypt(pvar, pvar->ssh_state.payload, len);
		/* PKT guarantees that the data is always 4-byte aligned */
		if (do_crc(pvar->ssh_state.payload, len - 4) !=
			get_uint32_MSBfirst(pvar->ssh_state.payload + len - 4)) {
			notify_fatal_error(pvar,
							   "Detected corrupted data; connection terminating.");
			return SSH_MSG_NONE;
		}

		pvar->ssh_state.payload += padding;
		pvar->ssh_state.payloadlen -= padding + 4;
	} else {
		int already_decrypted = get_predecryption_amount(pvar);

#if 0
		CRYPT_decrypt(pvar, data + already_decrypted,
					  len - already_decrypted);
#else
		CRYPT_decrypt(pvar, data + already_decrypted,
					  (4 + len) - already_decrypted);
#endif

		if (!CRYPT_verify_receiver_MAC
			(pvar, pvar->ssh_state.receiver_sequence_number, data, len + 4,
			 data + len + 4)) {
			notify_fatal_error(pvar,
							   "Detected corrupted data; connection terminating.");
			return SSH_MSG_NONE;
		}

		pvar->ssh_state.payload++;
		pvar->ssh_state.payloadlen -= padding + 1;
	}

	pvar->ssh_state.payload_grabbed = 0;

	if (pvar->ssh_state.decompressing) {
		if (pvar->ssh_state.decompress_stream.avail_in != 0) {
			notify_nonfatal_error(pvar,
								  "Internal error: a packet was not fully decompressed.\n"
								  "This is a bug, please report it.");
		}

		pvar->ssh_state.decompress_stream.next_in =
			pvar->ssh_state.payload;
		pvar->ssh_state.decompress_stream.avail_in =
			pvar->ssh_state.payloadlen;
		pvar->ssh_state.decompress_stream.next_out =
			pvar->ssh_state.postdecompress_inbuf;
		pvar->ssh_state.payloadlen = -1;
	} else {
		pvar->ssh_state.payload++;
	}

	if (!grab_payload_limited(pvar, 1)) {
		return SSH_MSG_NONE;
	}

	pvar->ssh_state.receiver_sequence_number++;

	return pvar->ssh_state.payload[-1];
}

/* Create a packet to be sent. The SSH protocol packet type is in 'type';
   'len' contains the length of the packet payload, in bytes (this
   does not include the space for any of the packet headers or padding,
   or for the packet type byte).
   Returns a pointer to the payload data area, a region of length 'len',
   to be filled by the caller. */
static unsigned char FAR *begin_send_packet(PTInstVar pvar, int type,
											int len)
{
	unsigned char FAR *buf;

	pvar->ssh_state.outgoing_packet_len = len + 1;

	if (pvar->ssh_state.compressing) {
		buf_ensure_size(&pvar->ssh_state.precompress_outbuf,
						&pvar->ssh_state.precompress_outbuflen, 1 + len);
		buf = pvar->ssh_state.precompress_outbuf;
	} else {
		/* For SSHv2,
		   Encrypted_length is 4(packetlength) + 1(paddinglength) + 1(packettype)
		   + len(payload) + 4(minpadding), rounded up to nearest block_size
		   We only need a reasonable upper bound for the buffer size */
		buf_ensure_size(&pvar->ssh_state.outbuf,
						&pvar->ssh_state.outbuflen,
						len + 30 + CRYPT_get_sender_MAC_size(pvar) +
						CRYPT_get_encryption_block_size(pvar));
		buf = pvar->ssh_state.outbuf + 12;
	}

	buf[0] = (unsigned char) type;
	return buf + 1;
}

#define finish_send_packet(pvar) finish_send_packet_special((pvar), 0)

static BOOL send_packet_blocking(PTInstVar pvar, char FAR * data, int len)
{
	u_long do_block = 0;

	if ((pvar->PWSAAsyncSelect) (pvar->socket, pvar->NotificationWindow,
								 0, 0) == SOCKET_ERROR
		|| ioctlsocket(pvar->socket, FIONBIO, &do_block) == SOCKET_ERROR
		|| (pvar->Psend) (pvar->socket, data, len, 0) != len
		|| (pvar->PWSAAsyncSelect) (pvar->socket, pvar->NotificationWindow,
									pvar->notification_msg,
									pvar->notification_events) ==
		SOCKET_ERROR) {
		notify_fatal_error(pvar,
						   "A communications error occurred while sending an SSH packet.\n"
						   "The connection will close.");
		return FALSE;
	} else {
		return TRUE;
	}
}

/* if skip_compress is true, then the data has already been compressed
   into outbuf + 12 */
static void finish_send_packet_special(PTInstVar pvar, int skip_compress)
{
	int len = pvar->ssh_state.outgoing_packet_len;
	char FAR *data;
	int data_length;

	if (pvar->ssh_state.compressing) {
		if (!skip_compress) {
			buf_ensure_size(&pvar->ssh_state.outbuf,
							&pvar->ssh_state.outbuflen,
							len + (len >> 6) + 50 +
							CRYPT_get_sender_MAC_size(pvar));
			pvar->ssh_state.compress_stream.next_in =
				pvar->ssh_state.precompress_outbuf;
			pvar->ssh_state.compress_stream.avail_in = len;
			pvar->ssh_state.compress_stream.next_out =
				pvar->ssh_state.outbuf + 12;
			pvar->ssh_state.compress_stream.avail_out =
				pvar->ssh_state.outbuflen - 12;

			if (deflate(&pvar->ssh_state.compress_stream, Z_SYNC_FLUSH) !=
				Z_OK) {
				notify_fatal_error(pvar,
								   "An error occurred while compressing packet data.\n"
								   "The connection will close.");
				return;
			}
		}

		len =
			pvar->ssh_state.outbuflen - 12 -
			pvar->ssh_state.compress_stream.avail_out;
	}

	if (SSHv1(pvar)) {
		int padding = 8 - ((len + 4) % 8);

		data = pvar->ssh_state.outbuf + 8 - padding;
		data_length = padding + len + 8;

		set_uint32(data, len + 4);
		if (CRYPT_get_receiver_cipher(pvar) != SSH_CIPHER_NONE) {
			CRYPT_set_random_data(pvar, data + 4, padding);
		} else {
			memset(data + 4, 0, padding);
		}
		set_uint32(data + data_length - 4,
				   do_crc(data + 4, data_length - 8));
		CRYPT_encrypt(pvar, data + 4, data_length - 4);
	} else { //for SSH2(yutaka)
		int block_size = CRYPT_get_encryption_block_size(pvar);
		int encryption_size;
		int padding;
		BOOL ret;

		if (block_size < 8) {
			block_size = 8;
		}
		encryption_size = ((len + 8) / block_size + 1) * block_size;
		data = pvar->ssh_state.outbuf + 7;
		data_length = encryption_size + CRYPT_get_sender_MAC_size(pvar);

		set_uint32(data, encryption_size - 4);
		padding = encryption_size - len - 5;
		data[4] = (unsigned char) padding;
		CRYPT_set_random_data(pvar, data + 5 + len, padding);
		ret = CRYPT_build_sender_MAC(pvar,
							   pvar->ssh_state.sender_sequence_number,
							   data, encryption_size,
							   data + encryption_size);
		if (ret == FALSE) { // HMACがまだ設定されていない場合
			data_length = encryption_size;
		}

		// パケットを暗号化する。HMAC以降は暗号化対象外。
		CRYPT_encrypt(pvar, data, encryption_size);
	}

	send_packet_blocking(pvar, data, data_length);

	pvar->ssh_state.sender_sequence_number++;

	// 送信時刻を記録 
	pvar->ssh_heartbeat_tick = time(NULL);
}

static void destroy_packet_buf(PTInstVar pvar)
{
	memset(pvar->ssh_state.outbuf, 0, pvar->ssh_state.outbuflen);
	if (pvar->ssh_state.compressing) {
		memset(pvar->ssh_state.precompress_outbuf, 0,
			   pvar->ssh_state.precompress_outbuflen);
	}
}

/* The handlers are added to the queue for each message. When one of the
   handlers fires, if it returns FALSE, then all handlers in the set are
   removed from their queues. */
static void enque_handlers(PTInstVar pvar, int num_msgs,
						   const int FAR * messages,
						   const SSHPacketHandler FAR * handlers)
{
	SSHPacketHandlerItem FAR *first_item;
	SSHPacketHandlerItem FAR *last_item = NULL;
	int i;

	for (i = 0; i < num_msgs; i++) {
		SSHPacketHandlerItem FAR *item =
			(SSHPacketHandlerItem FAR *)
			malloc(sizeof(SSHPacketHandlerItem));
		SSHPacketHandlerItem FAR *cur_item =
			pvar->ssh_state.packet_handlers[messages[i]];

		item->handler = handlers[i];

		if (cur_item == NULL) {
			pvar->ssh_state.packet_handlers[messages[i]] = item;
			item->next_for_message = item;
			item->last_for_message = item;
			item->active_for_message = messages[i];
		} else {
			item->next_for_message = cur_item;
			item->last_for_message = cur_item->last_for_message;
			cur_item->last_for_message->next_for_message = item;
			cur_item->last_for_message = item;
			item->active_for_message = -1;
		}

		if (last_item != NULL) {
			last_item->next_in_set = item;
		} else {
			first_item = item;
		}
		last_item = item;
	}

	if (last_item != NULL) {
		last_item->next_in_set = first_item;
	}
}

static SSHPacketHandler get_handler(PTInstVar pvar, int message)
{
	SSHPacketHandlerItem FAR *cur_item =
		pvar->ssh_state.packet_handlers[message];

	if (cur_item == NULL) {
		return NULL;
	} else {
		return cur_item->handler;
	}
}

/* Called only by SSH_handle_packet */
static void deque_handlers(PTInstVar pvar, int message)
{
	SSHPacketHandlerItem FAR *cur_item =
		pvar->ssh_state.packet_handlers[message];
	SSHPacketHandlerItem FAR *first_item_in_set = cur_item;

	do {
		SSHPacketHandlerItem FAR *next_in_set = cur_item->next_in_set;

		if (cur_item->active_for_message >= 0) {
			SSHPacketHandlerItem FAR *replacement =
				cur_item->next_for_message;

			if (replacement == cur_item) {
				replacement = NULL;
			} else {
				replacement->active_for_message =
					cur_item->active_for_message;
			}
			pvar->ssh_state.packet_handlers[cur_item->active_for_message] =
				replacement;
		}
		cur_item->next_for_message->last_for_message =
			cur_item->last_for_message;
		cur_item->last_for_message->next_for_message =
			cur_item->next_for_message;

		free(cur_item);
		cur_item = next_in_set;
	} while (cur_item != first_item_in_set);
}

static void enque_handler(PTInstVar pvar, int message,
						  SSHPacketHandler handler)
{
	enque_handlers(pvar, 1, &message, &handler);
}

static void chop_newlines(char FAR * buf)
{
	int len = strlen(buf);

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
		buf[len - 1] = 0;
		len--;
	}
}

/********************/
/* Message handlers */
/********************/

static BOOL handle_forwarding_success(PTInstVar pvar)
{
	return FALSE;
}

static BOOL handle_forwarding_failure(PTInstVar pvar)
{
	return FALSE;
}

static void enque_forwarding_request_handlers(PTInstVar pvar)
{
	static const int msgs[] = { SSH_SMSG_SUCCESS, SSH_SMSG_FAILURE };
	static const SSHPacketHandler handlers[]
	= { handle_forwarding_success, handle_forwarding_failure };

	enque_handlers(pvar, 2, msgs, handlers);
}

static BOOL handle_auth_failure(PTInstVar pvar)
{
	notify_verbose_message(pvar, "Authentication failed",
						   LOG_LEVEL_VERBOSE);

	AUTH_set_generic_mode(pvar);
	AUTH_advance_to_next_cred(pvar);
	pvar->ssh_state.status_flags &= ~STATUS_DONT_SEND_CREDENTIALS;
	try_send_credentials(pvar);
	return FALSE;
}

static BOOL handle_rsa_auth_refused(PTInstVar pvar)
{
	AUTH_destroy_cur_cred(pvar);
	return handle_auth_failure(pvar);
}

static BOOL handle_TIS_challenge(PTInstVar pvar)
{
	if (grab_payload(pvar, 4)) {
		int len = get_payload_uint32(pvar, 0);

		if (grab_payload(pvar, len)) {
			notify_verbose_message(pvar, "Received TIS challenge",
								   LOG_LEVEL_VERBOSE);

			AUTH_set_TIS_mode(pvar, pvar->ssh_state.payload + 4, len);
			AUTH_advance_to_next_cred(pvar);
			pvar->ssh_state.status_flags &= ~STATUS_DONT_SEND_CREDENTIALS;
			try_send_credentials(pvar);
		}
	}
	return FALSE;
}

static BOOL handle_auth_required(PTInstVar pvar)
{
	notify_verbose_message(pvar, "Server requires authentication",
						   LOG_LEVEL_VERBOSE);

	pvar->ssh_state.status_flags &= ~STATUS_DONT_SEND_CREDENTIALS;
	try_send_credentials(pvar);
	/* the first AUTH_advance_to_next_cred is issued early by ttxssh.c */

	return FALSE;
}

static BOOL handle_ignore(PTInstVar pvar)
{
	if (grab_payload(pvar, 4)
		&& grab_payload(pvar, get_payload_uint32(pvar, 0))) {
		/* ignore it! but it must be decompressed */
	}
	return TRUE;
}

static BOOL handle_debug(PTInstVar pvar)
{
	BOOL always_display;
	char FAR *description;
	int description_len;
	char buf[2048];

	if (SSHv1(pvar)) {
		if (grab_payload(pvar, 4)
			&& grab_payload(pvar, description_len =
							get_payload_uint32(pvar, 0))) {
			always_display = FALSE;
			description = pvar->ssh_state.payload + 4;
			description[description_len] = 0;
		} else {
			return TRUE;
		}
	} else {
		if (grab_payload(pvar, 5)
			&& grab_payload(pvar,
							(description_len =
							 get_payload_uint32(pvar, 1)) + 4)
			&& grab_payload(pvar,
							get_payload_uint32(pvar,
											   5 + description_len))) {
			always_display = pvar->ssh_state.payload[0] != 0;
			description = pvar->ssh_state.payload + 5;
			description[description_len] = 0;
		} else {
			return TRUE;
		}
	}

	chop_newlines(description);
	_snprintf(buf, sizeof(buf), "DEBUG message from server: %s",
			  description);
	buf[sizeof(buf) - 1] = 0;
	if (always_display) {
		notify_nonfatal_error(pvar, buf);
	} else {
		notify_verbose_message(pvar, buf, LOG_LEVEL_VERBOSE);
	}
	return TRUE;
}

static BOOL handle_disconnect(PTInstVar pvar)
{
	int reason_code;
	char FAR *description;
	int description_len;
	char buf[2048];
	char FAR *explanation = "";

	if (SSHv1(pvar)) {
		if (grab_payload(pvar, 4)
			&& grab_payload(pvar, description_len =
							get_payload_uint32(pvar, 0))) {
			reason_code = -1;
			description = pvar->ssh_state.payload + 4;
			description[description_len] = 0;
		} else {
			return TRUE;
		}
	} else {
		if (grab_payload(pvar, 8)
			&& grab_payload(pvar,
							(description_len =
							 get_payload_uint32(pvar, 4)) + 4)
			&& grab_payload(pvar,
							get_payload_uint32(pvar,
											   8 + description_len))) {
			reason_code = get_payload_uint32(pvar, 0);
			description = pvar->ssh_state.payload + 8;
			description[description_len] = 0;
		} else {
			return TRUE;
		}
	}

	chop_newlines(description);
	if (description[0] == 0) {
		description = NULL;
	}

	if (get_handler(pvar, SSH_SMSG_FAILURE) == handle_forwarding_failure) {
		explanation =
			"\nIt may have disconnected because it was unable to forward a port you requested to be forwarded from the server.\n"
			"This often happens when someone is already forwarding that port from the server.";
	}

	if (description != NULL) {
		_snprintf(buf, sizeof(buf),
				  "Server disconnected with message '%s'.%s", description,
				  explanation);
	} else {
		_snprintf(buf, sizeof(buf),
				  "Server disconnected (no reason given).%s", explanation);
	}
	buf[sizeof(buf) - 1] = 0;
	notify_fatal_error(pvar, buf);

	return TRUE;
}

static BOOL handle_unimplemented(PTInstVar pvar)
{
	/* Should never receive this since we only send base 2.0 protocol messages */
	grab_payload(pvar, 4);
	return TRUE;
}

static BOOL handle_crypt_success(PTInstVar pvar)
{
	notify_verbose_message(pvar, "Secure mode successfully achieved",
						   LOG_LEVEL_VERBOSE);
	return FALSE;
}

static BOOL handle_noauth_success(PTInstVar pvar)
{
	notify_verbose_message(pvar, "Server does not require authentication",
						   LOG_LEVEL_VERBOSE);
	prep_compression(pvar);
	return FALSE;
}

static BOOL handle_auth_success(PTInstVar pvar)
{
	notify_verbose_message(pvar, "Authentication accepted",
						   LOG_LEVEL_VERBOSE);
	prep_compression(pvar);

	// ハートビート・スレッドの開始 (2004.12.11 yutaka)
	start_ssh_heartbeat_thread(pvar);

	return FALSE;
}

static BOOL handle_server_public_key(PTInstVar pvar)
{
	int server_key_public_exponent_len;
	int server_key_public_modulus_pos;
	int server_key_public_modulus_len;
	int host_key_bits_pos;
	int host_key_public_exponent_len;
	int host_key_public_modulus_pos;
	int host_key_public_modulus_len;
	int protocol_flags_pos;
	int supported_ciphers;
	char FAR *inmsg;

	if (!grab_payload(pvar, 14))
		return FALSE;
	server_key_public_exponent_len = get_mpint_len(pvar, 12);

	if (!grab_payload(pvar, server_key_public_exponent_len + 2))
		return FALSE;
	server_key_public_modulus_pos = 14 + server_key_public_exponent_len;
	server_key_public_modulus_len =
		get_mpint_len(pvar, server_key_public_modulus_pos);

	if (!grab_payload(pvar, server_key_public_modulus_len + 6))
		return FALSE;
	host_key_bits_pos =
		server_key_public_modulus_pos + 2 + server_key_public_modulus_len;
	host_key_public_exponent_len =
		get_mpint_len(pvar, host_key_bits_pos + 4);

	if (!grab_payload(pvar, host_key_public_exponent_len + 2))
		return FALSE;
	host_key_public_modulus_pos =
		host_key_bits_pos + 6 + host_key_public_exponent_len;
	host_key_public_modulus_len =
		get_mpint_len(pvar, host_key_public_modulus_pos);

	if (!grab_payload(pvar, host_key_public_modulus_len + 12))
		return FALSE;
	protocol_flags_pos =
		host_key_public_modulus_pos + 2 + host_key_public_modulus_len;

	inmsg = pvar->ssh_state.payload;

	CRYPT_set_server_cookie(pvar, inmsg);
	if (!CRYPT_set_server_RSA_key
		(pvar, get_uint32(inmsg + 8), pvar->ssh_state.payload + 12,
		 inmsg + server_key_public_modulus_pos))
		return FALSE;
	if (!CRYPT_set_host_RSA_key
		(pvar, get_uint32(inmsg + host_key_bits_pos),
		 inmsg + host_key_bits_pos + 4,
		 inmsg + host_key_public_modulus_pos))
		return FALSE;
	pvar->ssh_state.server_protocol_flags =
		get_uint32(inmsg + protocol_flags_pos);

	supported_ciphers = get_uint32(inmsg + protocol_flags_pos + 4);
	if (!CRYPT_set_supported_ciphers
		(pvar, supported_ciphers, supported_ciphers))
		return FALSE;
	if (!AUTH_set_supported_auth_types
		(pvar, get_uint32(inmsg + protocol_flags_pos + 8)))
		return FALSE;

	/* this must be the LAST THING in this function, since it can cause
	   host_is_OK to be called. */
	HOSTS_check_host_key(pvar, pvar->ssh_state.hostname,
						 get_uint32(inmsg + host_key_bits_pos),
						 inmsg + host_key_bits_pos + 4,
						 inmsg + host_key_public_modulus_pos);

	return FALSE;
}

/*
The ID must have already been found to start with "SSH-". It must
be null-terminated.
*/
static BOOL parse_protocol_ID(PTInstVar pvar, char FAR * ID)
{
	char FAR *str;

	for (str = ID + 4; *str >= '0' && *str <= '9'; str++) {
	}

	if (*str != '.') {
		return FALSE;
	}

	pvar->protocol_major = atoi(ID + 4);
	pvar->protocol_minor = atoi(str + 1);

	// for SSH2(yutaka)
	// 1.99ならSSH2での接続を行う
	if (pvar->protocol_major == 1 && pvar->protocol_minor == 99) {
		// ユーザが SSH2 を選択しているのならば
		if (pvar->settings.ssh_protocol_version == 2) {
			pvar->protocol_major = 2;
			pvar->protocol_minor = 0;
		}

	}

	for (str = str + 1; *str >= '0' && *str <= '9'; str++) {
	}

	return *str == '-';
}

/*
On entry, the pvar->protocol_xxx fields hold the server's advertised
protocol number. We replace the fields with the protocol number we will
actually use, or return FALSE if there is no usable protocol version.
*/
static BOOL negotiate_protocol(PTInstVar pvar)
{
	switch (pvar->protocol_major) {
	case 1:
		if (pvar->protocol_minor > 5) {
			pvar->protocol_minor = 5;
		}

		return TRUE;

	// for SSH2(yutaka)
	case 2:
		return TRUE;			// SSH2 support

	default:
		return FALSE;
	}
}

static void init_protocol(PTInstVar pvar)
{
	CRYPT_initialize_random_numbers(pvar);
	HOSTS_prefetch_host_key(pvar, pvar->ssh_state.hostname);
	/* while we wait for a response from the server... */

	if (SSHv1(pvar)) {
		enque_handler(pvar, SSH_MSG_DISCONNECT, handle_disconnect);
		enque_handler(pvar, SSH_MSG_IGNORE, handle_ignore);
		enque_handler(pvar, SSH_MSG_DEBUG, handle_debug);
		enque_handler(pvar, SSH_SMSG_PUBLIC_KEY, handle_server_public_key);

	} else {  // for SSH2(yutaka)
		enque_handler(pvar, SSH2_MSG_DISCONNECT, handle_disconnect);
		enque_handler(pvar, SSH2_MSG_IGNORE, handle_ignore);
		enque_handler(pvar, SSH2_MSG_DEBUG, handle_debug);
		enque_handler(pvar, SSH2_MSG_KEXINIT, handle_SSH2_kexinit);
		enque_handler(pvar, SSH2_MSG_KEXDH_INIT, handle_unimplemented);
		enque_handler(pvar, SSH2_MSG_KEXDH_REPLY, handle_SSH2_dh_common_reply); 
		enque_handler(pvar, SSH2_MSG_KEX_DH_GEX_REPLY, handle_SSH2_dh_gex_reply); 
		enque_handler(pvar, SSH2_MSG_NEWKEYS, handle_SSH2_newkeys);
		enque_handler(pvar, SSH2_MSG_SERVICE_ACCEPT, handle_SSH2_authrequest);
		enque_handler(pvar, SSH2_MSG_USERAUTH_SUCCESS, handle_SSH2_userauth_success);
		enque_handler(pvar, SSH2_MSG_USERAUTH_FAILURE, handle_SSH2_userauth_failure);
		enque_handler(pvar, SSH2_MSG_USERAUTH_BANNER, handle_SSH2_userauth_banner);
		enque_handler(pvar, SSH2_MSG_USERAUTH_INFO_REQUEST, handle_SSH2_userauth_inforeq);

		enque_handler(pvar, SSH2_MSG_UNIMPLEMENTED, handle_unimplemented);

		// ユーザ認証後のディスパッチルーチン
		enque_handler(pvar, SSH2_MSG_CHANNEL_CLOSE, handle_SSH2_channel_close);
		enque_handler(pvar, SSH2_MSG_CHANNEL_DATA, handle_SSH2_channel_data);
		enque_handler(pvar, SSH2_MSG_CHANNEL_EOF, handle_SSH2_channel_eof);
//		enque_handler(pvar, SSH2_MSG_CHANNEL_EXTENDED_DATA, handle_SSH2_channel_extended_data);
//		enque_handler(pvar, SSH2_MSG_CHANNEL_OPEN, handle_unimplemented);
		enque_handler(pvar, SSH2_MSG_CHANNEL_OPEN_CONFIRMATION, handle_SSH2_open_confirm);
//		enque_handler(pvar, SSH2_MSG_CHANNEL_OPEN_FAILURE, handle_unimplemented);
		enque_handler(pvar, SSH2_MSG_CHANNEL_REQUEST, handle_SSH2_channel_request);
		enque_handler(pvar, SSH2_MSG_CHANNEL_WINDOW_ADJUST, handle_SSH2_window_adjust);
		enque_handler(pvar, SSH2_MSG_CHANNEL_SUCCESS, handle_SSH2_channel_success);
//		enque_handler(pvar, SSH2_MSG_GLOBAL_REQUEST, handle_unimplemented);
//		enque_handler(pvar, SSH2_MSG_REQUEST_FAILURE, handle_unimplemented);
		enque_handler(pvar, SSH2_MSG_REQUEST_SUCCESS, handle_SSH2_request_success);

	}
}

BOOL SSH_handle_server_ID(PTInstVar pvar, char FAR * ID, int ID_len)
{
	static const char prefix[] = "Received server prologue string: ";

	// initialize SSH2 memory dump (2005.3.7 yutaka)
	init_memdump();
	push_memdump("pure server ID", "プロトコル識別文字列交換開始", ID, ID_len);

	if (ID_len <= 0) {
		return FALSE;
	} else {
		char FAR *buf = (char FAR *) malloc(ID_len + NUM_ELEM(prefix));

		strcpy(buf, prefix);
		strncpy(buf + NUM_ELEM(prefix) - 1, ID, ID_len);
		buf[NUM_ELEM(prefix) + ID_len - 1] = 0;
		chop_newlines(buf);

		notify_verbose_message(pvar, buf, LOG_LEVEL_VERBOSE);

		free(buf);


		// ここでのコピーは削除 (2005.3.9 yutaka)
#if 0
		// for calculate SSH2 hash
		// サーババージョンの保存（改行は取り除くこと）
		if (ID_len >= sizeof(pvar->server_version_string)) 
			return FALSE;
		strncpy(pvar->server_version_string, ID, ID_len);
#endif


		if (ID[ID_len - 1] != '\n') {
			pvar->ssh_state.status_flags |= STATUS_IN_PARTIAL_ID_STRING;
			return FALSE;
		} else
			if ((pvar->ssh_state.
				 status_flags & STATUS_IN_PARTIAL_ID_STRING) != 0) {
			pvar->ssh_state.status_flags &= ~STATUS_IN_PARTIAL_ID_STRING;
			return FALSE;
		} else if (strncmp(ID, "SSH-", 4) != 0) {
			return FALSE;
		} else {
			ID[ID_len - 1] = 0;

			if (ID_len > 1 && ID[ID_len - 2] == '\r') {
				ID[ID_len - 2] = 0;
			}

			pvar->ssh_state.server_ID = _strdup(ID);

			if (!parse_protocol_ID(pvar, ID) || !negotiate_protocol(pvar)) {
				notify_fatal_error(pvar,
								   "This program does not understand the server's version of the protocol.");
			} else {
				char TTSSH_ID[1024];
				int TTSSH_ID_len;
				int a, b, c, d;

				// 自分自身のバージョンを取得する (2005.3.3 yutaka)
				get_file_version("ttxssh.dll", &a, &b, &c, &d);

				_snprintf(TTSSH_ID, sizeof(TTSSH_ID),
						  "SSH-%d.%d-TTSSH/%d.%d Win32\n",
						  pvar->protocol_major, pvar->protocol_minor, a, b);
				TTSSH_ID_len = strlen(TTSSH_ID);

				// for SSH2(yutaka)
				// クライアントバージョンの保存（改行は取り除くこと）
				strncpy(pvar->client_version_string, TTSSH_ID, TTSSH_ID_len);

				// サーババージョンの保存（改行は取り除くこと）(2005.3.9 yutaka)
				_snprintf(pvar->server_version_string, sizeof(pvar->server_version_string), "%s", pvar->ssh_state.server_ID);

				if ((pvar->Psend) (pvar->socket, TTSSH_ID, TTSSH_ID_len,
								   0) != TTSSH_ID_len) {
					notify_fatal_error(pvar,
									   "An error occurred while sending the SSH ID string.\n"
									   "The connection will close.");
				} else {
					// 改行コードの除去 (2004.8.4 yutaka)
					pvar->client_version_string[--TTSSH_ID_len] = 0;

					push_memdump("server ID", NULL, pvar->server_version_string, strlen(pvar->server_version_string));
					push_memdump("client ID", NULL, pvar->client_version_string, strlen(pvar->client_version_string));

					// SSHハンドラの登録を行う
					init_protocol(pvar);

					SSH2_dispatch_init(1);
					SSH2_dispatch_add_message(SSH2_MSG_KEXINIT);
					SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.3 yutaka)
				}
			}

			return TRUE;
		}
	}
}

static BOOL handle_exit(PTInstVar pvar)
{
	if (grab_payload(pvar, 4)) {
		begin_send_packet(pvar, SSH_CMSG_EXIT_CONFIRMATION, 0);
		finish_send_packet(pvar);
		notify_closed_connection(pvar);
	}
	return TRUE;
}

static BOOL handle_data(PTInstVar pvar)
{
	if (grab_payload_limited(pvar, 4)) {
		pvar->ssh_state.payload_datalen = get_payload_uint32(pvar, 0);
		pvar->ssh_state.payload_datastart = 4;
	}
	return TRUE;
}

static BOOL handle_channel_open(PTInstVar pvar)
{
	int host_len;
	int originator_len;

	if ((pvar->ssh_state.
		 server_protocol_flags & SSH_PROTOFLAG_HOST_IN_FWD_OPEN) != 0) {
		if (grab_payload(pvar, 8)
			&& grab_payload(pvar,
							8 + (host_len = get_payload_uint32(pvar, 4)))
			&& grab_payload(pvar, originator_len =
							get_payload_uint32(pvar, host_len + 12))) {
			int local_port = get_payload_uint32(pvar, 8 + host_len);

			pvar->ssh_state.payload[8 + host_len] = 0;
			FWD_open(pvar, get_payload_uint32(pvar, 0),
					 pvar->ssh_state.payload + 8, local_port,
					 pvar->ssh_state.payload + 16 + host_len,
					 originator_len);
		}
	} else {
		if (grab_payload(pvar, 8)
			&& grab_payload(pvar,
							4 + (host_len =
								 get_payload_uint32(pvar, 4)))) {
			int local_port = get_payload_uint32(pvar, 8 + host_len);

			pvar->ssh_state.payload[8 + host_len] = 0;
			FWD_open(pvar, get_payload_uint32(pvar, 0),
					 pvar->ssh_state.payload + 8, local_port, NULL, 0);
		}
	}

	return TRUE;
}

static BOOL handle_X11_channel_open(PTInstVar pvar)
{
	int originator_len;

	if ((pvar->ssh_state.
		 server_protocol_flags & SSH_PROTOFLAG_HOST_IN_FWD_OPEN) != 0) {
		if (grab_payload(pvar, 8)
			&& grab_payload(pvar, originator_len =
							get_payload_uint32(pvar, 4))) {
			FWD_X11_open(pvar, get_payload_uint32(pvar, 0),
						 pvar->ssh_state.payload + 8, originator_len);
		}
	} else {
		if (grab_payload(pvar, 4)) {
			FWD_X11_open(pvar, get_payload_uint32(pvar, 0), NULL, 0);
		}
	}

	return TRUE;
}

static BOOL handle_channel_open_confirmation(PTInstVar pvar)
{
	if (grab_payload(pvar, 8)) {
		FWD_confirmed_open(pvar, get_payload_uint32(pvar, 0),
						   get_payload_uint32(pvar, 4));
	}
	return FALSE;
}

static BOOL handle_channel_open_failure(PTInstVar pvar)
{
	if (grab_payload(pvar, 4)) {
		FWD_failed_open(pvar, get_payload_uint32(pvar, 0));
	}
	return FALSE;
}

static BOOL handle_channel_data(PTInstVar pvar)
{
	int len;

	if (grab_payload(pvar, 8)
		&& grab_payload(pvar, len = get_payload_uint32(pvar, 4))) {
		FWD_received_data(pvar, get_payload_uint32(pvar, 0),
						  pvar->ssh_state.payload + 8, len);
	}
	return TRUE;
}

static BOOL handle_channel_input_eof(PTInstVar pvar)
{
	if (grab_payload(pvar, 4)) {
		FWD_channel_input_eof(pvar, get_payload_uint32(pvar, 0));
	}
	return TRUE;
}

static BOOL handle_channel_output_eof(PTInstVar pvar)
{
	if (grab_payload(pvar, 4)) {
		FWD_channel_output_eof(pvar, get_payload_uint32(pvar, 0));
	}
	return TRUE;
}



// ハンドリングするメッセージを決定する

#define HANDLE_MESSAGE_MAX 30
static unsigned char handle_messages[HANDLE_MESSAGE_MAX];
static int handle_message_count = 0;
static int handle_message_stage = 0;

void SSH2_dispatch_init(int stage)
{
	handle_message_count = 0;
	handle_message_stage = stage;
}

int SSH2_dispatch_enabled_check(unsigned char message)
{
	int i;

	for (i = 0 ; i < handle_message_count ; i++) {
		if (handle_messages[i] == message)
			return 1;
	}
	return 0;
}

void SSH2_dispatch_add_message(unsigned char message)
{

	if (handle_message_count >= HANDLE_MESSAGE_MAX) {
		// TODO: error check
		return;
	}

	handle_messages[handle_message_count++] = message;
}

void SSH2_dispatch_add_range_message(unsigned char begin, unsigned char end)
{
	unsigned char c;

	for (c = begin ; c <= end ; c++) {
		SSH2_dispatch_add_message(c);
	}
}


void SSH_handle_packet(PTInstVar pvar, char FAR * data, int len,
					   int padding)
{
	unsigned char message = prep_packet(pvar, data, len, padding);

	
#ifdef SSH2_DEBUG
	// for SSH2(yutaka)
	if (SSHv2(pvar)) {
		if (pvar->key_done) {
			message = message;
		}

		if (pvar->userauth_success) {
			message = message;
		}

		if (pvar->rekeying) {
			message = message;
		}
	}
#endif

	// SSHのメッセージタイプをチェック
	if (message != SSH_MSG_NONE) {
		// メッセージタイプに応じたハンドラを起動
		SSHPacketHandler handler = get_handler(pvar, message);

		// for SSH2(yutaka)
		if (SSHv2(pvar)) {
			// 想定外のメッセージタイプが到着したらアボートさせる。
			if (!SSH2_dispatch_enabled_check(message) || handler == NULL) {
				char buf[1024];

				_snprintf(buf, sizeof(buf),
					"Unexpected SSH2 message(%d) on current stage(%d)", message, handle_message_stage);
				notify_fatal_error(pvar, buf);
				// abort
			}
		}

		if (handler == NULL) {
			if (SSHv1(pvar)) {
				char buf[1024];

				_snprintf(buf, sizeof(buf),
						  "Unexpected packet type received: %d", message);
				buf[sizeof(buf) - 1] = 0;
				notify_fatal_error(pvar, buf);
			} else {
				unsigned char FAR *outmsg =
					begin_send_packet(pvar, SSH2_MSG_UNIMPLEMENTED, 4);

				set_uint32(outmsg,
						   pvar->ssh_state.receiver_sequence_number - 1);
				finish_send_packet(pvar);
				/* XXX need to decompress incoming packet, but how? */
			}
		} else {
			if (!handler(pvar)) {
				deque_handlers(pvar, message);
			}
		}
	}
}

static BOOL handle_pty_success(PTInstVar pvar)
{
	FWD_enter_interactive_mode(pvar);
	enque_handler(pvar, SSH_SMSG_EXITSTATUS, handle_exit);
	enque_handler(pvar, SSH_SMSG_STDOUT_DATA, handle_data);
	enque_handler(pvar, SSH_SMSG_STDERR_DATA, handle_data);
	enque_handler(pvar, SSH_MSG_CHANNEL_DATA, handle_channel_data);
	enque_handler(pvar, SSH_MSG_CHANNEL_INPUT_EOF,
				  handle_channel_input_eof);
	enque_handler(pvar, SSH_MSG_CHANNEL_OUTPUT_CLOSED,
				  handle_channel_output_eof);
	enque_handler(pvar, SSH_MSG_PORT_OPEN, handle_channel_open);
	enque_handler(pvar, SSH_SMSG_X11_OPEN, handle_X11_channel_open);
	return FALSE;
}

static BOOL handle_pty_failure(PTInstVar pvar)
{
	notify_nonfatal_error(pvar,
						  "The server cannot allocate a pseudo-terminal. "
						  "You may encounter some problems with the terminal.");
	return handle_pty_success(pvar);
}

static void prep_pty(PTInstVar pvar)
{
	int len = strlen(pvar->ts->TermType);
	unsigned char FAR *outmsg =
		begin_send_packet(pvar, SSH_CMSG_REQUEST_PTY,
						  4 + len + 16 + sizeof(ssh_ttymodes));
	static const int msgs[] = { SSH_SMSG_SUCCESS, SSH_SMSG_FAILURE };
	static const SSHPacketHandler handlers[]
	= { handle_pty_success, handle_pty_failure };

	set_uint32(outmsg, len);
	memcpy(outmsg + 4, pvar->ts->TermType, len);
	set_uint32(outmsg + 4 + len, pvar->ssh_state.win_rows);
	set_uint32(outmsg + 4 + len + 4, pvar->ssh_state.win_cols);
	set_uint32(outmsg + 4 + len + 8, 0);
	set_uint32(outmsg + 4 + len + 12, 0);
	memcpy(outmsg + 4 + len + 16, ssh_ttymodes, sizeof(ssh_ttymodes));
	finish_send_packet(pvar);

	enque_handlers(pvar, 2, msgs, handlers);

	begin_send_packet(pvar, SSH_CMSG_EXEC_SHELL, 0);
	finish_send_packet(pvar);
}

static void prep_forwarding(PTInstVar pvar)
{
	FWD_prep_forwarding(pvar);
	prep_pty(pvar);
}

static void enable_compression(PTInstVar pvar)
{
	pvar->ssh_state.compress_stream.zalloc = NULL;
	pvar->ssh_state.compress_stream.zfree = NULL;
	pvar->ssh_state.compress_stream.opaque = NULL;
	if (deflateInit
		(&pvar->ssh_state.compress_stream,
		 pvar->ssh_state.compression_level) != Z_OK) {
		notify_fatal_error(pvar,
						   "An error occurred while setting up compression.\n"
						   "The connection will close.");
		return;
	} else {
		pvar->ssh_state.compressing = TRUE;
	}

	pvar->ssh_state.decompress_stream.zalloc = NULL;
	pvar->ssh_state.decompress_stream.zfree = NULL;
	pvar->ssh_state.decompress_stream.opaque = NULL;
	if (inflateInit(&pvar->ssh_state.decompress_stream) != Z_OK) {
		deflateEnd(&pvar->ssh_state.compress_stream);
		notify_fatal_error(pvar,
						   "An error occurred while setting up compression.\n"
						   "The connection will close.");
		return;
	} else {
		pvar->ssh_state.decompressing = TRUE;
		buf_ensure_size(&pvar->ssh_state.postdecompress_inbuf,
						&pvar->ssh_state.postdecompress_inbuflen, 1000);
	}
}

static BOOL handle_enable_compression(PTInstVar pvar)
{
	enable_compression(pvar);
	prep_forwarding(pvar);
	return FALSE;
}

static BOOL handle_disable_compression(PTInstVar pvar)
{
	prep_forwarding(pvar);
	return FALSE;
}

static void prep_compression(PTInstVar pvar)
{
	if (pvar->session_settings.CompressionLevel > 0) {
		static const int msgs[] = { SSH_SMSG_SUCCESS, SSH_SMSG_FAILURE };
		static const SSHPacketHandler handlers[]
		= { handle_enable_compression, handle_disable_compression };

		unsigned char FAR *outmsg =
			begin_send_packet(pvar, SSH_CMSG_REQUEST_COMPRESSION, 4);

		set_uint32(outmsg, pvar->session_settings.CompressionLevel);
		finish_send_packet(pvar);

		pvar->ssh_state.compression_level =
			pvar->session_settings.CompressionLevel;

		enque_handlers(pvar, 2, msgs, handlers);
	} else {
		prep_forwarding(pvar);
	}
}

static void enque_simple_auth_handlers(PTInstVar pvar)
{
	static const int msgs[] = { SSH_SMSG_SUCCESS, SSH_SMSG_FAILURE };
	static const SSHPacketHandler handlers[]
	= { handle_auth_success, handle_auth_failure };

	enque_handlers(pvar, 2, msgs, handlers);
}

static BOOL handle_rsa_challenge(PTInstVar pvar)
{
	int challenge_bytes;

	if (!grab_payload(pvar, 2)) {
		return FALSE;
	}

	challenge_bytes = get_mpint_len(pvar, 0);

	if (grab_payload(pvar, challenge_bytes)) {
		unsigned char FAR *outmsg =
			begin_send_packet(pvar, SSH_CMSG_AUTH_RSA_RESPONSE, 16);

		if (CRYPT_generate_RSA_challenge_response
			(pvar, pvar->ssh_state.payload + 2, challenge_bytes, outmsg)) {

			// セッション複製時にパスワードを使い回したいので、ここでのリソース解放はやめる。
			// socket close時にもこの関数は呼ばれているので、たぶん問題ない。(2005.4.8 yutaka)
#if 0
			//AUTH_destroy_cur_cred(pvar);
#endif

			finish_send_packet(pvar);

			enque_simple_auth_handlers(pvar);
		} else {
			notify_fatal_error(pvar,
							   "An error occurred while decrypting the RSA challenge.\n"
							   "Perhaps the key file is corrupted.");
		}
	}

	return FALSE;
}

#define OBFUSCATING_ROUND_TO 32

static int obfuscating_round_up(PTInstVar pvar, int size)
{
	return (size + OBFUSCATING_ROUND_TO - 1) & ~(OBFUSCATING_ROUND_TO - 1);
}

static void try_send_credentials(PTInstVar pvar)
{
	if ((pvar->ssh_state.status_flags & STATUS_DONT_SEND_CREDENTIALS) == 0) {
		AUTHCred FAR *cred = AUTH_get_cur_cred(pvar);
		static const int RSA_msgs[] =
			{ SSH_SMSG_AUTH_RSA_CHALLENGE, SSH_SMSG_FAILURE };
		static const SSHPacketHandler RSA_handlers[]
		= { handle_rsa_challenge, handle_rsa_auth_refused };
		static const int TIS_msgs[] =
			{ SSH_SMSG_AUTH_TIS_CHALLENGE, SSH_SMSG_FAILURE };
		static const SSHPacketHandler TIS_handlers[]
		= { handle_TIS_challenge, handle_auth_failure };

		// SSH2の場合は以下の処理をスキップ
		if (SSHv2(pvar)) 
			goto skip_ssh2;

		switch (cred->method) {
		case SSH_AUTH_NONE:
			return;
		case SSH_AUTH_PASSWORD:{
				int len = strlen(cred->password);
				// Round up password length to discourage traffic analysis
				int obfuscated_len = obfuscating_round_up(pvar, len);
				unsigned char FAR *outmsg =
					begin_send_packet(pvar, SSH_CMSG_AUTH_PASSWORD,
									  4 + obfuscated_len);

				notify_verbose_message(pvar,
									   "Trying PASSWORD authentication...",
									   LOG_LEVEL_VERBOSE);

				set_uint32(outmsg, obfuscated_len);
				memcpy(outmsg + 4, cred->password, len);
				memset(outmsg + 4 + len, 0, obfuscated_len - len);
				
				// セッション複製時にパスワードを使い回したいので、ここでのリソース解放はやめる。
				// socket close時にもこの関数は呼ばれているので、たぶん問題ない。(2005.4.8 yutaka)
#if 0
				//AUTH_destroy_cur_cred(pvar);
#endif
				
				enque_simple_auth_handlers(pvar);
				break;
			}
		case SSH_AUTH_RHOSTS:{
				int len = strlen(cred->rhosts_client_user);
				unsigned char FAR *outmsg =
					begin_send_packet(pvar, SSH_CMSG_AUTH_RHOSTS, 4 + len);

				notify_verbose_message(pvar,
									   "Trying RHOSTS authentication...",
									   LOG_LEVEL_VERBOSE);

				set_uint32(outmsg, len);
				memcpy(outmsg + 4, cred->rhosts_client_user, len);
				AUTH_destroy_cur_cred(pvar);
				enque_simple_auth_handlers(pvar);
				break;
			}
		case SSH_AUTH_RSA:{
				int len = BN_num_bytes(cred->key_pair->RSA_key->n);
				unsigned char FAR *outmsg =
					begin_send_packet(pvar, SSH_CMSG_AUTH_RSA, 2 + len);

				notify_verbose_message(pvar,
									   "Trying RSA authentication...",
									   LOG_LEVEL_VERBOSE);

				set_ushort16_MSBfirst(outmsg, len * 8);
				BN_bn2bin(cred->key_pair->RSA_key->n, outmsg + 2);
				/* don't destroy the current credentials yet */
				enque_handlers(pvar, 2, RSA_msgs, RSA_handlers);
				break;
			}
		case SSH_AUTH_RHOSTS_RSA:{
				int mod_len = BN_num_bytes(cred->key_pair->RSA_key->n);
				int name_len = strlen(cred->rhosts_client_user);
				int exp_len = BN_num_bytes(cred->key_pair->RSA_key->e);
				int index;
				unsigned char FAR *outmsg =
					begin_send_packet(pvar, SSH_CMSG_AUTH_RHOSTS_RSA,
									  12 + mod_len + name_len + exp_len);

				notify_verbose_message(pvar,
									   "Trying RHOSTS+RSA authentication...",
									   LOG_LEVEL_VERBOSE);

				set_uint32(outmsg, name_len);
				memcpy(outmsg + 4, cred->rhosts_client_user, name_len);
				index = 4 + name_len;

				set_uint32(outmsg + index, 8 * mod_len);
				set_ushort16_MSBfirst(outmsg + index + 4, 8 * exp_len);
				BN_bn2bin(cred->key_pair->RSA_key->e, outmsg + index + 6);
				index += 6 + exp_len;

				set_ushort16_MSBfirst(outmsg + index, 8 * mod_len);
				BN_bn2bin(cred->key_pair->RSA_key->n, outmsg + index + 2);
				/* don't destroy the current credentials yet */
				enque_handlers(pvar, 2, RSA_msgs, RSA_handlers);
				break;
			}
		case SSH_AUTH_TIS:{
				if (cred->password == NULL) {
					unsigned char FAR *outmsg =
						begin_send_packet(pvar, SSH_CMSG_AUTH_TIS, 0);

					notify_verbose_message(pvar,
										"Trying TIS authentication...",
										LOG_LEVEL_VERBOSE);
					enque_handlers(pvar, 2, TIS_msgs, TIS_handlers);
				} else {
					int len = strlen(cred->password);
					int obfuscated_len = obfuscating_round_up(pvar, len);
					unsigned char FAR *outmsg =
						begin_send_packet(pvar, SSH_CMSG_AUTH_TIS_RESPONSE,
										4 + obfuscated_len);

					notify_verbose_message(pvar, "Sending TIS response",
										LOG_LEVEL_VERBOSE);

					set_uint32(outmsg, obfuscated_len);
					memcpy(outmsg + 4, cred->password, len);
					memset(outmsg + 4 + len, 0, obfuscated_len - len);
					enque_simple_auth_handlers(pvar);
				}

				AUTH_destroy_cur_cred(pvar);
				break;
			}
		default:
			notify_fatal_error(pvar,
							   "Internal error: unsupported authentication method");
			return;
		}

		finish_send_packet(pvar);

skip_ssh2:;
		destroy_packet_buf(pvar);

		pvar->ssh_state.status_flags |= STATUS_DONT_SEND_CREDENTIALS;
	}
}

static void try_send_user_name(PTInstVar pvar)
{
	if ((pvar->ssh_state.status_flags & STATUS_DONT_SEND_USER_NAME) == 0) {
		char FAR *username = AUTH_get_user_name(pvar);

		if (username != NULL) {
			int len = strlen(username);
			int obfuscated_len = obfuscating_round_up(pvar, len);
			unsigned char FAR *outmsg =
				begin_send_packet(pvar, SSH_CMSG_USER, 4 + obfuscated_len);
			char buf[1024] = "Sending user name: ";
			static const int msgs[] =
				{ SSH_SMSG_SUCCESS, SSH_SMSG_FAILURE };
			static const SSHPacketHandler handlers[]
			= { handle_noauth_success, handle_auth_required };

			set_uint32(outmsg, obfuscated_len);
			memcpy(outmsg + 4, username, len);
			memset(outmsg + 4 + len, 0, obfuscated_len - len);
			finish_send_packet(pvar);

			pvar->ssh_state.status_flags |= STATUS_DONT_SEND_USER_NAME;

			strncpy(buf + strlen(buf), username,
					sizeof(buf) - strlen(buf) - 2);
			buf[sizeof(buf) - 1] = 0;
			notify_verbose_message(pvar, buf, LOG_LEVEL_VERBOSE);

			enque_handlers(pvar, 2, msgs, handlers);
		}
	}
}

static void send_session_key(PTInstVar pvar)
{
	int encrypted_session_key_len;
	unsigned char FAR *outmsg;

	if (SSHv1(pvar)) {
		encrypted_session_key_len =
			CRYPT_get_encrypted_session_key_len(pvar);
	}

	if (!CRYPT_choose_ciphers(pvar))
		return;

	if (SSHv1(pvar)) {
		outmsg =
			begin_send_packet(pvar, SSH_CMSG_SESSION_KEY,
							  15 + encrypted_session_key_len);
		outmsg[0] = (unsigned char) CRYPT_get_sender_cipher(pvar);
		memcpy(outmsg + 1, CRYPT_get_server_cookie(pvar), 8);	/* antispoofing cookie */
		outmsg[9] = (unsigned char) (encrypted_session_key_len >> 5);
		outmsg[10] = (unsigned char) (encrypted_session_key_len << 3);
		if (!CRYPT_choose_session_key(pvar, outmsg + 11))
			return;
		set_uint32(outmsg + 11 + encrypted_session_key_len,
				   SSH_PROTOFLAG_SCREEN_NUMBER |
				   SSH_PROTOFLAG_HOST_IN_FWD_OPEN);
		finish_send_packet(pvar);
	}

	if (!CRYPT_start_encryption(pvar, 1, 1))
		return;
	notify_established_secure_connection(pvar);

	if (SSHv1(pvar)) {
		enque_handler(pvar, SSH_SMSG_SUCCESS, handle_crypt_success);
	}

	pvar->ssh_state.status_flags &= ~STATUS_DONT_SEND_USER_NAME;

	if (SSHv1(pvar)) {
		try_send_user_name(pvar);
	}
}

/*************************
   END of message handlers
   ************************/

void SSH_init(PTInstVar pvar)
{
	int i;

	buf_create(&pvar->ssh_state.outbuf, &pvar->ssh_state.outbuflen);
	buf_create(&pvar->ssh_state.precompress_outbuf,
			   &pvar->ssh_state.precompress_outbuflen);
	buf_create(&pvar->ssh_state.postdecompress_inbuf,
			   &pvar->ssh_state.postdecompress_inbuflen);
	pvar->ssh_state.payload = NULL;
	pvar->ssh_state.compressing = FALSE;
	pvar->ssh_state.decompressing = FALSE;
	pvar->ssh_state.status_flags =
		STATUS_DONT_SEND_USER_NAME | STATUS_DONT_SEND_CREDENTIALS;
	pvar->ssh_state.payload_datalen = 0;
	pvar->ssh_state.hostname = NULL;
	pvar->ssh_state.server_ID = NULL;
	pvar->ssh_state.receiver_sequence_number = 0;
	pvar->ssh_state.sender_sequence_number = 0;
	for (i = 0; i < NUM_ELEM(pvar->ssh_state.packet_handlers); i++) {
		pvar->ssh_state.packet_handlers[i] = NULL;
	}

	// for SSH2(yutaka)
	memset(pvar->ssh2_keys, 0, sizeof(pvar->ssh2_keys));
	pvar->userauth_success = 0;
	pvar->session_nego_status = 0;
	pvar->settings.ssh_protocol_version = 2;  // SSH2(default)
	pvar->rekeying = 0;
	pvar->key_done = 0;
	pvar->ssh2_autologin = 0;  // autologin disabled(default)
	pvar->userauth_retry_count = 0;

}

void SSH_open(PTInstVar pvar)
{
	pvar->ssh_state.hostname = _strdup(pvar->ts->HostName);
	pvar->ssh_state.win_cols = pvar->ts->TerminalWidth;
	pvar->ssh_state.win_rows = pvar->ts->TerminalHeight;
}

void SSH_notify_disconnecting(PTInstVar pvar, char FAR * reason)
{
	if (SSHv1(pvar)) {
		int len = reason == NULL ? 0 : strlen(reason);
		unsigned char FAR *outmsg =
			begin_send_packet(pvar, SSH_MSG_DISCONNECT, len + 4);

		set_uint32(outmsg, len);
		if (reason != NULL) {
			memcpy(outmsg + 4, reason, len);
		}
		finish_send_packet(pvar);

	} else { // for SSH2(yutaka)
		buffer_t *msg;
		unsigned char *outmsg;
		int len;
		Channel_t *c;

		c = ssh2_channel_lookup(pvar->shell_id);
		if (c == NULL)
			return;

		// SSH2 serverにchannel closeを伝える
		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return;
		}
		buffer_put_int(msg, c->remote_id);  

		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_CLOSE, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);

	}

}

void SSH_notify_host_OK(PTInstVar pvar)
{
	if ((pvar->ssh_state.status_flags & STATUS_HOST_OK) == 0) {
		pvar->ssh_state.status_flags |= STATUS_HOST_OK;
		send_session_key(pvar);
	}
}

void SSH_notify_win_size(PTInstVar pvar, int cols, int rows)
{
	pvar->ssh_state.win_cols = cols;
	pvar->ssh_state.win_rows = rows;

	if (SSHv1(pvar)) {
		if (get_handler(pvar, SSH_SMSG_STDOUT_DATA) == handle_data) {
			unsigned char FAR *outmsg =
				begin_send_packet(pvar, SSH_CMSG_WINDOW_SIZE, 16);

			set_uint32(outmsg, rows);
			set_uint32(outmsg + 4, cols);
			set_uint32(outmsg + 8, 0);
			set_uint32(outmsg + 12, 0);
			finish_send_packet(pvar);
		}

	} else if (SSHv2(pvar)) { // ターミナルサイズ変更通知の追加 (2005.1.4 yutaka)
		                      // SSH2かどうかのチェックも行う。(2005.1.5 yutaka)
		buffer_t *msg;
		char *s;
		unsigned char *outmsg;
		int len;
		Channel_t *c;

		c = ssh2_channel_lookup(pvar->shell_id);
		if (c == NULL)
			return;

		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return;
		}
		buffer_put_int(msg, c->remote_id);  
		s = "window-change";  
		buffer_put_string(msg, s, strlen(s));  
		buffer_put_char(msg, 0);  // wantconfirm
		buffer_put_int(msg, pvar->ssh_state.win_cols);  // columns
		buffer_put_int(msg, pvar->ssh_state.win_rows);  // lines
		buffer_put_int(msg, 480);  // XXX:
		buffer_put_int(msg, 640);  // XXX:
		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_REQUEST, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);

		notify_verbose_message(pvar, "SSH2_MSG_CHANNEL_REQUEST was sent at SSH_notify_win_size().", LOG_LEVEL_VERBOSE);

	} else {
		// SSHでない場合は何もしない。

	}

}

int SSH_get_min_packet_size(PTInstVar pvar)
{
	if (SSHv1(pvar)) {
		return 12;
	} else {
		int block_size = CRYPT_get_decryption_block_size(pvar);

		return max(16, block_size);
	}
}

/* data is guaranteed to be at least SSH_get_min_packet_size bytes long
   at least 5 bytes must be decrypted */
void SSH_predecrpyt_packet(PTInstVar pvar, char FAR * data)
{
	if (SSHv2(pvar)) {
		CRYPT_decrypt(pvar, data, get_predecryption_amount(pvar));
	}
}

int SSH_get_clear_MAC_size(PTInstVar pvar)
{
	if (SSHv1(pvar)) {
		return 0;
	} else {
		return CRYPT_get_receiver_MAC_size(pvar);
	}
}

void SSH_notify_user_name(PTInstVar pvar)
{
	try_send_user_name(pvar);
}

void SSH_notify_cred(PTInstVar pvar)
{
	try_send_credentials(pvar);
}

void SSH_send(PTInstVar pvar, unsigned char const FAR * buf, int buflen)
{
	if (SSHv1(pvar)) {
		if (get_handler(pvar, SSH_SMSG_STDOUT_DATA) != handle_data) {
			return;
		}

		while (buflen > 0) {
			int len =
				buflen >
				SSH_MAX_SEND_PACKET_SIZE ? SSH_MAX_SEND_PACKET_SIZE : buflen;
			unsigned char FAR *outmsg =
				begin_send_packet(pvar, SSH_CMSG_STDIN_DATA, 4 + len);

			set_uint32(outmsg, len);

			if (pvar->ssh_state.compressing) {
				buf_ensure_size(&pvar->ssh_state.outbuf,
								&pvar->ssh_state.outbuflen,
								len + (len >> 6) + 50);
				pvar->ssh_state.compress_stream.next_in =
					pvar->ssh_state.precompress_outbuf;
				pvar->ssh_state.compress_stream.avail_in = 5;
				pvar->ssh_state.compress_stream.next_out =
					pvar->ssh_state.outbuf + 12;
				pvar->ssh_state.compress_stream.avail_out =
					pvar->ssh_state.outbuflen - 12;

				if (deflate(&pvar->ssh_state.compress_stream, Z_NO_FLUSH) !=
					Z_OK) {
					notify_fatal_error(pvar, "Error compressing packet data");
					return;
				}

				pvar->ssh_state.compress_stream.next_in =
					(unsigned char FAR *) buf;
				pvar->ssh_state.compress_stream.avail_in = len;

				if (deflate(&pvar->ssh_state.compress_stream, Z_SYNC_FLUSH) !=
					Z_OK) {
					notify_fatal_error(pvar, "Error compressing packet data");
					return;
				}
			} else {
				memcpy(outmsg + 4, buf, len);
			}

			finish_send_packet_special(pvar, 1);

			buflen -= len;
			buf += len;
		}

	} else { // for SSH2(yutaka)
		buffer_t *msg;
		unsigned char *outmsg;
		int len;
		Channel_t *c;

		// SSH2鍵交換中の場合、パケットを捨てる。(2005.6.19 yutaka)
		if (pvar->rekeying) {
			// TODO: 理想としてはパケット破棄ではなく、パケット読み取り遅延にしたいところだが、
			// 将来直すことにする。
			c = NULL;

			return;
		}

		c = ssh2_channel_lookup(pvar->shell_id);
		if (c == NULL)
			return;

		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return;
		}
		buffer_put_int(msg, c->remote_id);  
		buffer_put_string(msg, (char *)buf, buflen);  

		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_DATA, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);

		// remote window sizeの調整
		c->remote_window -= len;

	}

}

int SSH_extract_payload(PTInstVar pvar, unsigned char FAR * dest, int len)
{
	int num_bytes = pvar->ssh_state.payload_datalen;

	if (num_bytes > len) {
		num_bytes = len;
	}

	if (!pvar->ssh_state.decompressing) {
		memcpy(dest,
			   pvar->ssh_state.payload + pvar->ssh_state.payload_datastart,
			   num_bytes);
		pvar->ssh_state.payload_datastart += num_bytes;
	} else if (num_bytes > 0) {
		pvar->ssh_state.decompress_stream.next_out = dest;
		pvar->ssh_state.decompress_stream.avail_out = num_bytes;

		if (inflate(&pvar->ssh_state.decompress_stream, Z_SYNC_FLUSH) !=
			Z_OK) {
			notify_fatal_error(pvar,
							   "Invalid compressed data in received packet");
			return 0;
		}
	}

	pvar->ssh_state.payload_datalen -= num_bytes;

	return num_bytes;
}

void SSH_get_compression_info(PTInstVar pvar, char FAR * dest, int len)
{
	char buf[1024];
	char buf2[1024];

	if (pvar->ssh_state.compressing) {
		unsigned long total_in = pvar->ssh_state.compress_stream.total_in;
		unsigned long total_out =
			pvar->ssh_state.compress_stream.total_out;

		if (total_out > 0) {
			_snprintf(buf, sizeof(buf), "level %d; ratio %.1f (%ld:%ld)",
					  pvar->ssh_state.compression_level,
					  ((double) total_in) / total_out, total_in,
					  total_out);
		} else {
			_snprintf(buf, sizeof(buf), "level %d",
					  pvar->ssh_state.compression_level);
		}
	} else {
		strcpy(buf, "none");
	}
	buf[sizeof(buf) - 1] = 0;

	if (pvar->ssh_state.decompressing) {
		unsigned long total_in =
			pvar->ssh_state.decompress_stream.total_in;
		unsigned long total_out =
			pvar->ssh_state.decompress_stream.total_out;

		if (total_in > 0) {
			_snprintf(buf2, sizeof(buf2), "level %d; ratio %.1f (%ld:%ld)",
					  pvar->ssh_state.compression_level,
					  ((double) total_out) / total_in, total_out,
					  total_in);
		} else {
			_snprintf(buf2, sizeof(buf2), "level %d",
					  pvar->ssh_state.compression_level);
		}
	} else {
		strcpy(buf2, "none");
	}
	buf2[sizeof(buf2) - 1] = 0;

	_snprintf(dest, len, "Upstream %s; Downstream %s", buf, buf2);
	dest[len - 1] = 0;
}

void SSH_get_server_ID_info(PTInstVar pvar, char FAR * dest, int len)
{
	strncpy(dest, pvar->ssh_state.server_ID == NULL ? "Unknown"
			: pvar->ssh_state.server_ID, len);
	dest[len - 1] = 0;
}

void SSH_get_protocol_version_info(PTInstVar pvar, char FAR * dest,
								   int len)
{
	if (pvar->protocol_major == 0) {
		strncpy(dest, "Unknown", len);
	} else {
		_snprintf(dest, len, "%d.%d", pvar->protocol_major,
				  pvar->protocol_minor);
	}
	dest[len - 1] = 0;
}

void SSH_end(PTInstVar pvar)
{
	int i;

	for (i = 0; i < 256; i++) {
		SSHPacketHandlerItem FAR *first_item =
			pvar->ssh_state.packet_handlers[i];

		if (first_item != NULL) {
			SSHPacketHandlerItem FAR *item = first_item;

			do {
				SSHPacketHandlerItem FAR *cur_item = item;

				item = item->next_for_message;
				free(cur_item);
			} while (item != first_item);
		}
		pvar->ssh_state.packet_handlers[i] = NULL;
	}

	free(pvar->ssh_state.hostname);
	pvar->ssh_state.hostname = NULL;
	free(pvar->ssh_state.server_ID);
	pvar->ssh_state.server_ID = NULL;
	buf_destroy(&pvar->ssh_state.outbuf, &pvar->ssh_state.outbuflen);
	buf_destroy(&pvar->ssh_state.precompress_outbuf,
				&pvar->ssh_state.precompress_outbuflen);
	buf_destroy(&pvar->ssh_state.postdecompress_inbuf,
				&pvar->ssh_state.postdecompress_inbuflen);

	if (pvar->ssh_state.compressing) {
		deflateEnd(&pvar->ssh_state.compress_stream);
		pvar->ssh_state.compressing = FALSE;
	}
	if (pvar->ssh_state.decompressing) {
		inflateEnd(&pvar->ssh_state.decompress_stream);
		pvar->ssh_state.decompressing = FALSE;
	}

#if 1
	// SSH2のデータを解放する (2004.12.27 yutaka)
	if (SSHv2(pvar)) {
		if (pvar->kexdh) {
			DH_free(pvar->kexdh);
			pvar->kexdh = NULL;
		}
		memset(pvar->server_version_string, 0, sizeof(pvar->server_version_string));
		memset(pvar->client_version_string, 0, sizeof(pvar->client_version_string));

		if (pvar->my_kex != NULL) {
			buffer_free(pvar->my_kex);
			pvar->my_kex = NULL;
		}
		if (pvar->peer_kex != NULL) {
			buffer_free(pvar->peer_kex);
			pvar->peer_kex = NULL;
		}

		pvar->we_need = 0;
		pvar->key_done = 0;
		pvar->rekeying = 0;

		if (pvar->session_id != NULL) {
			free(pvar->session_id);
			pvar->session_id = NULL;
		}
		pvar->session_id_len = 0;

		pvar->userauth_success = 0;
		//pvar->remote_id = 0;
		pvar->shell_id = 0;
		pvar->session_nego_status = 0;

		pvar->ssh_heartbeat_tick = 0;
	}
#endif

}

/* support for port forwarding */
void SSH_channel_send(PTInstVar pvar, int channel_num,
					  uint32 remote_channel_num,
					  unsigned char FAR * buf, int len)
{
	int buflen = len;

	if (SSHv1(pvar)) {
		unsigned char FAR *outmsg =
			begin_send_packet(pvar, SSH_MSG_CHANNEL_DATA, 8 + len);

		set_uint32(outmsg, remote_channel_num);
		set_uint32(outmsg + 4, len);

		if (pvar->ssh_state.compressing) {
			buf_ensure_size(&pvar->ssh_state.outbuf,
							&pvar->ssh_state.outbuflen, len + (len >> 6) + 50);
			pvar->ssh_state.compress_stream.next_in =
				pvar->ssh_state.precompress_outbuf;
			pvar->ssh_state.compress_stream.avail_in = 9;
			pvar->ssh_state.compress_stream.next_out =
				pvar->ssh_state.outbuf + 12;
			pvar->ssh_state.compress_stream.avail_out =
				pvar->ssh_state.outbuflen - 12;

			if (deflate(&pvar->ssh_state.compress_stream, Z_NO_FLUSH) != Z_OK) {
				notify_fatal_error(pvar, "Error compressing packet data");
				return;
			}

			pvar->ssh_state.compress_stream.next_in =
				(unsigned char FAR *) buf;
			pvar->ssh_state.compress_stream.avail_in = len;

			if (deflate(&pvar->ssh_state.compress_stream, Z_SYNC_FLUSH) !=
				Z_OK) {
				notify_fatal_error(pvar, "Error compressing packet data");
				return;
			}
		} else {
			memcpy(outmsg + 8, buf, len);
		}

		finish_send_packet_special(pvar, 1);

	} else {
		// ポートフォワーディングにおいてクライアントからの送信要求を、SSH通信に乗せてサーバまで送り届ける。
		buffer_t *msg;
		unsigned char *outmsg;
		int len;
		Channel_t *c;

		// SSH2鍵交換中の場合、パケットを捨てる。(2005.6.19 yutaka)
		if (pvar->rekeying) {
			// TODO: 理想としてはパケット破棄ではなく、パケット読み取り遅延にしたいところだが、
			// 将来直すことにする。
			c = NULL;

			return;
		}

		c = ssh2_local_channel_lookup(channel_num);
		if (c == NULL)
			return;

		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return;
		}
		buffer_put_int(msg, c->remote_id);  
		buffer_put_string(msg, (char *)buf, buflen);  

		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_DATA, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);

		// remote window sizeの調整
		c->remote_window -= len;
	}

}

void SSH_fail_channel_open(PTInstVar pvar, uint32 remote_channel_num)
{
	unsigned char FAR *outmsg =
		begin_send_packet(pvar, SSH_MSG_CHANNEL_OPEN_FAILURE, 4);

	set_uint32(outmsg, remote_channel_num);
	finish_send_packet(pvar);
}

void SSH_confirm_channel_open(PTInstVar pvar, uint32 remote_channel_num,
							  uint32 local_channel_num)
{
	unsigned char FAR *outmsg =
		begin_send_packet(pvar, SSH_MSG_CHANNEL_OPEN_CONFIRMATION, 8);

	set_uint32(outmsg, remote_channel_num);
	set_uint32(outmsg + 4, local_channel_num);
	finish_send_packet(pvar);
}

void SSH_channel_output_eof(PTInstVar pvar, uint32 remote_channel_num)
{
	if (SSHv1(pvar)){
		unsigned char FAR *outmsg =
			begin_send_packet(pvar, SSH_MSG_CHANNEL_OUTPUT_CLOSED, 4);

		set_uint32(outmsg, remote_channel_num);
		finish_send_packet(pvar);

	} else {
		// SSH2: 特になし。

	}
}

void SSH_channel_input_eof(PTInstVar pvar, uint32 remote_channel_num, uint32 local_channel_num)
{
	if (SSHv1(pvar)){
		unsigned char FAR *outmsg =
			begin_send_packet(pvar, SSH_MSG_CHANNEL_INPUT_EOF, 4);

		set_uint32(outmsg, remote_channel_num);
		finish_send_packet(pvar);

	} else {
		// SSH2: チャネルクローズをサーバへ通知
			buffer_t *msg;
			unsigned char *outmsg;
			int len;
			Channel_t *c;

			// SSH2鍵交換中の場合、パケットを捨てる。(2005.6.21 yutaka)
			if (pvar->rekeying) {
				// TODO: 理想としてはパケット破棄ではなく、パケット読み取り遅延にしたいところだが、
				// 将来直すことにする。
				c = NULL;

				return;
			}

			c = ssh2_local_channel_lookup(local_channel_num);
			if (c == NULL)
				return;

			msg = buffer_init();
			if (msg == NULL) {
				// TODO: error check
				return;
			}
			buffer_put_int(msg, c->remote_id);  // remote ID

			len = buffer_len(msg);
			outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_EOF, len);
			memcpy(outmsg, buffer_ptr(msg), len);
			finish_send_packet(pvar);
			buffer_free(msg);
	}

}

void SSH_request_forwarding(PTInstVar pvar, int from_server_port,
							char FAR * to_local_host, int to_local_port)
{
	int host_len = strlen(to_local_host);
	unsigned char FAR *outmsg =
		begin_send_packet(pvar, SSH_CMSG_PORT_FORWARD_REQUEST,
						  12 + host_len);

	set_uint32(outmsg, from_server_port);
	set_uint32(outmsg + 4, host_len);
	memcpy(outmsg + 8, to_local_host, host_len);
	set_uint32(outmsg + 8 + host_len, to_local_port);
	finish_send_packet(pvar);

	enque_forwarding_request_handlers(pvar);
}

void SSH_request_X11_forwarding(PTInstVar pvar,
								char FAR * auth_protocol,
								unsigned char FAR * auth_data,
								int auth_data_len, int screen_num)
{
	int protocol_len = strlen(auth_protocol);
	int data_len = auth_data_len * 2;
	unsigned char FAR *outmsg =
		begin_send_packet(pvar, SSH_CMSG_X11_REQUEST_FORWARDING,
						  12 + protocol_len + data_len);
	int i;
	char FAR *auth_data_ptr;

	set_uint32(outmsg, protocol_len);
	memcpy(outmsg + 4, auth_protocol, protocol_len);
	set_uint32(outmsg + 4 + protocol_len, data_len);
	auth_data_ptr = outmsg + 8 + protocol_len;
	for (i = 0; i < auth_data_len; i++) {
		sprintf(auth_data_ptr + i * 2, "%.2x", auth_data[i]);
	}
	set_uint32(outmsg + 8 + protocol_len + data_len, screen_num);

	finish_send_packet(pvar);

	enque_forwarding_request_handlers(pvar);
}

void SSH_open_channel(PTInstVar pvar, uint32 local_channel_num,
					  char FAR * to_remote_host, int to_remote_port,
					  char FAR * originator, unsigned short originator_port)
{
	static const int msgs[]
	= { SSH_MSG_CHANNEL_OPEN_CONFIRMATION, SSH_MSG_CHANNEL_OPEN_FAILURE };
	static const SSHPacketHandler handlers[]
	= { handle_channel_open_confirmation, handle_channel_open_failure };

	int host_len = strlen(to_remote_host);

	if ((pvar->ssh_state.
		 server_protocol_flags & SSH_PROTOFLAG_HOST_IN_FWD_OPEN) != 0) {
		int originator_len = strlen(originator);
		unsigned char FAR *outmsg =
			begin_send_packet(pvar, SSH_MSG_PORT_OPEN,
							  16 + host_len + originator_len);

		set_uint32(outmsg, local_channel_num);
		set_uint32(outmsg + 4, host_len);
		memcpy(outmsg + 8, to_remote_host, host_len);
		set_uint32(outmsg + 8 + host_len, to_remote_port);
		set_uint32(outmsg + 12 + host_len, originator_len);
		memcpy(outmsg + 16 + host_len, originator, originator_len);
	} else {

		if (SSHv1(pvar)) {
			unsigned char FAR *outmsg =
				begin_send_packet(pvar, SSH_MSG_PORT_OPEN,
								12 + host_len);

			set_uint32(outmsg, local_channel_num);
			set_uint32(outmsg + 4, host_len);
			memcpy(outmsg + 8, to_remote_host, host_len);
			set_uint32(outmsg + 8 + host_len, to_remote_port);

		} else {
			// SSH2 port-fowarding (2005.2.26 yutaka)
			buffer_t *msg;
			char *s;
			unsigned char *outmsg;
			int len;
			Channel_t *c;

			// SSH2鍵交換中の場合、パケットを捨てる。(2005.6.21 yutaka)
			if (pvar->rekeying) {
				// TODO: 理想としてはパケット破棄ではなく、パケット読み取り遅延にしたいところだが、
				// 将来直すことにする。
				c = NULL;

				return;
			}

			c = ssh2_channel_new(CHAN_SES_WINDOW_DEFAULT, CHAN_SES_PACKET_DEFAULT, TYPE_PORTFWD, local_channel_num);

			msg = buffer_init();
			if (msg == NULL) {
				// TODO: error check
				return;
			}
			s = "direct-tcpip";
			buffer_put_string(msg, s, strlen(s)); // ctype
			buffer_put_int(msg, c->self_id);  // self
			buffer_put_int(msg, c->local_window);  // local_window
			buffer_put_int(msg, c->local_maxpacket);  // local_maxpacket

			s = to_remote_host;
			buffer_put_string(msg, s, strlen(s)); // target host
			buffer_put_int(msg, to_remote_port);  // target port

			s = originator;
			buffer_put_string(msg, s, strlen(s)); // originator host
			buffer_put_int(msg, originator_port);  // originator port

			len = buffer_len(msg);
			outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_OPEN, len);
			memcpy(outmsg, buffer_ptr(msg), len);
			finish_send_packet(pvar);
			buffer_free(msg);

			return;

			/* NOT REACHED */
		}

	}

	if (SSHv1(pvar)) { // SSH1のみ
		finish_send_packet(pvar);
		enque_handlers(pvar, 2, msgs, handlers);
	}

}


/////////////////////////////////////////////////////////////////////////////
//
// SSH2 protocol procedure in the following code:
//
/////////////////////////////////////////////////////////////////////////////

void debug_print(int no, char *msg, int len)
{
#ifdef _DEBUG
	FILE *fp;
	char file[128];

	sprintf(file, "dump%d.bin", no);

	fp = fopen(file, "wb");
	if (fp == NULL) 
		return;

	fwrite(msg, 1, len, fp);

	fclose(fp);
#endif
}

// クライアントからサーバへの提案事項
#ifdef SSH2_DEBUG
static char *myproposal[PROPOSAL_MAX] = {
//	"diffie-hellman-group-exchange-sha1,diffie-hellman-group14-sha1,diffie-hellman-group1-sha1",
	"diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1",
//	"ssh-rsa,ssh-dss",
	"ssh-dss,ssh-rsa",
	"3des-cbc,aes128-cbc",
	"3des-cbc,aes128-cbc",
	"hmac-md5,hmac-sha1",
	"hmac-md5,hmac-sha1",
//	"hmac-sha1,hmac-md5",
//	"hmac-sha1,hmac-md5",
//	"hmac-sha1",
//	"hmac-sha1",
	"none",
	"none",
	"",
	"",
};
#else
static char *myproposal[PROPOSAL_MAX] = {
	"diffie-hellman-group-exchange-sha1,diffie-hellman-group14-sha1,diffie-hellman-group1-sha1",
	"ssh-rsa,ssh-dss",
	"3des-cbc,aes128-cbc",
	"3des-cbc,aes128-cbc",
	"hmac-sha1,hmac-md5",
	"hmac-sha1,hmac-md5",
	"none",
	"none",
	"",
	"",
};
#endif


typedef struct ssh2_cipher {
	SSHCipher cipher;
	char *name;
	int block_size;
	int key_len;
	const EVP_CIPHER *(*func)(void);
} ssh2_cipher_t;

ssh2_cipher_t ssh2_ciphers[] = {
	{SSH_CIPHER_3DES_CBC, "3des-cbc", 8, 24, EVP_des_ede3_cbc},
	{SSH_CIPHER_AES128, "aes128-cbc", 16, 16, EVP_aes_128_cbc},
	{SSH_CIPHER_NONE, NULL, 0, 0, NULL},
};


typedef struct ssh2_mac {
	char *name;
	const EVP_MD *(*func)(void);
	int truncatebits;
} ssh2_mac_t;

ssh2_mac_t ssh2_macs[] = {
	{"hmac-sha1", EVP_sha1, 0},
	{"hmac-md5", EVP_md5, 0},
	{NULL, NULL, 0},
};

static Newkeys current_keys[MODE_MAX];


#define write_buffer_file(buf,len) do_write_buffer_file(buf,len,__FILE__,__LINE__)


//
// general
//

static int get_cipher_block_size(SSHCipher cipher) 
{
	ssh2_cipher_t *ptr = ssh2_ciphers;
	int val = 0;

	while (ptr->name != NULL) {
		if (cipher == ptr->cipher) {
			val = ptr->block_size;
			break;
		}
		ptr++;
	}
	return (val);
}

static int get_cipher_key_len(SSHCipher cipher) 
{
	ssh2_cipher_t *ptr = ssh2_ciphers;
	int val = 0;

	while (ptr->name != NULL) {
		if (cipher == ptr->cipher) {
			val = ptr->key_len;
			break;
		}
		ptr++;
	}
	return (val);
}


#if 0
static int get_mac_index(char *name) 
{
	ssh2_mac_t *ptr = ssh2_macs;
	int val = -1;

	while (ptr->name != NULL) {
		if (strcmp(ptr->name, name) == 0) {
			val = ptr - ssh2_macs;
			break;
		}
		ptr++;
	}
	return (val);
}
#endif


static void do_write_buffer_file(void *buf, int len, char *file, int lineno)
{
	FILE *fp;
	char filename[256];

	_snprintf(filename, sizeof(filename), "data%d.bin", lineno);

	fp = fopen(filename, "wb");
	if (fp == NULL)
		return;

	fwrite(buf, 1, len, fp);

	fclose(fp);
}


void SSH2_packet_start(buffer_t *msg, unsigned char type)
{
	unsigned char buf[9];
	int len = 6;

	memset(buf, 0, sizeof(buf));
	buf[len - 1] = type;
	buffer_clear(msg);
	buffer_append(msg, buf, len);
}


// the caller is normalize_cipher_order()
void SSH2_update_cipher_myproposal(PTInstVar pvar)
{
	static char buf[128]; // TODO: malloc()にすべき
	int cipher;
	int len, i;

	// 暗号アルゴリズム優先順位に応じて、myproposal[]を書き換える。(2004.11.6 yutaka)
	buf[0] = '\0';
	for (i = 0 ; pvar->ts_SSH->CipherOrder[i] != 0 ; i++) {
		cipher = pvar->ts_SSH->CipherOrder[i] - '0';
		if (cipher == 0) // disabled line
			break;
		if (cipher == SSH_CIPHER_AES128) {
			strcat(buf, "aes128-cbc,");
		}
		if (cipher == SSH_CIPHER_3DES_CBC) {
			strcat(buf, "3des-cbc,");
		}
	}
	if (buf[0] != '\0') {
		len = strlen(buf);
		buf[len - 1] = '\0';  // get rid of comma
		myproposal[PROPOSAL_ENC_ALGS_CTOS] = buf;  // Client To Server
		myproposal[PROPOSAL_ENC_ALGS_STOC] = buf;  // Server To Client
	}
}


// クライアントからサーバへのキー交換開始要求
void SSH2_send_kexinit(PTInstVar pvar)
{
	char cookie[SSH2_COOKIE_LENGTH];
	buffer_t *msg;
	unsigned char *outmsg;
	int len, i;

	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return;
	}
	if (pvar->my_kex != NULL)
		buffer_free(pvar->my_kex);
	pvar->my_kex = msg;

	// メッセージタイプ
	//SSH2_packet_start(msg, SSH2_MSG_KEXINIT);

	// cookieのセット
	CRYPT_set_random_data(pvar, cookie, sizeof(cookie));
	CRYPT_set_server_cookie(pvar, cookie);
	buffer_append(msg, cookie, sizeof(cookie));

	// クライアントのキー情報
	for (i = 0 ; i < PROPOSAL_MAX ; i++) {
		buffer_put_string(msg, myproposal[i], strlen(myproposal[i]));
	}
	buffer_put_char(msg, 0);
	buffer_put_int(msg, 0);

	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_KEXINIT, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);

	// my_kexに取っておくため、フリーしてはいけない。
	//buffer_free(msg);

	notify_verbose_message(pvar, "SSH2_MSG_KEXINIT was sent", LOG_LEVEL_VERBOSE);
}


static SSHCipher choose_SSH2_cipher_algorithm(char *server_proposal, char *my_proposal)
{
	char tmp[1024], *ptr;
	SSHCipher cipher = SSH_CIPHER_NONE;

	_snprintf(tmp, sizeof(tmp), my_proposal);
	ptr = strtok(tmp, ","); // not thread-safe
	while (ptr != NULL) {
		// server_proposalにはサーバのproposalがカンマ文字列で格納されている
		if (strstr(server_proposal, ptr)) { // match
			break;
		}
		ptr = strtok(NULL, ",");
	}
	if (strstr(ptr, "3des-cbc")) {
		cipher = SSH_CIPHER_3DES_CBC;

	} else if (strstr(ptr, "aes128-cbc")) {
		cipher = SSH_CIPHER_AES128;
	}

	return (cipher);
}


static enum hmac_type choose_SSH2_hmac_algorithm(char *server_proposal, char *my_proposal)
{
	char tmp[1024], *ptr;
	enum hmac_type type = HMAC_UNKNOWN;

	_snprintf(tmp, sizeof(tmp), my_proposal);
	ptr = strtok(tmp, ","); // not thread-safe
	while (ptr != NULL) {
		// server_proposalにはサーバのproposalがカンマ文字列で格納されている
		if (strstr(server_proposal, ptr)) { // match
			break;
		}
		ptr = strtok(NULL, ",");
	}
	if (strstr(ptr, "hmac-sha1")) {
		type = HMAC_SHA1;
	} else if (strstr(ptr, "hmac-md5")) {
		type = HMAC_MD5;
	}

	return (type);
}


// 暗号アルゴリズムのキーサイズ、ブロックサイズ、MACサイズのうち最大値(we_need)を決定する。
static void choose_SSH2_key_maxlength(PTInstVar pvar)
{
	int mode, need, val, ctos;
	const EVP_MD *md;

	for (mode = 0; mode < MODE_MAX; mode++) {
		if (mode == MODE_OUT)
			ctos = 1;
		else
			ctos = 0;

		if (ctos == 1) {
			val = pvar->ctos_hmac;
		} else {
			val = pvar->stoc_hmac;
		}

		// current_keys[]に設定しておいて、あとで pvar->ssh2_keys[] へコピーする。
		md = ssh2_macs[val].func();
		current_keys[mode].mac.md = md;
		current_keys[mode].mac.key_len = current_keys[mode].mac.mac_len = EVP_MD_size(md);
		if (ssh2_macs[val].truncatebits != 0) {
			current_keys[mode].mac.mac_len = ssh2_macs[val].truncatebits / 8;
		}

		// キーサイズとブロックサイズもここで設定しておく (2004.11.7 yutaka)
		if (ctos == 1) {
			current_keys[mode].enc.key_len = get_cipher_key_len(pvar->ctos_cipher);
			current_keys[mode].enc.block_size = get_cipher_block_size(pvar->ctos_cipher);
		} else {
			current_keys[mode].enc.key_len = get_cipher_key_len(pvar->stoc_cipher);
			current_keys[mode].enc.block_size = get_cipher_block_size(pvar->stoc_cipher);
		}
		current_keys[mode].mac.enabled = 0;

		// 現時点ではMACはdisable
		pvar->ssh2_keys[mode].mac.enabled = 0;
	}
	need = 0;
	for (mode = 0; mode < MODE_MAX; mode++) {
		if (mode == MODE_OUT)
			ctos = 1;
		else
			ctos = 0;

	 	val = current_keys[mode].enc.key_len;
		if (need < val)
			need = val;

	 	val = current_keys[mode].enc.block_size;
		if (need < val)
			need = val;

		val = current_keys[mode].mac.key_len;
		if (need < val)
			need = val;
	}
	pvar->we_need = need;

}


// キー交換開始前のチェック (SSH2_MSG_KEXINIT)
// ※当該関数はデータ通信中にも呼ばれてくる可能性あり
static BOOL handle_SSH2_kexinit(PTInstVar pvar)
{
	char buf[1024];
	char *data;
	int len, i, size;
	int offset = 0;
	char *msg = NULL;
	char tmp[1024+512];
	char *ptr;

	notify_verbose_message(pvar, "SSH2_MSG_KEXINIT is receiving", LOG_LEVEL_VERBOSE);

	// すでにキー交換が終わっているにも関わらず、サーバから SSH2_MSG_KEXINIT が
	// 送られてくる場合は、キー再作成を行う。(2004.10.24 yutaka)
	if (pvar->key_done == 1) {
		pvar->rekeying = 1;
		pvar->key_done = 0;

		// サーバへSSH2_MSG_KEXINIT を送る
		SSH2_send_kexinit(pvar);
	}

	if (pvar->peer_kex != NULL) { // already allocated
		buffer_free(pvar->peer_kex);
	}
	pvar->peer_kex = buffer_init();
	if (pvar->peer_kex == NULL) {
		msg = "Out of memory @ handle_SSH2_kexinit()";
		goto error;
	}
	// [-2]:padding size
//	len = pvar->ssh_state.payloadlen + pvar->ssh_state.payload[-2] + 1;
	len = pvar->ssh_state.payloadlen - 1;
//	buffer_append(pvar->peer_kex, &pvar->ssh_state.payload[-6], len);
	buffer_append(pvar->peer_kex, pvar->ssh_state.payload, len);
	//write_buffer_file(&pvar->ssh_state.payload[-6], len);

	// TODO: buffer overrun check

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディング+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	//write_buffer_file(data, len);
	push_memdump("KEXINIT", "アルゴリズムリストの交換", data, len);

	if (offset + 20 >= len) {
		msg = "payload size too small @ handle_SSH2_kexinit()";
		goto error;
	}

#if 0
	for (i = 0; i < SSH2_COOKIE_LENGTH; i++) {
		cookie[i] = data[i];
	}
#endif
	// get rid of Cookie length
	offset += SSH2_COOKIE_LENGTH;

	// キー交換アルゴリズムチェック
	size = get_payload_uint32(pvar, offset);
	offset += 4;
	for (i = 0; i < size; i++) {
		buf[i] = data[offset + i];
	}
	buf[i] = '\0'; // null-terminate
	offset += size;

	// KEXの決定。判定順をmyproposal[PROPOSAL_KEX_ALGS]の並びと合わせること。
	// サーバは、クライアントから送られてきた myproposal[PROPOSAL_KEX_ALGS] のカンマ文字列のうち、
	// 先頭から自分の myproposal[] と比較を行い、最初にマッチしたものがKEXアルゴリズムとして
	// 選択される。(2004.10.30 yutaka)
	pvar->kex_type = -1;
	_snprintf(tmp, sizeof(tmp), myproposal[PROPOSAL_KEX_ALGS]);
	ptr = strtok(tmp, ","); // not thread-safe
	while (ptr != NULL) {
		// buf[]にはサーバのproposalがカンマ文字列で格納されている
		if (strstr(buf, ptr)) { // match
			break;
		}
		ptr = strtok(NULL, ",");
	}
	if (ptr == NULL) { // not match
		strcpy(tmp, "unknown KEX algorithm: ");
		strcat(tmp, buf);
		msg = tmp;
		goto error;
	}
	if (strstr(ptr, KEX_DH14)) {
		pvar->kex_type = KEX_DH_GRP14_SHA1;
	} else if (strstr(ptr, KEX_DH1)) {
		pvar->kex_type = KEX_DH_GRP1_SHA1;
	} else if (strstr(ptr, KEX_DHGEX)) {
		pvar->kex_type = KEX_DH_GEX_SHA1;
	}


	// ホストキーアルゴリズムチェック
	size = get_payload_uint32(pvar, offset);
	offset += 4;
	for (i = 0; i < size; i++) {
		buf[i] = data[offset + i];
	}
	buf[i] = 0;
	offset += size;
	pvar->hostkey_type = -1;
	_snprintf(tmp, sizeof(tmp), myproposal[PROPOSAL_SERVER_HOST_KEY_ALGS]);
	ptr = strtok(tmp, ","); // not thread-safe
	while (ptr != NULL) {
		// buf[]にはサーバのproposalがカンマ文字列で格納されている
		if (strstr(buf, ptr)) { // match
			break;
		}
		ptr = strtok(NULL, ",");
	}
	if (ptr == NULL) { // not match
		strcpy(tmp, "unknown host KEY type: ");
		strcat(tmp, buf);
		msg = tmp;
		goto error;
	}
	if (strstr(ptr, "ssh-rsa")) {
		pvar->hostkey_type = KEY_RSA;
	} else if (strstr(ptr, "ssh-dss")) {
		pvar->hostkey_type = KEY_DSA;
	} else 	if (strstr(ptr, "rsa1")) {
		pvar->hostkey_type = KEY_RSA1;
	} else if (strstr(ptr, "rsa")) {
		pvar->hostkey_type = KEY_RSA;
	} else if (strstr(ptr, "dsa")) {
		pvar->hostkey_type = KEY_DSA;
	}
#if 0
	// TODO: 現状は KEY_RSA しかサポートしていない (2004.10.24 yutaka)
	if (pvar->hostkey_type != KEY_RSA) {
		strcpy(tmp, "unknown KEY type: ");
		strcat(tmp, buf);
		msg = tmp;
		goto error;
	}
#endif

	// クライアント -> サーバ暗号アルゴリズムチェック
	size = get_payload_uint32(pvar, offset);
	offset += 4;
	for (i = 0; i < size; i++) {
		buf[i] = data[offset + i];
	}
	buf[i] = 0;
	offset += size;
	pvar->ctos_cipher = choose_SSH2_cipher_algorithm(buf, myproposal[PROPOSAL_ENC_ALGS_CTOS]);
	if (pvar->ctos_cipher == SSH_CIPHER_NONE) {
		strcpy(tmp, "unknown Encrypt algorithm(ctos): ");
		strcat(tmp, buf);
		msg = tmp;
		goto error;
	}

	// サーバ -> クライアント暗号アルゴリズムチェック
	size = get_payload_uint32(pvar, offset);
	offset += 4;
	for (i = 0; i < size; i++) {
		buf[i] = data[offset + i];
	}
	buf[i] = 0;
	offset += size;
	pvar->stoc_cipher = choose_SSH2_cipher_algorithm(buf, myproposal[PROPOSAL_ENC_ALGS_STOC]);
	if (pvar->stoc_cipher == SSH_CIPHER_NONE) {
		strcpy(tmp, "unknown Encrypt algorithm(stoc): ");
		strcat(tmp, buf);
		msg = tmp;
		goto error;
	}


	// HMAC(Hash Message Authentication Code)アルゴリズムの決定 (2004.12.17 yutaka)
	size = get_payload_uint32(pvar, offset);
	offset += 4;
	for (i = 0; i < size; i++) {
		buf[i] = data[offset + i];
	}
	buf[i] = 0;
	offset += size;
	pvar->ctos_hmac = choose_SSH2_hmac_algorithm(buf, myproposal[PROPOSAL_MAC_ALGS_CTOS]);
	if (pvar->ctos_hmac == HMAC_UNKNOWN) { // not match
		strcpy(tmp, "unknown HMAC algorithm: ");
		strcat(tmp, buf);
		msg = tmp;
		goto error;
	}

	size = get_payload_uint32(pvar, offset);
	offset += 4;
	for (i = 0; i < size; i++) {
		buf[i] = data[offset + i];
	}
	buf[i] = 0;
	offset += size;
	pvar->stoc_hmac = choose_SSH2_hmac_algorithm(buf, myproposal[PROPOSAL_MAC_ALGS_STOC]);
	if (pvar->ctos_hmac == HMAC_UNKNOWN) { // not match
		strcpy(tmp, "unknown HMAC algorithm: ");
		strcat(tmp, buf);
		msg = tmp;
		goto error;
	}


	// we_needの決定 (2004.11.6 yutaka)
	// キー再作成の場合はスキップする。
	if (pvar->rekeying == 0) {
		choose_SSH2_key_maxlength(pvar);
	}

	// send DH kex init
	if (pvar->kex_type == KEX_DH_GRP1_SHA1) {
		SSH2_dh_kex_init(pvar);

	} else if (pvar->kex_type == KEX_DH_GRP14_SHA1) {
		SSH2_dh_kex_init(pvar);

	} else if (pvar->kex_type == KEX_DH_GEX_SHA1) {
		SSH2_dh_gex_kex_init(pvar);

	}

	return TRUE;

error:;
	buffer_free(pvar->peer_kex);

	notify_fatal_error(pvar, msg);

	return FALSE;
}



static DH *dh_new_group_asc(const char *gen, const char *modulus)
{
	DH *dh = NULL;

	if ((dh = DH_new()) == NULL) {
		printf("dh_new_group_asc: DH_new");
		goto error;
	}

	// PとGは公開してもよい素数の組み合わせ
	if (BN_hex2bn(&dh->p, modulus) == 0) {
		printf("BN_hex2bn p");
		goto error;
	}

	if (BN_hex2bn(&dh->g, gen) == 0) {
		printf("BN_hex2bn g");
		goto error;
	}

	return (dh);

error:
	DH_free(dh);
	return (NULL);
}


static DH *dh_new_group1(void)
{
	static char *gen = "2", *group1 =
	    "FFFFFFFF" "FFFFFFFF" "C90FDAA2" "2168C234" "C4C6628B" "80DC1CD1"
	    "29024E08" "8A67CC74" "020BBEA6" "3B139B22" "514A0879" "8E3404DD"
	    "EF9519B3" "CD3A431B" "302B0A6D" "F25F1437" "4FE1356D" "6D51C245"
	    "E485B576" "625E7EC6" "F44C42E9" "A637ED6B" "0BFF5CB6" "F406B7ED"
	    "EE386BFB" "5A899FA5" "AE9F2411" "7C4B1FE6" "49286651" "ECE65381"
	    "FFFFFFFF" "FFFFFFFF";

	return (dh_new_group_asc(gen, group1));
}


static DH *dh_new_group14(void)
{
    static char *gen = "2", *group14 =        
		"FFFFFFFF" "FFFFFFFF" "C90FDAA2" "2168C234" "C4C6628B" "80DC1CD1"
        "29024E08" "8A67CC74" "020BBEA6" "3B139B22" "514A0879" "8E3404DD"        
		"EF9519B3" "CD3A431B" "302B0A6D" "F25F1437" "4FE1356D" "6D51C245"
        "E485B576" "625E7EC6" "F44C42E9" "A637ED6B" "0BFF5CB6" "F406B7ED"
        "EE386BFB" "5A899FA5" "AE9F2411" "7C4B1FE6" "49286651" "ECE45B3D"
        "C2007CB8" "A163BF05" "98DA4836" "1C55D39A" "69163FA8" "FD24CF5F"
        "83655D23" "DCA3AD96" "1C62F356" "208552BB" "9ED52907" "7096966D"
        "670C354E" "4ABC9804" "F1746C08" "CA18217C" "32905E46" "2E36CE3B"
        "E39E772C" "180E8603" "9B2783A2" "EC07A28F" "B5C55DF0" "6F4C52C9"
        "DE2BCBF6" "95581718" "3995497C" "EA956AE5" "15D22618" "98FA0510"
        "15728E5A" "8AACAA68" "FFFFFFFF" "FFFFFFFF";

	return (dh_new_group_asc(gen, group14));
}


// DH鍵を生成する
static void dh_gen_key(PTInstVar pvar, DH *dh, int we_need /* bytes */ )
{
	int i;

	dh->priv_key = NULL;

	// 秘密にすべき乱数(X)を生成
	for (i = 0 ; i < 10 ; i++) { // retry counter
		if (dh->priv_key != NULL) {
			BN_clear_free(dh->priv_key);
		}
		dh->priv_key = BN_new();
		if (dh->priv_key == NULL)
			goto error;
		if (BN_rand(dh->priv_key, 2*(we_need*8), 0, 0) == 0)
			goto error;
		if (DH_generate_key(dh) == 0)
			goto error;
		if (dh_pub_is_valid(dh, dh->pub_key))
			break;
	}
	if (i >= 10) {
		goto error;
	}
	return;

error:;
	notify_fatal_error(pvar, "error occurred @ dh_gen_key()");

}

//
// KEX_DH_GRP1_SHA1 or KEX_DH_GRP14_SHA1
//
static void SSH2_dh_kex_init(PTInstVar pvar)
{
	DH *dh = NULL;
	char *s, *t;
	buffer_t *msg = NULL;
	unsigned char *outmsg;
	int len;

	// Diffie-Hellman key agreement
	if (pvar->kex_type == KEX_DH_GRP1_SHA1) {
		dh = dh_new_group1();
	} else if (pvar->kex_type == KEX_DH_GRP14_SHA1) {
		dh = dh_new_group14();
	} else {
		goto error;
	}

	// 秘密にすべき乱数(X)を生成
	dh_gen_key(pvar, dh, pvar->we_need);

	s = BN_bn2hex(dh->priv_key);
	t = BN_bn2hex(dh->pub_key);


	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return;
	}

	buffer_put_bignum2(msg, dh->pub_key);

	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_KEXDH_INIT, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);

	if (pvar->kexdh != NULL) {
		DH_free(dh);
	}
	pvar->kexdh = dh;

	SSH2_dispatch_init(2);
	SSH2_dispatch_add_message(SSH2_MSG_KEXDH_REPLY);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.5 yutaka)

	buffer_free(msg);

	notify_verbose_message(pvar, "SSH2_MSG_KEXDH_INIT was sent", LOG_LEVEL_VERBOSE);

	return;

error:;
	DH_free(dh);
	buffer_free(msg);

	notify_fatal_error(pvar, "error occurred @ SSH2_dh_kex_init()");
}



//
// KEX_DH_GEX_SHA1
// 
// cf.  Diffie-Hellman Group Exchange for the SSH Transport Layer Protocol
//      (draft-ietf-secsh-dh-group-exchange-04.txt)
//

static int dh_estimate(int bits)
{
    if (bits <= 128)
        return (1024);  /* O(2**86) */
    if (bits <= 192) 
        return (2048);  /* O(2**116) */
    return (4096);      /* O(2**156) */
}       

static void SSH2_dh_gex_kex_init(PTInstVar pvar)
{
	buffer_t *msg = NULL;
	unsigned char *outmsg;
	int len;
	int bits, min, max;

	msg = buffer_init();
	if (msg == NULL) {
		goto error;
	}

	// サーバが保証すべき最低限のビット数を求める（we_needはバイト）。
	bits = dh_estimate(pvar->we_need * 8); 
	min = 1024;
	max = 8192;

	// サーバへgroup sizeを送って、p と g を作ってもらう。
	buffer_put_int(msg, min);
	buffer_put_int(msg, bits);
	buffer_put_int(msg, max);
	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_KEX_DH_GEX_REQUEST, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);

	// あとでハッシュ計算に使うので取っておく。
	pvar->kexgex_min = min;
	pvar->kexgex_bits = bits;
	pvar->kexgex_max = max;

	{
		char tmp[128];
		_snprintf(tmp, sizeof(tmp), "we_need %d min %d bits %d max %d", 
			pvar->we_need, min, bits, max);
		push_memdump("DH_GEX_REQUEST", "requested key bits", tmp, strlen(tmp));
	}

	SSH2_dispatch_init(2);
	SSH2_dispatch_add_message(SSH2_MSG_KEX_DH_GEX_GROUP);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.5 yutaka)

	buffer_free(msg);

	notify_verbose_message(pvar, "SSH2_MSG_KEX_DH_GEX_REQUEST was sent", LOG_LEVEL_VERBOSE);

	return;

error:;
	buffer_free(msg);

	notify_fatal_error(pvar, "error occurred @ SSH2_dh_gex_kex_init()");
}


// SSH2_MSG_KEX_DH_GEX_GROUP
static BOOL handle_SSH2_dh_gex_group(PTInstVar pvar)
{
	char *data;
	int len;
	BIGNUM *p = NULL, *g = NULL;
	DH *dh = NULL;
	buffer_t *msg = NULL;
	unsigned char *outmsg;

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	p = BN_new();
	g = BN_new();
	if (p == NULL || g == NULL)
		goto error;

	buffer_get_bignum2(&data, p); // 素数の取得
	buffer_get_bignum2(&data, g); // 生成元の取得

	dh = DH_new();
	if (dh == NULL)
		goto error;
	dh->p = p;
	dh->g = g;

	// 秘密にすべき乱数(X)を生成
	dh_gen_key(pvar, dh, pvar->we_need);

	// 公開鍵をサーバへ送信
	msg = buffer_init();
	if (msg == NULL) {
		goto error;
	}
	buffer_put_bignum2(msg, dh->pub_key);
	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_KEX_DH_GEX_INIT, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);

	// ここで作成したDH鍵は、あとでハッシュ計算に使うため取っておく。(2004.10.31 yutaka)
	pvar->kexdh = dh;

	{
		push_bignum_memdump("DH_GEX_GROUP", "p", dh->p);
		push_bignum_memdump("DH_GEX_GROUP", "g", dh->g);
		push_bignum_memdump("DH_GEX_GROUP", "pub_key", dh->pub_key);
	}

	SSH2_dispatch_init(2);
	SSH2_dispatch_add_message(SSH2_MSG_KEX_DH_GEX_REPLY);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.5 yutaka)

	buffer_free(msg);

	return TRUE;

error:;
	BN_free(p);
	BN_free(g);
	DH_free(dh);

	return FALSE;
}



// SHA-1(160bit)を求める
unsigned char *kex_dh_hash(
    char *client_version_string,
    char *server_version_string,
    char *ckexinit, int ckexinitlen,
    char *skexinit, int skexinitlen,
    u_char *serverhostkeyblob, int sbloblen,
    BIGNUM *client_dh_pub,
    BIGNUM *server_dh_pub,
    BIGNUM *shared_secret)
{
	buffer_t *b;
	static unsigned char digest[EVP_MAX_MD_SIZE];
	const EVP_MD *evp_md = EVP_sha1();
	EVP_MD_CTX md;

	b = buffer_init();
	buffer_put_string(b, client_version_string, strlen(client_version_string));
	buffer_put_string(b, server_version_string, strlen(server_version_string));

	/* kexinit messages: fake header: len+SSH2_MSG_KEXINIT */
	buffer_put_int(b, ckexinitlen+1);
	buffer_put_char(b, SSH2_MSG_KEXINIT);
	buffer_append(b, ckexinit, ckexinitlen);
	buffer_put_int(b, skexinitlen+1);
	buffer_put_char(b, SSH2_MSG_KEXINIT);
	buffer_append(b, skexinit, skexinitlen);

	buffer_put_string(b, serverhostkeyblob, sbloblen);
	buffer_put_bignum2(b, client_dh_pub);
	buffer_put_bignum2(b, server_dh_pub);
	buffer_put_bignum2(b, shared_secret);

	// yutaka
	//debug_print(38, buffer_ptr(b), buffer_len(b));

	EVP_DigestInit(&md, evp_md);
	EVP_DigestUpdate(&md, buffer_ptr(b), buffer_len(b));
	EVP_DigestFinal(&md, digest, NULL);

	buffer_free(b);

	//write_buffer_file(digest, EVP_MD_size(evp_md));

	return digest;
}


int dh_pub_is_valid(DH *dh, BIGNUM *dh_pub)
{
	int i;
	int n = BN_num_bits(dh_pub);
	int bits_set = 0;

	if (dh_pub->neg) {
		//logit("invalid public DH value: negativ");
		return 0;
	}
	for (i = 0; i <= n; i++)
		if (BN_is_bit_set(dh_pub, i))
			bits_set++;
	//debug2("bits set: %d/%d", bits_set, BN_num_bits(dh->p));

	/* if g==2 and bits_set==1 then computing log_g(dh_pub) is trivial */
	if (bits_set > 1 && (BN_cmp(dh_pub, dh->p) == -1))
		return 1;
	//logit("invalid public DH value (%d/%d)", bits_set, BN_num_bits(dh->p));
	return 0;
}


static u_char *derive_key(int id, int need, u_char *hash, BIGNUM *shared_secret, char *session_id, int session_id_len)
{
	buffer_t *b;
	const EVP_MD *evp_md = EVP_sha1();
	EVP_MD_CTX md;
	char c = id;
	int have;
	int mdsz = EVP_MD_size(evp_md); // 20bytes(160bit)
	u_char *digest = malloc(roundup(need, mdsz));

	if (digest == NULL)
		goto skip;

	b = buffer_init();
	if (b == NULL)
		goto skip;

	buffer_put_bignum2(b, shared_secret);

	/* K1 = HASH(K || H || "A" || session_id) */
	EVP_DigestInit(&md, evp_md);
	EVP_DigestUpdate(&md, buffer_ptr(b), buffer_len(b));
	EVP_DigestUpdate(&md, hash, mdsz);
	EVP_DigestUpdate(&md, &c, 1);
	EVP_DigestUpdate(&md, session_id, session_id_len);
	EVP_DigestFinal(&md, digest, NULL);

	/*
	 * expand key:
	 * Kn = HASH(K || H || K1 || K2 || ... || Kn-1)
	 * Key = K1 || K2 || ... || Kn
	 */
	for (have = mdsz; need > have; have += mdsz) {
		EVP_DigestInit(&md, evp_md);
		EVP_DigestUpdate(&md, buffer_ptr(b), buffer_len(b));
		EVP_DigestUpdate(&md, hash, mdsz);
		EVP_DigestUpdate(&md, digest, have);
		EVP_DigestFinal(&md, digest + have, NULL);
	}
	buffer_free(b);

skip:;
	return digest;
}


static void ssh2_set_newkeys(PTInstVar pvar, int mode)
{
#if 1
	// free already allocated buffer
	if (pvar->ssh2_keys[mode].enc.iv != NULL) {
		free(pvar->ssh2_keys[mode].enc.iv);
	}
	if (pvar->ssh2_keys[mode].enc.key != NULL) {
		free(pvar->ssh2_keys[mode].enc.key);
	}
	if (pvar->ssh2_keys[mode].mac.key != NULL) {
		free(pvar->ssh2_keys[mode].mac.key);
	}
#endif

	pvar->ssh2_keys[mode] = current_keys[mode];
}


void kex_derive_keys(PTInstVar pvar, int need, u_char *hash, BIGNUM *shared_secret, char *session_id, int session_id_len)
{
#define NKEYS	6
	u_char *keys[NKEYS];
	int i, mode, ctos;

	for (i = 0; i < NKEYS; i++) {
		keys[i] = derive_key('A'+i, need, hash, shared_secret, session_id, session_id_len);
		//debug_print(i, keys[i], need);
	}

	for (mode = 0; mode < MODE_MAX; mode++) {
		if (mode == MODE_OUT)
			ctos = 1;
		else
			ctos = 0;

#if 0
		// free already allocated buffer (2004.12.27 yutaka)
		// キー再作成時にMAC corruptとなるので削除。(2005.1.5 yutaka)
		if (current_keys[mode].enc.iv != NULL)
			free(current_keys[mode].enc.iv);
		if (current_keys[mode].enc.key != NULL)
			free(current_keys[mode].enc.key);
		if (current_keys[mode].mac.key != NULL)
			free(current_keys[mode].mac.key);
#endif

		// setting
		current_keys[mode].enc.iv  = keys[ctos ? 0 : 1];
		current_keys[mode].enc.key = keys[ctos ? 2 : 3];
		current_keys[mode].mac.key = keys[ctos ? 4 : 5];

		//debug_print(20 + mode*3, current_keys[mode]->enc.iv, 8);
		//debug_print(21 + mode*3, current_keys[mode]->enc.key, 24);
		//debug_print(22 + mode*3, current_keys[mode]->mac.key, 24);
	}
}


//////////////////////////////////////////////////////////////////////////////
//
// Key verify function
// 
//////////////////////////////////////////////////////////////////////////////

//
// DSS
// 

static int ssh_dss_verify(
			DSA *key, u_char *signature, u_int signaturelen,
			u_char *data, u_int datalen)
{
	DSA_SIG *sig;
	const EVP_MD *evp_md = EVP_sha1();
	EVP_MD_CTX md;
	unsigned char digest[EVP_MAX_MD_SIZE], *sigblob;
	unsigned int len, dlen;
	int ret;
	char *ptr;

	OpenSSL_add_all_digests();

	if (key == NULL) {
		return -2;
	}

	ptr = signature;

	// step1
	len = get_uint32_MSBfirst(ptr);
	ptr += 4;
	if (strncmp("ssh-dss", ptr, len) != 0) {
		return -3;
	}
	ptr += len;

	// step2
	len = get_uint32_MSBfirst(ptr);
	ptr += 4;
	sigblob = ptr;
	ptr += len;

	if (len != SIGBLOB_LEN) {
		return -4;
	}

	/* parse signature */
	if ((sig = DSA_SIG_new()) == NULL)
		return -5;
	if ((sig->r = BN_new()) == NULL)
		return -6;
	if ((sig->s = BN_new()) == NULL)
		return -7;
	BN_bin2bn(sigblob, INTBLOB_LEN, sig->r);
	BN_bin2bn(sigblob+ INTBLOB_LEN, INTBLOB_LEN, sig->s);

	/* sha1 the data */
	EVP_DigestInit(&md, evp_md);
	EVP_DigestUpdate(&md, data, datalen);
	EVP_DigestFinal(&md, digest, &dlen);

	ret = DSA_do_verify(digest, dlen, sig, key);
	memset(digest, 'd', sizeof(digest));

	DSA_SIG_free(sig);

	return ret;
}


//
// RSA
// 

/*
* See:
* http://www.rsasecurity.com/rsalabs/pkcs/pkcs-1/
* ftp://ftp.rsasecurity.com/pub/pkcs/pkcs-1/pkcs-1v2-1.asn
*/
/*
* id-sha1 OBJECT IDENTIFIER ::= { iso(1) identified-organization(3)
*	oiw(14) secsig(3) algorithms(2) 26 }
*/
static const u_char id_sha1[] = {
	0x30, 0x21, /* type Sequence, length 0x21 (33) */
		0x30, 0x09, /* type Sequence, length 0x09 */
		0x06, 0x05, /* type OID, length 0x05 */
		0x2b, 0x0e, 0x03, 0x02, 0x1a, /* id-sha1 OID */
		0x05, 0x00, /* NULL */
		0x04, 0x14  /* Octet string, length 0x14 (20), followed by sha1 hash */
};
/*
* id-md5 OBJECT IDENTIFIER ::= { iso(1) member-body(2) us(840)
*	rsadsi(113549) digestAlgorithm(2) 5 }
*/
static const u_char id_md5[] = {
	0x30, 0x20, /* type Sequence, length 0x20 (32) */
		0x30, 0x0c, /* type Sequence, length 0x09 */
		0x06, 0x08, /* type OID, length 0x05 */
		0x2a, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x02, 0x05, /* id-md5 */
		0x05, 0x00, /* NULL */
		0x04, 0x10  /* Octet string, length 0x10 (16), followed by md5 hash */
};

static int openssh_RSA_verify(int type, u_char *hash, u_int hashlen,
				   u_char *sigbuf, u_int siglen, RSA *rsa)
{
	u_int ret, rsasize, oidlen = 0, hlen = 0;
	int len;
	const u_char *oid = NULL;
	u_char *decrypted = NULL;

	ret = 0;
	switch (type) {
	case NID_sha1:
		oid = id_sha1;
		oidlen = sizeof(id_sha1);
		hlen = 20;
		break;
	case NID_md5:
		oid = id_md5;
		oidlen = sizeof(id_md5);
		hlen = 16;
		break;
	default:
		goto done;
		break;
	}
	if (hashlen != hlen) {
		//error("bad hashlen");
		goto done;
	}
	rsasize = RSA_size(rsa);
	if (siglen == 0 || siglen > rsasize) {
		//error("bad siglen");
		goto done;
	}
	decrypted = malloc(rsasize);
	if (decrypted == NULL)
		return 1; // error

	if ((len = RSA_public_decrypt(siglen, sigbuf, decrypted, rsa,
		RSA_PKCS1_PADDING)) < 0) {
			//error("RSA_public_decrypt failed: %s",
			//    ERR_error_string(ERR_get_error(), NULL));
			goto done;
		}
		if (len != hlen + oidlen) {
			//error("bad decrypted len: %d != %d + %d", len, hlen, oidlen);
			goto done;
		}
		if (memcmp(decrypted, oid, oidlen) != 0) {
			//error("oid mismatch");
			goto done;
		}
		if (memcmp(decrypted + oidlen, hash, hlen) != 0) {
			//error("hash mismatch");
			goto done;
		}
		ret = 1;
done:
		if (decrypted)
			free(decrypted);
		return ret;
}

static int ssh_rsa_verify(RSA *key, u_char *signature, u_int signaturelen,
				   u_char *data, u_int datalen)
{
	const EVP_MD *evp_md;
	EVP_MD_CTX md;
	//	char *ktype;
	u_char digest[EVP_MAX_MD_SIZE], *sigblob;
	u_int len, dlen, modlen;
//	int rlen, ret, nid;
	int ret, nid;
	char *ptr;

	OpenSSL_add_all_digests();

	if (key == NULL) {
		return -2;
	}
	if (BN_num_bits(key->n) < SSH_RSA_MINIMUM_MODULUS_SIZE) {
		return -3;
	}
	//debug_print(41, signature, signaturelen);
	ptr = signature;

	// step1
	len = get_uint32_MSBfirst(ptr);
	ptr += 4;
	if (strncmp("ssh-rsa", ptr, len) != 0) {
		return -4;
	}
	ptr += len;

	// step2
	len = get_uint32_MSBfirst(ptr);
	ptr += 4;
	sigblob = ptr;
	ptr += len;
#if 0
	rlen = get_uint32_MSBfirst(ptr);
	if (rlen != 0) {
		return -1;
	}
#endif

	/* RSA_verify expects a signature of RSA_size */
	modlen = RSA_size(key);
	if (len > modlen) {
		return -5;

	} else if (len < modlen) {
		u_int diff = modlen - len;
		sigblob = realloc(sigblob, modlen);
		memmove(sigblob + diff, sigblob, len);
		memset(sigblob, 0, diff);
		len = modlen;
	}
	//	nid = (datafellows & SSH_BUG_RSASIGMD5) ? NID_md5 : NID_sha1;
	nid = NID_sha1;
	if ((evp_md = EVP_get_digestbynid(nid)) == NULL) {
		//error("ssh_rsa_verify: EVP_get_digestbynid %d failed", nid);
		return -6;
	}
	EVP_DigestInit(&md, evp_md);
	EVP_DigestUpdate(&md, data, datalen);
	EVP_DigestFinal(&md, digest, &dlen);

	ret = openssh_RSA_verify(nid, digest, dlen, sigblob, len, key);

	memset(digest, 'd', sizeof(digest));
	memset(sigblob, 's', len);
	//free(sigblob);
	//debug("ssh_rsa_verify: signature %scorrect", (ret==0) ? "in" : "");

	return ret;
}

static int key_verify(
			   RSA *rsa_key, 
			   DSA *dsa_key,
			   unsigned char *signature, unsigned int signaturelen,
			   unsigned char *data, unsigned int datalen)
{
	int ret = 0;

	if (rsa_key != NULL) {
		ret = ssh_rsa_verify(rsa_key, signature, signaturelen, data, datalen);

	} else if (dsa_key != NULL) {
		ret = ssh_dss_verify(dsa_key, signature, signaturelen, data, datalen);

	} else {
		return -1;
	}

	return (ret);   // success
}


// Diffie-Hellman Key Exchange Reply(SSH2_MSG_KEXDH_REPLY:31)
//
static BOOL handle_SSH2_dh_kex_reply(PTInstVar pvar)
{
	char *data;
	int len;
	int offset = 0;
	char *server_host_key_blob;
	int bloblen, keynamelen, siglen;
	char key[128];
	RSA *rsa = NULL;
	DSA *dsa = NULL;
	BIGNUM *dh_server_pub = NULL;
	char *signature;
	int dh_len, share_len;
	char *dh_buf = NULL;
	BIGNUM *share_key = NULL;
	char *hash;
	char *emsg, emsg_tmp[1024];  // error message


	// TODO: buffer overrun check

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	// for debug
	//write_buffer_file(data, len);

	bloblen = get_uint32_MSBfirst(data);
	data += 4;
	server_host_key_blob = data; // for hash

	keynamelen = get_uint32_MSBfirst(data);
	if (keynamelen >= 128) {
		emsg = "keyname length too big @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	data +=4 ;
	memcpy(key, data, keynamelen);
	key[keynamelen] = 0;
	data += keynamelen;

	// RSA key
	if (strcmp(key, "ssh-rsa") == 0) {
		rsa = RSA_new();
		if (rsa == NULL) {
			emsg = "Out of memory1 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}
		rsa->n = BN_new();
		rsa->e = BN_new();
		if (rsa->n == NULL || rsa->e == NULL) {
			emsg = "Out of memory2 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}

		buffer_get_bignum2(&data, rsa->e);
		buffer_get_bignum2(&data, rsa->n);

	} else if (strcmp(key, "ssh-dss") == 0) { // DSA key
		dsa = DSA_new();
		if (dsa == NULL) {
			emsg = "Out of memory3 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}
		dsa->p = BN_new();
		dsa->q = BN_new();
		dsa->g = BN_new();
		dsa->pub_key = BN_new();
		if (dsa->p == NULL || 
			dsa->q == NULL ||
			dsa->g == NULL ||
			dsa->pub_key == NULL) {
			emsg = "Out of memory4 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}

		buffer_get_bignum2(&data, dsa->p);
		buffer_get_bignum2(&data, dsa->q);
		buffer_get_bignum2(&data, dsa->g);
		buffer_get_bignum2(&data, dsa->pub_key);

	} else {
		// unknown key
		_snprintf(emsg_tmp, sizeof(emsg_tmp), "Unknown key type(%s) @ handle_SSH2_dh_kex_reply()", key);
		emsg = emsg_tmp;
		goto error;

	}

	dh_server_pub = BN_new();
	if (dh_server_pub == NULL) {
		emsg = "Out of memory5 @ handle_SSH2_dh_kex_reply()";
		goto error;
	}

	buffer_get_bignum2(&data, dh_server_pub);

	siglen = get_uint32_MSBfirst(data);
	data += 4;
	signature = data;
	data += siglen;


	// check DH public value
	if (!dh_pub_is_valid(pvar->kexdh, dh_server_pub)) {
		emsg = "DH public value invalid @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	// 共通鍵の生成
	dh_len = DH_size(pvar->kexdh);
	dh_buf = malloc(dh_len);
	if (dh_buf == NULL) {
		emsg = "Out of memory6 @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	share_len = DH_compute_key(dh_buf, dh_server_pub, pvar->kexdh);
	share_key = BN_new();
	if (share_key == NULL) {
		emsg = "Out of memory7 @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	// 'share_key'がサーバとクライアントで共有する鍵（G^A×B mod P）となる。
	BN_bin2bn(dh_buf, share_len, share_key);
	//debug_print(40, dh_buf, share_len);

	// ハッシュの計算
    /* calc and verify H */    
	hash = kex_dh_hash(
		pvar->client_version_string,
		pvar->server_version_string,
		buffer_ptr(pvar->my_kex),  buffer_len(pvar->my_kex),       
		buffer_ptr(pvar->peer_kex),  buffer_len(pvar->peer_kex),       
		server_host_key_blob, bloblen,        
		pvar->kexdh->pub_key,        
		dh_server_pub,        
		share_key
    );
	//debug_print(30, hash, 20);
	//debug_print(31, pvar->client_version_string, strlen(pvar->client_version_string));
	//debug_print(32, pvar->server_version_string, strlen(pvar->server_version_string));
	//debug_print(33, buffer_ptr(pvar->my_kex), buffer_len(pvar->my_kex));
	//debug_print(34, buffer_ptr(pvar->peer_kex), buffer_len(pvar->peer_kex));
	//debug_print(35, server_host_key_blob, bloblen);

	// session idの保存（初回接続時のみ）
	if (pvar->session_id == NULL) {
		pvar->session_id_len = 20;
		pvar->session_id = malloc(pvar->session_id_len);
		if (pvar->session_id != NULL) {
			memcpy(pvar->session_id, hash, pvar->session_id_len);
		} else {
			// TODO:
		}
	}

	if (key_verify(rsa, dsa, signature, siglen, hash, 20) != 1) {
		emsg = "key verify error @ handle_SSH2_dh_kex_reply()";
		save_memdump(LOGDUMP);
		goto error;
	}

	kex_derive_keys(pvar, pvar->we_need, hash, share_key, pvar->session_id, pvar->session_id_len);

	// KEX finish
	begin_send_packet(pvar, SSH2_MSG_NEWKEYS, 0);
	finish_send_packet(pvar);

	// SSH2_MSG_NEWKEYSを送り終わったあとにキーの設定および再設定を行う
	// 送信用の暗号鍵は SSH2_MSG_NEWKEYS の送信後に、受信用のは SSH2_MSG_NEWKEYS の
	// 受信後に再設定を行う。
	if (pvar->rekeying == 1) { // キーの再設定
		// まず、送信用だけ設定する。
		ssh2_set_newkeys(pvar, MODE_OUT);
		pvar->ssh2_keys[MODE_OUT].mac.enabled = 1;
		if (!CRYPT_start_encryption(pvar, 1, 0)) {
			// TODO: error
		}

	} else {
		// 初回接続の場合は実際に暗号ルーチンが設定されるのは、あとになってから
		// なので（CRYPT_start_encryption関数）、ここで鍵の設定をしてしまってもよい。
		ssh2_set_newkeys(pvar, MODE_IN);
		ssh2_set_newkeys(pvar, MODE_OUT);
	}

	// TTSSHバージョン情報に表示するキービット数を求めておく (2004.10.30 yutaka)
	pvar->client_key_bits = BN_num_bits(pvar->kexdh->pub_key);
	pvar->server_key_bits = BN_num_bits(dh_server_pub);

	SSH2_dispatch_init(3);
	SSH2_dispatch_add_message(SSH2_MSG_NEWKEYS);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.5 yutaka)

	BN_free(dh_server_pub);
	RSA_free(rsa);
	DSA_free(dsa);
	DH_free(pvar->kexdh); pvar->kexdh = NULL;
	free(dh_buf);
	return TRUE;

error:
	RSA_free(rsa);
	DSA_free(dsa);
	DH_free(pvar->kexdh); pvar->kexdh = NULL;
	BN_free(dh_server_pub);
	free(dh_buf);
	BN_free(share_key);

	notify_fatal_error(pvar, emsg);

	return FALSE;
}



// SHA-1(160bit)を求める
static unsigned char *kex_dh_gex_hash(
    char *client_version_string,
    char *server_version_string,
    char *ckexinit, int ckexinitlen,
    char *skexinit, int skexinitlen,
    u_char *serverhostkeyblob, int sbloblen,
	int kexgex_min,
	int kexgex_bits,
	int kexgex_max,
	BIGNUM *kexgex_p,
	BIGNUM *kexgex_g,
    BIGNUM *client_dh_pub,
    BIGNUM *server_dh_pub,
    BIGNUM *shared_secret)
{
	buffer_t *b;
	static unsigned char digest[EVP_MAX_MD_SIZE];
	const EVP_MD *evp_md = EVP_sha1();
	EVP_MD_CTX md;

	b = buffer_init();
	buffer_put_string(b, client_version_string, strlen(client_version_string));
	buffer_put_string(b, server_version_string, strlen(server_version_string));

	/* kexinit messages: fake header: len+SSH2_MSG_KEXINIT */
	buffer_put_int(b, ckexinitlen+1);
	buffer_put_char(b, SSH2_MSG_KEXINIT);
	buffer_append(b, ckexinit, ckexinitlen);
	buffer_put_int(b, skexinitlen+1);
	buffer_put_char(b, SSH2_MSG_KEXINIT);
	buffer_append(b, skexinit, skexinitlen);

	buffer_put_string(b, serverhostkeyblob, sbloblen);

	// DH group sizeのビット数を加算する
	buffer_put_int(b, kexgex_min);
	buffer_put_int(b, kexgex_bits);
	buffer_put_int(b, kexgex_max);

	// DH鍵の素数と生成元を加算する
	buffer_put_bignum2(b, kexgex_p);
	buffer_put_bignum2(b, kexgex_g);

	buffer_put_bignum2(b, client_dh_pub);
	buffer_put_bignum2(b, server_dh_pub);
	buffer_put_bignum2(b, shared_secret);

	// yutaka
	//debug_print(38, buffer_ptr(b), buffer_len(b));

	EVP_DigestInit(&md, evp_md);
	EVP_DigestUpdate(&md, buffer_ptr(b), buffer_len(b));
	EVP_DigestFinal(&md, digest, NULL);

	buffer_free(b);

	//write_buffer_file(digest, EVP_MD_size(evp_md));

	return digest;
}

// Diffie-Hellman Key Exchange Reply(SSH2_MSG_KEX_DH_GEX_REPLY:33)
//
// C then computes K = f^x mod p, H = hash(V_C ||
//          V_S || I_C || I_S || K_S || min || n || max || p || g || e ||
//          f || K), and verifies the signature s on H.
static BOOL handle_SSH2_dh_gex_reply(PTInstVar pvar)
{
	char *data;
	int len;
	int offset = 0;
	char *server_host_key_blob;
	int bloblen, keynamelen, siglen;
	char key[128];
	RSA *rsa = NULL;
	DSA *dsa = NULL;
	BIGNUM *dh_server_pub = NULL;
	char *signature;
	int dh_len, share_len;
	char *dh_buf = NULL;
	BIGNUM *share_key = NULL;
	char *hash;
	char *emsg, emsg_tmp[1024];  // error message
	int ret;


	// TODO: buffer overrun check

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	push_memdump("DH_GEX_REPLY", "full dump", data, len);

	// for debug
	//write_buffer_file(data, len);

	bloblen = get_uint32_MSBfirst(data);
	data += 4;
	server_host_key_blob = data; // for hash

	push_memdump("DH_GEX_REPLY", "server_host_key_blob", server_host_key_blob, bloblen);

	keynamelen = get_uint32_MSBfirst(data);
	if (keynamelen >= 128) {
		emsg = "keyname length too big @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	data +=4 ;
	memcpy(key, data, keynamelen);
	key[keynamelen] = 0;
	data += keynamelen;

	push_memdump("DH_GEX_REPLY", "keyname", key, keynamelen);

	// RSA key
	if (strcmp(key, "ssh-rsa") == 0) {
		rsa = RSA_new();
		if (rsa == NULL) {
			emsg = "Out of memory1 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}
		rsa->n = BN_new();
		rsa->e = BN_new();
		if (rsa->n == NULL || rsa->e == NULL) {
			emsg = "Out of memory2 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}

		buffer_get_bignum2(&data, rsa->e);
		buffer_get_bignum2(&data, rsa->n);

	} else if (strcmp(key, "ssh-dss") == 0) { // DSA key
		dsa = DSA_new();
		if (dsa == NULL) {
			emsg = "Out of memory3 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}
		dsa->p = BN_new();
		dsa->q = BN_new();
		dsa->g = BN_new();
		dsa->pub_key = BN_new();
		if (dsa->p == NULL || 
			dsa->q == NULL ||
			dsa->g == NULL ||
			dsa->pub_key == NULL) {
			emsg = "Out of memory4 @ handle_SSH2_dh_kex_reply()";
			goto error;
		}

		buffer_get_bignum2(&data, dsa->p);
		buffer_get_bignum2(&data, dsa->q);
		buffer_get_bignum2(&data, dsa->g);
		buffer_get_bignum2(&data, dsa->pub_key);

	} else {
		// unknown key
		_snprintf(emsg_tmp, sizeof(emsg_tmp), "Unknown key type(%s) @ handle_SSH2_dh_kex_reply()", key);
		emsg = emsg_tmp;
		goto error;

	}

	dh_server_pub = BN_new();
	if (dh_server_pub == NULL) {
		emsg = "Out of memory5 @ handle_SSH2_dh_kex_reply()";
		goto error;
	}

	buffer_get_bignum2(&data, dh_server_pub);

	siglen = get_uint32_MSBfirst(data);
	data += 4;
	signature = data;
	data += siglen;

	push_memdump("DH_GEX_REPLY", "signature", signature, siglen);

	// check DH public value
	if (!dh_pub_is_valid(pvar->kexdh, dh_server_pub)) {
		emsg = "DH public value invalid @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	// 共通鍵の生成
	dh_len = DH_size(pvar->kexdh);
	dh_buf = malloc(dh_len);
	if (dh_buf == NULL) {
		emsg = "Out of memory6 @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	share_len = DH_compute_key(dh_buf, dh_server_pub, pvar->kexdh);
	share_key = BN_new();
	if (share_key == NULL) {
		emsg = "Out of memory7 @ handle_SSH2_dh_kex_reply()";
		goto error;
	}
	// 'share_key'がサーバとクライアントで共有する鍵（G^A×B mod P）となる。
	BN_bin2bn(dh_buf, share_len, share_key);
	//debug_print(40, dh_buf, share_len);

	// ハッシュの計算
    /* calc and verify H */    
	hash = kex_dh_gex_hash(
		pvar->client_version_string,
		pvar->server_version_string,
		buffer_ptr(pvar->my_kex),  buffer_len(pvar->my_kex),       
		buffer_ptr(pvar->peer_kex),  buffer_len(pvar->peer_kex),       
		server_host_key_blob, bloblen,    
		/////// KEXGEX
		pvar->kexgex_min, 
		pvar->kexgex_bits, 
		pvar->kexgex_max,
		pvar->kexdh->p,        
		pvar->kexdh->g,
		pvar->kexdh->pub_key,
		/////// KEXGEX
		dh_server_pub,        
		share_key
    );

	
	{
		push_memdump("DH_GEX_REPLY kex_dh_gex_hash", "my_kex", buffer_ptr(pvar->my_kex), buffer_len(pvar->my_kex));
		push_memdump("DH_GEX_REPLY kex_dh_gex_hash", "peer_kex", buffer_ptr(pvar->peer_kex), buffer_len(pvar->peer_kex));

		push_bignum_memdump("DH_GEX_REPLY kex_dh_gex_hash", "dh_server_pub", dh_server_pub);
		push_bignum_memdump("DH_GEX_REPLY kex_dh_gex_hash", "share_key", share_key);

		push_memdump("DH_GEX_REPLY kex_dh_gex_hash", "hash", hash, 20);
	}

	//debug_print(30, hash, 20);
	//debug_print(31, pvar->client_version_string, strlen(pvar->client_version_string));
	//debug_print(32, pvar->server_version_string, strlen(pvar->server_version_string));
	//debug_print(33, buffer_ptr(pvar->my_kex), buffer_len(pvar->my_kex));
	//debug_print(34, buffer_ptr(pvar->peer_kex), buffer_len(pvar->peer_kex));
	//debug_print(35, server_host_key_blob, bloblen);

	// session idの保存（初回接続時のみ）
	if (pvar->session_id == NULL) {
		pvar->session_id_len = 20;
		pvar->session_id = malloc(pvar->session_id_len);
		if (pvar->session_id != NULL) {
			memcpy(pvar->session_id, hash, pvar->session_id_len);
		} else {
			// TODO:
		}
	}

	if ((ret = key_verify(rsa, dsa, signature, siglen, hash, 20)) != 1) {
		_snprintf(emsg_tmp, sizeof(emsg_tmp), "key verify error(%d) @ SSH2_DH_GEX\r\n%s", ret, SENDTOME);
		emsg = emsg_tmp;
		save_memdump(LOGDUMP);
		goto error;
	}

	kex_derive_keys(pvar, pvar->we_need, hash, share_key, pvar->session_id, pvar->session_id_len);

	// KEX finish
	begin_send_packet(pvar, SSH2_MSG_NEWKEYS, 0);
	finish_send_packet(pvar);

	// SSH2_MSG_NEWKEYSを送り終わったあとにキーの設定および再設定を行う
	// 送信用の暗号鍵は SSH2_MSG_NEWKEYS の送信後に、受信用のは SSH2_MSG_NEWKEYS の
	// 受信後に再設定を行う。
	if (pvar->rekeying == 1) { // キーの再設定
		// まず、送信用だけ設定する。
		ssh2_set_newkeys(pvar, MODE_OUT);
		pvar->ssh2_keys[MODE_OUT].mac.enabled = 1;
		if (!CRYPT_start_encryption(pvar, 1, 0)) {
			// TODO: error
		}

	} else {
		// 初回接続の場合は実際に暗号ルーチンが設定されるのは、あとになってから
		// なので（CRYPT_start_encryption関数）、ここで鍵の設定をしてしまってもよい。
		ssh2_set_newkeys(pvar, MODE_IN);
		ssh2_set_newkeys(pvar, MODE_OUT);
	}

	// TTSSHバージョン情報に表示するキービット数を求めておく (2004.10.30 yutaka)
	pvar->client_key_bits = BN_num_bits(pvar->kexdh->pub_key);
	pvar->server_key_bits = BN_num_bits(dh_server_pub);

	SSH2_dispatch_init(3);
	SSH2_dispatch_add_message(SSH2_MSG_NEWKEYS);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.5 yutaka)

	BN_free(dh_server_pub);
	RSA_free(rsa);
	DSA_free(dsa);
	DH_free(pvar->kexdh); pvar->kexdh = NULL;
	free(dh_buf);
	return TRUE;

error:
	RSA_free(rsa);
	DSA_free(dsa);
	DH_free(pvar->kexdh); pvar->kexdh = NULL;
	BN_free(dh_server_pub);
	free(dh_buf);
	BN_free(share_key);

	notify_fatal_error(pvar, emsg);

	return FALSE;
}


// KEXにおいてサーバから返ってくる 31 番メッセージに対するハンドラ
static BOOL handle_SSH2_dh_common_reply(PTInstVar pvar)
{

	if (pvar->kex_type == KEX_DH_GRP1_SHA1 || pvar->kex_type == KEX_DH_GRP14_SHA1) {
		handle_SSH2_dh_kex_reply(pvar);

	} else if (pvar->kex_type == KEX_DH_GEX_SHA1) {
		handle_SSH2_dh_gex_group(pvar);

	} else {
		// TODO: 

	}

	return TRUE;
}


static void do_SSH2_dispatch_setup_for_transfer(PTInstVar pvar)
{
	// キー再作成情報のクリア (2005.6.19 yutaka)
	pvar->rekeying = 0;

	SSH2_dispatch_init(6);
	SSH2_dispatch_add_range_message(SSH2_MSG_REQUEST_SUCCESS, SSH2_MSG_CHANNEL_FAILURE);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX
	// OpenSSH 3.9ではデータ通信中のDH鍵交換要求が、サーバから送られてくることがある。
	SSH2_dispatch_add_message(SSH2_MSG_KEXINIT);
}


static BOOL handle_SSH2_newkeys(PTInstVar pvar)
{
	int supported_ciphers = (1 << SSH_CIPHER_3DES_CBC | 1 << SSH_CIPHER_AES128);
	int type = (1 << SSH_AUTH_PASSWORD) | (1 << SSH_AUTH_RSA) | (1 << SSH_AUTH_TIS);

	notify_verbose_message(pvar, "SSH2_MSG_NEWKEYS is received(DH key generation is completed).", LOG_LEVEL_VERBOSE);

	// ログ採取の終了 (2005.3.7 yutaka)
	finish_memdump();

	// finish key exchange
	pvar->key_done = 1;

	// キー再作成なら認証はパスする。
	if (pvar->rekeying == 1) {
		// かつ、受信用の暗号鍵の再設定をここで行う。
		ssh2_set_newkeys(pvar, MODE_IN);
		pvar->ssh2_keys[MODE_IN].mac.enabled = 1;
		if (!CRYPT_start_encryption(pvar, 0, 1)) {
			// TODO: error
		}
		do_SSH2_dispatch_setup_for_transfer(pvar);
		return TRUE;
	}

	// 暗号アルゴリズムの設定
	if (!CRYPT_set_supported_ciphers(pvar, supported_ciphers, supported_ciphers))
		return FALSE;
	if (!AUTH_set_supported_auth_types(pvar, type))
		return FALSE;

	SSH_notify_host_OK(pvar);


	return TRUE;
}

// ユーザ認証の開始
BOOL do_SSH2_userauth(PTInstVar pvar)
{
	buffer_t *msg;
	char *s;
	unsigned char *outmsg;
	int len;
	int mode;

	// SSH2 keyboard-interactive methodの初期化 (2005.1.22 yutaka)
	pvar->keyboard_interactive_done = 0;
	// パスワードが入力されたら 1 を立てる (2005.3.12 yutaka)
	pvar->keyboard_interactive_password_input = 0;

	// すでにログイン処理を行っている場合は、SSH2_MSG_SERVICE_REQUESTの送信は
	// しないことにする。OpenSSHでは支障ないが、Tru64 UNIXではサーバエラーとなってしまうため。
	// (2005.3.10 yutaka)
	if (pvar->userauth_retry_count > 0) {
		return handle_SSH2_authrequest(pvar);
		/* NOT REACHED */
	}

	for (mode = 0 ; mode < MODE_MAX ; mode++) {
		pvar->ssh2_keys[mode].mac.enabled = 1;
	}

	// start user authentication
	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return FALSE;
	}
	s = "ssh-userauth";
	buffer_put_string(msg, s, strlen(s));
	//buffer_put_padding(msg, 32); // XXX: 
	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_SERVICE_REQUEST, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);
	buffer_free(msg);

	SSH2_dispatch_init(4);
	SSH2_dispatch_add_message(SSH2_MSG_SERVICE_ACCEPT);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.5 yutaka)

	notify_verbose_message(pvar, "SSH2_MSG_SERVICE_REQUEST was sent.", LOG_LEVEL_VERBOSE);

	return TRUE;
}


static char *get_SSH2_keyname(CRYPTKeyPair *keypair)
{
	char *s;

	if (keypair->RSA_key != NULL) {
		s = "ssh-rsa";
	} else {
		s = "ssh-dss";
	}

	return (s);
}


static BOOL generate_SSH2_keysign(CRYPTKeyPair *keypair, char **sigptr, int *siglen, char *data, int datalen)
{
	buffer_t *msg = NULL;
	char *s;

	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return FALSE;
	}

	if (keypair->RSA_key != NULL) { // RSA
		const EVP_MD *evp_md;
		EVP_MD_CTX md;
		u_char digest[EVP_MAX_MD_SIZE], *sig;
		u_int slen, dlen, len;
		int ok, nid;

		nid = NID_sha1;
		if ((evp_md = EVP_get_digestbynid(nid)) == NULL) {
			goto error;
		}

		// ダイジェスト値の計算
		EVP_DigestInit(&md, evp_md);
		EVP_DigestUpdate(&md, data, datalen);
		EVP_DigestFinal(&md, digest, &dlen);

		slen = RSA_size(keypair->RSA_key);
		sig = malloc(slen);
		if (sig == NULL)
			goto error;

		// 電子署名を計算
		ok = RSA_sign(nid, digest, dlen, sig, &len, keypair->RSA_key);
		memset(digest, 'd', sizeof(digest));
		if (ok != 1) { // error
			free(sig);
			goto error;
		}
		// 署名のサイズがバッファより小さい場合、後ろへずらす。先頭はゼロで埋める。
		if (len < slen) {
			u_int diff = slen - len;
			memmove(sig + diff, sig, len);
			memset(sig, 0, diff);

		} else if (len > slen) {
			free(sig);
			goto error;

		} else {
			// do nothing

		}

		s = get_SSH2_keyname(keypair);
		buffer_put_string(msg, s, strlen(s));
		buffer_append_length(msg, sig, slen);
		len = buffer_len(msg);

		// setting
		*siglen = len;
		*sigptr = malloc(len);
		if (*sigptr == NULL) {
			free(sig);
			goto error;
		}
		memcpy(*sigptr, buffer_ptr(msg), len);
		free(sig);

	} else { // DSA
		DSA_SIG *sig;
		const EVP_MD *evp_md = EVP_sha1();
		EVP_MD_CTX md;
		u_char digest[EVP_MAX_MD_SIZE], sigblob[SIGBLOB_LEN];
		u_int rlen, slen, len, dlen;

		// ダイジェストの計算
		EVP_DigestInit(&md, evp_md);
		EVP_DigestUpdate(&md, data, datalen);
		EVP_DigestFinal(&md, digest, &dlen);

		// DSA電子署名を計算
		sig = DSA_do_sign(digest, dlen, keypair->DSA_key);
		memset(digest, 'd', sizeof(digest));
		if (sig == NULL) {
			goto error;
		}

		// BIGNUMからバイナリ値への変換
		rlen = BN_num_bytes(sig->r);
		slen = BN_num_bytes(sig->s);
		if (rlen > INTBLOB_LEN || slen > INTBLOB_LEN) {
			DSA_SIG_free(sig);
			goto error;
		}
		memset(sigblob, 0, SIGBLOB_LEN);
		BN_bn2bin(sig->r, sigblob+ SIGBLOB_LEN - INTBLOB_LEN - rlen);
		BN_bn2bin(sig->s, sigblob+ SIGBLOB_LEN - slen);
		DSA_SIG_free(sig);

		// setting
		s = get_SSH2_keyname(keypair);
		buffer_put_string(msg, s, strlen(s));
		buffer_append_length(msg, sigblob, sizeof(sigblob));
		len = buffer_len(msg);

		// setting
		*siglen = len;
		*sigptr = malloc(len);
		if (*sigptr == NULL) {
			goto error;
		}
		memcpy(*sigptr, buffer_ptr(msg), len);

	}

	buffer_free(msg);
	return TRUE;

error:
	buffer_free(msg);

	return FALSE;
}


static BOOL get_SSH2_publickey_blob(PTInstVar pvar, buffer_t **blobptr, int *bloblen)
{
	buffer_t *msg = NULL;
	CRYPTKeyPair *keypair;
	char *s;

	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return FALSE;
	}

	keypair = pvar->auth_state.cur_cred.key_pair;

	if (keypair->RSA_key != NULL) { // RSA
		s = get_SSH2_keyname(keypair);
		buffer_put_string(msg, s, strlen(s));
		buffer_put_bignum2(msg, keypair->RSA_key->e); // 公開指数
		buffer_put_bignum2(msg, keypair->RSA_key->n); // p×q

	} else { // DSA
		s = get_SSH2_keyname(keypair);
		buffer_put_string(msg, s, strlen(s));
		buffer_put_bignum2(msg, keypair->DSA_key->p); // 素数
		buffer_put_bignum2(msg, keypair->DSA_key->q); // (p-1)の素因数
		buffer_put_bignum2(msg, keypair->DSA_key->g); // 整数
		buffer_put_bignum2(msg, keypair->DSA_key->pub_key); // 公開鍵

	}

	*blobptr = msg;
	*bloblen = buffer_len(msg);

	return TRUE;
}


// ユーザ認証パケットの構築
static BOOL handle_SSH2_authrequest(PTInstVar pvar)
{
	buffer_t *msg = NULL;
	char *s;
	unsigned char *outmsg;
	int len;
	char *connect_id = "ssh-connection";
	int kbdint = 0;

	notify_verbose_message(pvar, "SSH2_MSG_SERVICE_ACCEPT is received.", LOG_LEVEL_VERBOSE);

	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return FALSE;
	}

	// ペイロードの構築
	if (pvar->ssh2_autologin == 1) { // SSH2自動ログイン
		s = pvar->ssh2_username;
	} else {
		s = pvar->auth_state.user;  // ユーザ名
	}
	buffer_put_string(msg, s, strlen(s));

	if (pvar->auth_state.cur_cred.method == SSH_AUTH_PASSWORD) { // パスワード認証
		// 初回は keyboard-interactive メソッドでトライする (2005.1.22 yutaka)
		// cf. http://www.openssh.com/txt/draft-ietf-secsh-auth-kbdinteract-02.txt

		if (pvar->settings.ssh2_keyboard_interactive == 1 &&
			pvar->keyboard_interactive_done == 0) { // keyboard-interactive method
			pvar->keyboard_interactive_done = 1;
			kbdint = 1;

			//s = "ssh-userauth";  // service name
			s = connect_id;
			buffer_put_string(msg, s, strlen(s));
			s = "keyboard-interactive";  // method name
			buffer_put_string(msg, s, strlen(s));
			s = "";  // language tag
			buffer_put_string(msg, s, strlen(s));
			s = "";  // submethods
			buffer_put_string(msg, s, strlen(s));


		} else {
			// password authentication method
			s = connect_id;
			buffer_put_string(msg, s, strlen(s));
			s = "password";
			buffer_put_string(msg, s, strlen(s));
			buffer_put_char(msg, 0); // 0

			if (pvar->ssh2_autologin == 1) { // SSH2自動ログイン
				s = pvar->ssh2_password;
			} else {
				s = pvar->auth_state.cur_cred.password;  // パスワード
			}
			buffer_put_string(msg, s, strlen(s));

		}

	} else if (pvar->auth_state.cur_cred.method == SSH_AUTH_TIS) { // keyboard-interactive (2005.3.12 yutaka)
		pvar->keyboard_interactive_done = 1;
		kbdint = 1;

		//s = "ssh-userauth";  // service name
		s = connect_id;
		buffer_put_string(msg, s, strlen(s));
		s = "keyboard-interactive";  // method name
		buffer_put_string(msg, s, strlen(s));
		s = "";  // language tag
		buffer_put_string(msg, s, strlen(s));
		s = "";  // submethods
		buffer_put_string(msg, s, strlen(s));

	} else if (pvar->auth_state.cur_cred.method == SSH_AUTH_RSA) { // 公開鍵認証
		buffer_t *signbuf = NULL;
		buffer_t *blob = NULL;
		int bloblen;
		char *signature = NULL;
		int siglen;
		CRYPTKeyPair *keypair = pvar->auth_state.cur_cred.key_pair;

		if (get_SSH2_publickey_blob(pvar, &blob, &bloblen) == FALSE) {
			goto error;
		}

		// step1
		signbuf = buffer_init();
		if (signbuf == NULL) {
			buffer_free(blob);
			goto error;
		}
		// セッションID
		buffer_append_length(signbuf, pvar->session_id, pvar->session_id_len);
		buffer_put_char(signbuf, SSH2_MSG_USERAUTH_REQUEST); 
		s = pvar->auth_state.user;  // ユーザ名
		buffer_put_string(signbuf, s, strlen(s));
		s = connect_id;
		buffer_put_string(signbuf, s, strlen(s));
		s = "publickey";
		buffer_put_string(signbuf, s, strlen(s));
		buffer_put_char(signbuf, 1); // true
		s = get_SSH2_keyname(keypair); // key typeに応じた文字列を得る
		buffer_put_string(signbuf, s, strlen(s));
		s = buffer_ptr(blob);
		buffer_append_length(signbuf, s, bloblen);

		// 署名の作成
		if ( generate_SSH2_keysign(keypair, &signature, &siglen, buffer_ptr(signbuf), buffer_len(signbuf)) == FALSE) {
			buffer_free(blob);
			buffer_free(signbuf);
			goto error;
		}

		// step3
		s = connect_id;
		buffer_put_string(msg, s, strlen(s));
		s = "publickey";
		buffer_put_string(msg, s, strlen(s));
		buffer_put_char(msg, 1); // true
		s = get_SSH2_keyname(keypair); // key typeに応じた文字列を得る
		buffer_put_string(msg, s, strlen(s));
		s = buffer_ptr(blob);
		buffer_append_length(msg, s, bloblen);
		buffer_append_length(msg, signature, siglen);


		buffer_free(blob);
		buffer_free(signbuf);
		free(signature);

	} else {
		goto error;

	}


	// パケット送信
	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_USERAUTH_REQUEST, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);
	buffer_free(msg);

	SSH2_dispatch_init(5);
	SSH2_dispatch_add_message(SSH2_MSG_IGNORE); // XXX: Tru64 UNIX workaround   (2005.3.5 yutaka)
	if (kbdint == 1) { // keyboard-interactive method
		SSH2_dispatch_add_message(SSH2_MSG_USERAUTH_INFO_REQUEST);
	} 
	SSH2_dispatch_add_message(SSH2_MSG_USERAUTH_SUCCESS);
	SSH2_dispatch_add_message(SSH2_MSG_USERAUTH_FAILURE);
	SSH2_dispatch_add_message(SSH2_MSG_USERAUTH_BANNER);

	{
	char buf[128];
	_snprintf(buf, sizeof(buf), "SSH2_MSG_USERAUTH_REQUEST was sent(method %d)", pvar->auth_state.cur_cred.method);
	notify_verbose_message(pvar, buf, LOG_LEVEL_VERBOSE);
	}

	return TRUE;

error:
	buffer_free(msg);

	return FALSE;
}


//
// SSH2 heartbeat procedure
// 
// NAT環境において、SSHクライアントとサーバ間で通信が発生しなかった場合、
// ルータが勝手にNATテーブルをクリアすることがあり、SSHコネクションが
// 切れてしまうことがある。定期的に、クライアントからダミーパケットを
// 送信することで対処する。(2004.12.10 yutaka)
//
static unsigned __stdcall ssh_heartbeat_thread(void FAR * p)
{
	static int instance = 0;
	PTInstVar pvar = (PTInstVar)p;
	time_t tick;

	// すでに実行中なら何もせずに返る。
	if (instance > 0)
		return 0;
	instance++;

	for (;;) {
		// ソケットがクローズされたらスレッドを終わる
		if (pvar->socket == INVALID_SOCKET)
			break;

		// 一定時間無通信であれば、サーバへダミーパケットを送る
		// 閾値が0であれば何もしない。
		tick = time(NULL) - pvar->ssh_heartbeat_tick;
		if (pvar->ts_SSH->ssh_heartbeat_overtime > 0 && 
			tick > pvar->ts_SSH->ssh_heartbeat_overtime) { 
			buffer_t *msg;
			char *s;
			unsigned char *outmsg;
			int len;

			// 別コンテキストで、SSHのシーケンスへ割り込まないようにロックを取る。
			ssh_heartbeat_lock();

			msg = buffer_init();
			if (msg == NULL) {
				// TODO: error check
				continue;
			}
			s = "ssh-heartbeat";
			buffer_put_string(msg, s, strlen(s));
			len = buffer_len(msg);
			if (SSHv1(pvar)) {
				outmsg = begin_send_packet(pvar, SSH_MSG_IGNORE, len);
			} else {
				outmsg = begin_send_packet(pvar, SSH2_MSG_IGNORE, len);
			}
			memcpy(outmsg, buffer_ptr(msg), len);
			finish_send_packet(pvar);
			buffer_free(msg);

			ssh_heartbeat_unlock();
		}

		Sleep(100); // yield
	}

	instance = 0;

	return 0; 
}

static void start_ssh_heartbeat_thread(PTInstVar pvar)
{
	HANDLE thread;
	unsigned tid;

	thread = (HANDLE)_beginthreadex(NULL, 0, ssh_heartbeat_thread, pvar, 0, &tid);
	if (thread == (HANDLE)-1) {
		// TODO:
	} 
	pvar->ssh_heartbeat_thread = thread;
}

// スレッドの停止 (2004.12.27 yutaka)
void halt_ssh_heartbeat_thread(PTInstVar pvar)
{
	if (pvar->ssh_heartbeat_thread != (HANDLE)-1L) {
		WaitForSingleObject(pvar->ssh_heartbeat_thread, INFINITE);
		CloseHandle(pvar->ssh_heartbeat_thread);
		pvar->ssh_heartbeat_thread = (HANDLE)-1L;
	}
}


static BOOL handle_SSH2_userauth_success(PTInstVar pvar)
{
	buffer_t *msg;
	char *s;
	unsigned char *outmsg;
	int len;
	Channel_t *c;

	// 認証OK
	pvar->userauth_success = 1;

	// ポートフォワーディングの準備 (2005.2.26 yutaka)
	FWD_enter_interactive_mode(pvar);

	// ディスパッチルーチンの再設定
	do_SSH2_dispatch_setup_for_transfer(pvar);


	// チャネル設定
	c = ssh2_channel_new(CHAN_SES_WINDOW_DEFAULT, CHAN_SES_PACKET_DEFAULT, TYPE_SHELL, -1);
	if (c == NULL) {
		// TODO: error check
		return FALSE;
	}

	// シェルのIDを取っておく
	pvar->shell_id = c->self_id;

	// シェルオープン
	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return FALSE;
	}
	s = "session";
	buffer_put_string(msg, s, strlen(s));  // ctype
	buffer_put_int(msg, c->self_id);  // self(channel number)
	buffer_put_int(msg, c->local_window);  // local_window
	buffer_put_int(msg, c->local_maxpacket);  // local_maxpacket
	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_OPEN, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);
	buffer_free(msg);

	// ハートビート・スレッドの開始 (2004.12.11 yutaka)
	start_ssh_heartbeat_thread(pvar);

	notify_verbose_message(pvar, "User authentication is successful and SSH heartbeat thread is starting.", LOG_LEVEL_VERBOSE);
	notify_verbose_message(pvar, "SSH2_MSG_CHANNEL_OPEN was sent.", LOG_LEVEL_VERBOSE);

	return TRUE;
}


static BOOL handle_SSH2_userauth_failure(PTInstVar pvar)
{
	// TCP connection closed
	//notify_closed_connection(pvar);

	// retry countの追加 (2005.3.10 yutaka)
	pvar->userauth_retry_count++;

	// keyboard-interactive methodでトライして失敗した場合、次にpassword authentication method
	// で無条件にトライしてみる。(2005.1.22 yutaka)
	if (pvar->keyboard_interactive_done == 1) {
		handle_SSH2_authrequest(pvar);
		pvar->keyboard_interactive_done = 0; // clear flag
		return TRUE;
	}

	if (pvar->ssh2_autologin == 1) {
		// SSH2自動ログインが有効の場合は、リトライは行わない。(2004.12.4 yutaka)
		notify_fatal_error(pvar,
			"SSH2 autologin error: user authentication was failure");
		return TRUE;
	}

	// ユーザ認証に失敗したときは、ユーザ名は固定して、パスワードの再入力を
	// させる。ここの処理は SSH1 と同じ。(2004.10.3 yutaka)
	AUTH_set_generic_mode(pvar);
	AUTH_advance_to_next_cred(pvar);
	pvar->ssh_state.status_flags &= ~STATUS_DONT_SEND_CREDENTIALS;
	try_send_credentials(pvar);

	return TRUE;
}

static BOOL handle_SSH2_userauth_banner(PTInstVar pvar)
{
	//

	return TRUE;
}


// SSH2 keyboard-interactive methodの SSH2_MSG_USERAUTH_INFO_REQUEST 処理関数
//
// ※メモ：OpenSSHでPAMを有効にする方法
//・ビルド
//# ./configure --with-pam
//# make
//
//・/etc/ssh/sshd_config に下記のように書く。
//PasswordAuthentication no
//PermitEmptyPasswords no
//ChallengeResponseAuthentication yes
//UsePAM yes
//
// (2005.1.23 yutaka)
BOOL handle_SSH2_userauth_inforeq(PTInstVar pvar)
{
	int len;
	char *data;
	int slen = 0, num, echo;
	char *s, *prompt = NULL;
	buffer_t *msg;
	unsigned char *outmsg;
	int i;

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	//debug_print(10, data, len);

	///////// step1
	// get string
	slen = get_uint32_MSBfirst(data);
	data += 4;
	s = data;  // name
	data += slen;

	// get string
	slen = get_uint32_MSBfirst(data);
	data += 4;
	s = data;  // instruction
	data += slen;

	// get string
	slen = get_uint32_MSBfirst(data);
	data += 4;
	s = data;  // language tag
	data += slen;

	// num-prompts
	num = get_uint32_MSBfirst(data);
	data += 4;

	///////// step2
	// サーバへパスフレーズを送る
	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return FALSE;
	}
	buffer_put_int(msg, num);

	// プロンプトの数だけ prompt & echo が繰り返される。
	for (i = 0 ; i < num ; i++) {
		// get string
		slen = get_uint32_MSBfirst(data);
		data += 4;
		prompt = data;  // prompt
		data += slen;

		// get boolean
		echo = data[0];
		data += 1;

		// keyboard-interactive method (2005.3.12 yutaka)
		if (pvar->keyboard_interactive_password_input == 0 &&
			pvar->auth_state.cur_cred.method == SSH_AUTH_TIS) {
			AUTH_set_TIS_mode(pvar, prompt, slen);
			AUTH_advance_to_next_cred(pvar);
			pvar->ssh_state.status_flags &= ~STATUS_DONT_SEND_CREDENTIALS;
			//try_send_credentials(pvar);
			buffer_free(msg);
			return TRUE;
		}

		// TODO: ここでプロンプトを表示してユーザから入力させるのが正解。
		s = pvar->auth_state.cur_cred.password;
		buffer_put_string(msg, s, strlen(s));  
	}

	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_USERAUTH_INFO_RESPONSE, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);
	buffer_free(msg);

	return TRUE;
}


static BOOL handle_SSH2_open_confirm(PTInstVar pvar)
{	
	buffer_t *msg;
	char *s;
	unsigned char *outmsg;
	int len;
	char *data;
	int id, remote_id;
	Channel_t *c;
#ifdef DONT_WANTCONFIRM
	int wantconfirm = 0; // false 
#else
	int wantconfirm = 1; // true 
#endif

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	id = get_uint32_MSBfirst(data);
	data += 4;

	c = ssh2_channel_lookup(id);
	if (c == NULL) {
		// TODO:
		return FALSE;
	}

	// TODO: id check
	remote_id = get_uint32_MSBfirst(data);
	data += 4;

	c->remote_id = remote_id;
	pvar->session_nego_status = 1;  

	// remote window size
	c->remote_window = get_uint32_MSBfirst(data);
	data += 4;
	c->remote_maxpacket = get_uint32_MSBfirst(data);
	data += 4;

	if (c->type == TYPE_PORTFWD) {
		// port-forwadingの"direct-tcpip"が成功。
		FWD_confirmed_open(pvar, c->local_num, -1);
		notify_verbose_message(pvar, "SSH2_MSG_CHANNEL_REQUEST was received. (port-fowarding)", LOG_LEVEL_VERBOSE);
		return TRUE;
	}

	//debug_print(100, data, len);

	// pty open
	msg = buffer_init();
	if (msg == NULL) {
		// TODO: error check
		return FALSE;
	}
	buffer_put_int(msg, remote_id);  
	s = "pty-req";  // pseudo terminalのリクエスト
	buffer_put_string(msg, s, strlen(s));  
	buffer_put_char(msg, wantconfirm);  // wantconfirm (disableに変更 2005/3/28 yutaka)
	s = pvar->ts->TermType; // TERM
	buffer_put_string(msg, s, strlen(s));  
	buffer_put_int(msg, pvar->ssh_state.win_cols);  // columns
	buffer_put_int(msg, pvar->ssh_state.win_rows);  // lines
	buffer_put_int(msg, 480);  // XXX:
	buffer_put_int(msg, 640);  // XXX:
	s = "";
	buffer_put_string(msg, s, strlen(s));  
	len = buffer_len(msg);
	outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_REQUEST, len);
	memcpy(outmsg, buffer_ptr(msg), len);
	finish_send_packet(pvar);
	buffer_free(msg);

	notify_verbose_message(pvar, "SSH2_MSG_CHANNEL_REQUEST was sent at handle_SSH2_open_confirm().", LOG_LEVEL_VERBOSE);

	if (wantconfirm == 0) {
		handle_SSH2_channel_success(pvar);
	}

	return TRUE;
}


static BOOL handle_SSH2_request_success(PTInstVar pvar)
{	

	return TRUE;
}

static BOOL handle_SSH2_channel_success(PTInstVar pvar)
{	
	buffer_t *msg;
	char *s;
	unsigned char *outmsg;
	int len;
	Channel_t *c;

	{
	char buf[128];
	_snprintf(buf, sizeof(buf), "SSH2_MSG_CHANNEL_SUCCESS is received(nego_status %d).", pvar->session_nego_status);
	notify_verbose_message(pvar, buf, LOG_LEVEL_VERBOSE);
	}

	if (pvar->session_nego_status == 1) {
#ifdef DONT_WANTCONFIRM
		int wantconfirm = 0; // false 
#else
		int wantconfirm = 1; // true
#endif

		pvar->session_nego_status = 2;  
		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return FALSE;
		}
		// find channel by shell id(2005.2.27 yutaka)
		c = ssh2_channel_lookup(pvar->shell_id);
		if (c == NULL) {
			// TODO: error check
			return FALSE;
		}
		buffer_put_int(msg, c->remote_id);  
//		buffer_put_int(msg, pvar->remote_id);  

		s = "shell";
		buffer_put_string(msg, s, strlen(s));  // ctype
		buffer_put_char(msg, wantconfirm);   // wantconfirm (disableに変更 2005/3/28 yutaka)

		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_REQUEST, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);

		notify_verbose_message(pvar, "SSH2_MSG_CHANNEL_REQUEST was sent at handle_SSH2_channel_success().", LOG_LEVEL_VERBOSE);

		if (wantconfirm == 0) {
			handle_SSH2_channel_success(pvar);
		}

	} else if (pvar->session_nego_status == 2) {
		pvar->session_nego_status = 3;

	} else {

	}

	return TRUE;
}



// クライアントのwindow sizeをサーバへ知らせる
static void do_SSH2_adjust_window_size(PTInstVar pvar, Channel_t *c)
{
	const unsigned int window_size = CHAN_SES_PACKET_DEFAULT;
	buffer_t *msg;
	unsigned char *outmsg;
	int len;

	// ローカルのwindow sizeにまだ余裕があるなら、何もしない。
	if (c->local_window > window_size)
		return;

	{
		// pty open
		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return;
		}
		buffer_put_int(msg, c->remote_id);  
		buffer_put_int(msg, window_size - c->local_window);  

		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, SSH2_MSG_CHANNEL_WINDOW_ADJUST, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);

		// クライアントのwindow sizeを増やす
		c->local_window = window_size;
	}

}

static BOOL handle_SSH2_channel_data(PTInstVar pvar)
{	
	int len;
	char *data;
	int id;
	unsigned int strlen;
	Channel_t *c;

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	//debug_print(80, data, len);

	// channel number
	id = get_uint32_MSBfirst(data);
	data += 4;

	c = ssh2_channel_lookup(id);
	if (c == NULL) {
		// TODO:
		return FALSE;
	}

	// string length
	strlen = get_uint32_MSBfirst(data);
	data += 4;

	// バッファサイズのチェック
	if (strlen > c->local_window_max) {
		// TODO: logging
	}
	if (strlen > c->local_window) {
		// TODO: logging
		// local window sizeより大きなパケットは捨てる
		return FALSE;
	}

	// ペイロードとしてクライアント(TeraTerm)へ渡す
	if (c->type == TYPE_SHELL) {
		pvar->ssh_state.payload_datalen = strlen;
		pvar->ssh_state.payload_datastart = 8;

	} else {
		//debug_print(0, data, strlen);
		FWD_received_data(pvar, c->local_num, data, strlen);

	}

	//debug_print(200, data, strlen);

	// ウィンドウサイズの調整
	c->local_window -= strlen;

	do_SSH2_adjust_window_size(pvar, c);

	return TRUE;
}


static BOOL handle_SSH2_channel_extended_data(PTInstVar pvar)
{	


	return TRUE;
}


static BOOL handle_SSH2_channel_eof(PTInstVar pvar)
{	
	int len;
	char *data;
	int id;
	Channel_t *c;

	// 切断時にサーバが SSH2_MSG_CHANNEL_EOF を送ってくるので、チャネルを解放する。(2005.6.19 yutaka)

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	// channel number
	id = get_uint32_MSBfirst(data);
	data += 4;

	c = ssh2_channel_lookup(id);
	if (c == NULL) {
		// TODO:
		return FALSE;
	}
	if (c->type != TYPE_SHELL) {
		FWD_channel_input_eof(pvar, c->local_num);
	}

	return TRUE;
}


static BOOL handle_SSH2_channel_close(PTInstVar pvar)
{	
	int len;
	char *data;
	int id;
	Channel_t *c;
	buffer_t *msg;
	char *s;
	unsigned char *outmsg;

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	id = get_uint32_MSBfirst(data);
	data += 4;
	c = ssh2_channel_lookup(id);
	if (c == NULL) {
		// TODO:
		return FALSE;
	}

	if (c->type == TYPE_SHELL) {
		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return FALSE;
		}
		buffer_put_int(msg, SSH2_DISCONNECT_PROTOCOL_ERROR);  
		s = "disconnected by server request";
		buffer_put_string(msg, s, strlen(s));  
		s = "";
		buffer_put_string(msg, s, strlen(s));  

		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, SSH2_MSG_DISCONNECT, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);

		// TCP connection closed
		notify_closed_connection(pvar);
	}

	return TRUE;
}


static BOOL handle_SSH2_channel_request(PTInstVar pvar)
{	
	int len;
	char *data;
	int id;
	int buflen;
	char *str;
	int reply;
	int success = 0;
	char *emsg = "exit-status";
	int estat = 0;
	Channel_t *c;

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	//debug_print(98, data, len);

	// ID(4) + string(any) + reply(1) + exit status(4)
	id = get_uint32_MSBfirst(data);
	data += 4;
	c = ssh2_channel_lookup(id);
	if (c == NULL) {
		// TODO:
		return FALSE;
	}

	buflen = get_uint32_MSBfirst(data);
	data += 4;
	str = data;
	data += buflen;

	reply = data[0];
	data += 1;

	// 終了コードが含まれているならば
	if (memcmp(str, emsg, strlen(emsg)) == 0) {
		success = 1;
		estat = get_uint32_MSBfirst(data);
	}

	if (reply) {
		buffer_t *msg;
		unsigned char *outmsg;
		int len;
		int type;

		if (success) {
			type = SSH2_MSG_CHANNEL_SUCCESS;
		} else {
			type = SSH2_MSG_CHANNEL_FAILURE;
		}

		msg = buffer_init();
		if (msg == NULL) {
			// TODO: error check
			return FALSE;
		}
		buffer_put_int(msg, c->remote_id);  

		len = buffer_len(msg);
		outmsg = begin_send_packet(pvar, type, len);
		memcpy(outmsg, buffer_ptr(msg), len);
		finish_send_packet(pvar);
		buffer_free(msg);
	}

	return TRUE;
}


static BOOL handle_SSH2_window_adjust(PTInstVar pvar)
{	
	int len;
	char *data;
	int id;
	unsigned int adjust;
	Channel_t *c;

	// 6byte（サイズ＋パディング＋タイプ）を取り除いた以降のペイロード
	data = pvar->ssh_state.payload;
	// パケットサイズ - (パディングサイズ+1)；真のパケットサイズ
	len = pvar->ssh_state.payloadlen;

	//debug_print(80, data, len);

	// channel number
	id = get_uint32_MSBfirst(data);
	data += 4;

	c = ssh2_channel_lookup(id);
	if (c == NULL)
		return FALSE;

	adjust = get_uint32_MSBfirst(data);
	data += 4;

	// window sizeの調整
	c->remote_window += adjust;

	return TRUE;
}

/*
 * $Log: not supported by cvs2svn $
 * Revision 1.27  2005/06/19 09:17:47  yutakakn
 * SSH2 port-fowarding(local to remote)をサポートした。
 *
 * Revision 1.26  2005/04/08 14:55:03  yutakakn
 * "Duplicate session"においてSSH自動ログインを行うようにした。
 *
 * Revision 1.25  2005/04/03 14:39:48  yutakakn
 * SSH2 channel lookup機構の追加（ポートフォワーディングのため）。
 * TTSSH 2.10で追加したlog dump機構において、DH鍵再作成時にbuffer freeで
 * アプリケーションが落ちてしまうバグを修正。
 *
 * Revision 1.24  2005/03/28 13:52:05  yutakakn
 * SSH2_MSG_CHANNEL_REQUEST送信時において、wantconfirmをfalseにした（サーバからのリプライを期待しない）。
 * NetScreen(HITACHI) workaround対応。
 *
 * Revision 1.23  2005/03/27 04:39:55  yutakakn
 * SSH2のログ採取(verbose)のデータを追加した。
 *
 * Revision 1.22  2005/03/12 15:07:33  yutakakn
 * SSH2 keyboard-interactive認証をTISダイアログに実装した。
 *
 * Revision 1.21  2005/03/10 13:40:39  yutakakn
 * すでにログイン処理を行っている場合は、SSH2_MSG_SERVICE_REQUESTの送信は
 * しないことにする。OpenSSHでは支障ないが、Tru64 UNIXではサーバエラーとなってしまうため。
 *
 * Revision 1.20  2005/03/09 14:14:25  yutakakn
 * サーバIDに CR+LF が含まれていた場合、CRの除去ができていなかったバグを修正。
 *
 * Revision 1.19  2005/03/08 14:24:11  yutakakn
 * SSH2 log dump機構の追加。
 * とりあえず、DH_GEXにおけるkey verifyまでにトレース採取を組み込んだ。
 *
 * Revision 1.18  2005/03/05 10:19:05  yutakakn
 * Tru64 UNIX(HP-UX)向けworkaroundを追加。
 *
 * Revision 1.17  2005/03/03 13:37:31  yutakakn
 * Tru64 UNIX(HP-UX)向けworkaroundを追加。
 * KEXINIT時にSSH2_MSG_IGNOREを受信可能とした。
 *
 * Revision 1.16  2005/03/03 13:28:23  yutakakn
 * クライアントのSSHバージョンを ttxssh.dll から取得して、サーバへ送るようにした。
 *
 * Revision 1.15  2005/01/24 14:07:07  yutakakn
 * ・keyboard-interactive認証をサポートした。
 * 　それに伴い、teraterm.iniに "KeyboardInteractive" エントリを追加した。
 * ・バージョンダイアログに OpenSSLバージョン を追加
 *
 * Revision 1.14  2005/01/06 12:29:07  yutakakn
 * telnet接続時にターミナルサイズ変更を行うと、アプリケーションエラーとなるバグを修正。
 *
 * Revision 1.13  2005/01/04 16:09:45  yutakakn
 * キー再作成時にMAC corruptとなるバグを修正（メモリの二重フリーが原因）。
 *
 * Revision 1.12  2005/01/04 13:57:01  yutakakn
 * SSH2ターミナルサイズ変更通知の追加
 *
 * Revision 1.11  2004/12/27 14:22:16  yutakakn
 * メモリリークを修正。
 *
 * Revision 1.10  2004/12/27 14:05:08  yutakakn
 * 'Auto window close'が有効の場合、切断後の接続ができない問題を修正した。
 * 　・スレッドの終了待ち合わせ処理の追加
 * 　・確保済みSSHリソースの解放
 *
 * Revision 1.9  2004/12/22 17:28:14  yutakakn
 * SSH2公開鍵認証(RSA/DSA)をサポートした。
 *
 * Revision 1.8  2004/12/17 16:52:36  yutakakn
 * KEXにおけるRSAおよびDSSのkey verify処理を追加。
 *
 * Revision 1.7  2004/12/17 14:28:36  yutakakn
 * メッセージ認証アルゴリズムに HMAC-MD5 を追加。
 * TTSSHバージョンダイアログにHMACアルゴリズム表示を追加。
 *
 * Revision 1.6  2004/12/17 14:05:55  yutakakn
 * パケット受信時のHMACチェックを追加。
 * KEXにおけるHMACアルゴリズムチェックを追加。
 *
 * Revision 1.5  2004/12/11 07:31:00  yutakakn
 * SSH heartbeatスレッドの追加した。これにより、IPマスカレード環境において、ルータの
 * NATテーブルクリアにより、SSHコネクションが切断される現象が回避される。
 * それに合わせて、teraterm.iniのTTSSHセクションに、HeartBeat エントリを追加。
 *
 * Revision 1.4  2004/12/04 08:18:31  yutakakn
 * SSH2自動ログインにおいて、ユーザ認証に失敗した場合、リトライを行わないようにした。
 *
 * Revision 1.3  2004/12/01 15:37:49  yutakakn
 * SSH2自動ログイン機能を追加。
 * 現状、パスワード認証のみに対応。
 * ・コマンドライン
 *   /ssh /auth=認証メソッド /user=ユーザ名 /passwd=パスワード
 *
 * Revision 1.2  2004/11/29 15:52:37  yutakakn
 * SSHのdefault protocolをSSH2にした。
 * 
 */
