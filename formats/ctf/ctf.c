/*
 * BabelTrace - Common Trace Format (CTF)
 *
 * Format registration.
 *
 * Copyright 2010-2011 EfficiOS Inc. and Linux Foundation
 *
 * Author: Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#include <babeltrace/format.h>
#include <babeltrace/ctf/types.h>
#include <babeltrace/ctf/metadata.h>
#include <babeltrace/babeltrace-internal.h>
#include <babeltrace/ctf/events-internal.h>
#include <inttypes.h>
#include <stdio.h>
#include <uuid/uuid.h>
#include <sys/mman.h>
#include <errno.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <glib.h>
#include <unistd.h>
#include <stdlib.h>

#include "metadata/ctf-scanner.h"
#include "metadata/ctf-parser.h"
#include "metadata/ctf-ast.h"
#include "events-private.h"

/*
 * We currently simply map a page to read the packet header and packet
 * context to get the packet length and content length. (in bits)
 */
#define MAX_PACKET_HEADER_LEN	(getpagesize() * CHAR_BIT)
#define WRITE_PACKET_LEN	(getpagesize() * 8 * CHAR_BIT)
#define UUID_LEN 16	/* uuid by value len */

#ifndef min
#define min(a, b)	(((a) < (b)) ? (a) : (b))
#endif

#define NSEC_PER_SEC 1000000000ULL

int opt_clock_raw,
	opt_clock_seconds,
	opt_clock_date,
	opt_clock_gmt;

uint64_t opt_clock_offset;

extern int yydebug;

static
struct trace_descriptor *ctf_open_trace(const char *path, int flags,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence),
		FILE *metadata_fp);
static
struct trace_descriptor *ctf_open_mmap_trace(
		struct mmap_stream_list *mmap_list,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence),
		FILE *metadata_fp);

static
void ctf_close_trace(struct trace_descriptor *descriptor);

static
rw_dispatch read_dispatch_table[] = {
	[ CTF_TYPE_INTEGER ] = ctf_integer_read,
	[ CTF_TYPE_FLOAT ] = ctf_float_read,
	[ CTF_TYPE_ENUM ] = ctf_enum_read,
	[ CTF_TYPE_STRING ] = ctf_string_read,
	[ CTF_TYPE_STRUCT ] = ctf_struct_rw,
	[ CTF_TYPE_VARIANT ] = ctf_variant_rw,
	[ CTF_TYPE_ARRAY ] = ctf_array_read,
	[ CTF_TYPE_SEQUENCE ] = ctf_sequence_read,
};

static
rw_dispatch write_dispatch_table[] = {
	[ CTF_TYPE_INTEGER ] = ctf_integer_write,
	[ CTF_TYPE_FLOAT ] = ctf_float_write,
	[ CTF_TYPE_ENUM ] = ctf_enum_write,
	[ CTF_TYPE_STRING ] = ctf_string_write,
	[ CTF_TYPE_STRUCT ] = ctf_struct_rw,
	[ CTF_TYPE_VARIANT ] = ctf_variant_rw,
	[ CTF_TYPE_ARRAY ] = ctf_array_write,
	[ CTF_TYPE_SEQUENCE ] = ctf_sequence_write,
};

static
struct format ctf_format = {
	.open_trace = ctf_open_trace,
	.open_mmap_trace = ctf_open_mmap_trace,
	.close_trace = ctf_close_trace,
};

/*
 * Update stream current timestamp, keep at clock frequency.
 */
static
void ctf_update_timestamp(struct ctf_stream *stream,
			  struct definition_integer *integer_definition)
{
	struct declaration_integer *integer_declaration =
		integer_definition->declaration;
	uint64_t oldval, newval, updateval;

	if (unlikely(integer_declaration->len == 64)) {
		stream->timestamp = integer_definition->value._unsigned;
		return;
	}
	/* keep low bits */
	oldval = stream->timestamp;
	oldval &= (1ULL << integer_declaration->len) - 1;
	newval = integer_definition->value._unsigned;
	/* Test for overflow by comparing low bits */
	if (newval < oldval)
		newval += 1ULL << integer_declaration->len;
	/* updateval contains old high bits, and new low bits (sum) */
	updateval = stream->timestamp;
	updateval &= ~((1ULL << integer_declaration->len) - 1);
	updateval += newval;
	stream->prev_timestamp = stream->timestamp;
	stream->timestamp = updateval;
}

/*
 * Print timestamp, rescaling clock frequency to nanoseconds and
 * applying offsets as needed (unix time).
 */
void ctf_print_timestamp(FILE *fp,
			struct ctf_stream *stream,
			uint64_t timestamp)
{
	uint64_t ts_sec = 0, ts_nsec;

	if (opt_clock_raw) {
		ts_nsec = ctf_get_timestamp_raw(stream, timestamp);
	} else {
		ts_nsec = ctf_get_timestamp(stream, timestamp);
	}

	/* Add command-line offset */
	ts_sec += opt_clock_offset;

	ts_sec += ts_nsec / NSEC_PER_SEC;
	ts_nsec = ts_nsec % NSEC_PER_SEC;

	if (!opt_clock_seconds) {
		struct tm tm;
		time_t time_s = (time_t) ts_sec;

		if (!opt_clock_gmt) {
			struct tm *res;

			res = localtime_r(&time_s, &tm);
			if (!res) {
				fprintf(stderr, "[warning] Unable to get localtime.\n");
				goto seconds;
			}
		} else {
			struct tm *res;

			res = gmtime_r(&time_s, &tm);
			if (!res) {
				fprintf(stderr, "[warning] Unable to get gmtime.\n");
				goto seconds;
			}
		}
		if (opt_clock_date) {
			char timestr[26];
			size_t res;

			/* Print date and time */
			res = strftime(timestr, sizeof(timestr),
				"%F ", &tm);
			if (!res) {
				fprintf(stderr, "[warning] Unable to print ascii time.\n");
				goto seconds;
			}
			fprintf(fp, "%s", timestr);
		}
		/* Print time in HH:MM:SS.ns */
		fprintf(fp, "%02d:%02d:%02d.%09" PRIu64,
			tm.tm_hour, tm.tm_min, tm.tm_sec, ts_nsec);
		goto end;
	}
seconds:
	fprintf(fp, "%3" PRIu64 ".%09" PRIu64,
		ts_sec, ts_nsec);

end:
	return;
}

static
int ctf_read_event(struct stream_pos *ppos, struct ctf_stream *stream)
{
	struct ctf_stream_pos *pos =
		container_of(ppos, struct ctf_stream_pos, parent);
	struct ctf_stream_class *stream_class = stream->stream_class;
	struct ctf_stream_event *event;
	uint64_t id = 0;
	int ret;

	/* We need to check for EOF here for empty files. */
	if (unlikely(pos->offset == EOF))
		return EOF;

	ctf_pos_get_event(pos);

	/* save the current position as a restore point */
	pos->last_offset = pos->offset;
	/* we just read the event, it is consumed when used by the caller */
	stream->consumed = 0;

	/*
	 * This is the EOF check after we've advanced the position in
	 * ctf_pos_get_event.
	 */
	if (unlikely(pos->offset == EOF))
		return EOF;
	assert(pos->offset < pos->content_size);

	/* Read event header */
	if (likely(stream->stream_event_header)) {
		struct definition_integer *integer_definition;
		struct definition *variant;

		ret = generic_rw(ppos, &stream->stream_event_header->p);
		if (unlikely(ret))
			goto error;
		/* lookup event id */
		integer_definition = lookup_integer(&stream->stream_event_header->p, "id", FALSE);
		if (integer_definition) {
			id = integer_definition->value._unsigned;
		} else {
			struct definition_enum *enum_definition;

			enum_definition = lookup_enum(&stream->stream_event_header->p, "id", FALSE);
			if (enum_definition) {
				id = enum_definition->integer->value._unsigned;
			}
		}

		variant = lookup_variant(&stream->stream_event_header->p, "v");
		if (variant) {
			integer_definition = lookup_integer(variant, "id", FALSE);
			if (integer_definition) {
				id = integer_definition->value._unsigned;
			}
		}
		stream->event_id = id;

		/* lookup timestamp */
		stream->has_timestamp = 0;
		integer_definition = lookup_integer(&stream->stream_event_header->p, "timestamp", FALSE);
		if (integer_definition) {
			ctf_update_timestamp(stream, integer_definition);
			stream->has_timestamp = 1;
		} else {
			if (variant) {
				integer_definition = lookup_integer(variant, "timestamp", FALSE);
				if (integer_definition) {
					ctf_update_timestamp(stream, integer_definition);
					stream->has_timestamp = 1;
				}
			}
		}
	}

	/* Read stream-declared event context */
	if (stream->stream_event_context) {
		ret = generic_rw(ppos, &stream->stream_event_context->p);
		if (ret)
			goto error;
	}

	if (unlikely(id >= stream_class->events_by_id->len)) {
		fprintf(stderr, "[error] Event id %" PRIu64 " is outside range.\n", id);
		return -EINVAL;
	}
	event = g_ptr_array_index(stream->events_by_id, id);
	if (unlikely(!event)) {
		fprintf(stderr, "[error] Event id %" PRIu64 " is unknown.\n", id);
		return -EINVAL;
	}

	/* Read event-declared event context */
	if (event->event_context) {
		ret = generic_rw(ppos, &event->event_context->p);
		if (ret)
			goto error;
	}

	/* Read event payload */
	if (likely(event->event_fields)) {
		ret = generic_rw(ppos, &event->event_fields->p);
		if (ret)
			goto error;
	}

	return 0;

error:
	fprintf(stderr, "[error] Unexpected end of stream. Either the trace data stream is corrupted or metadata description does not match data layout.\n");
	return ret;
}

static
int ctf_write_event(struct stream_pos *pos, struct ctf_stream *stream)
{
	struct ctf_stream_class *stream_class = stream->stream_class;
	struct ctf_stream_event *event;
	uint64_t id;
	int ret;

	id = stream->event_id;

	/* print event header */
	if (likely(stream->stream_event_header)) {
		ret = generic_rw(pos, &stream->stream_event_header->p);
		if (ret)
			goto error;
	}

	/* print stream-declared event context */
	if (stream->stream_event_context) {
		ret = generic_rw(pos, &stream->stream_event_context->p);
		if (ret)
			goto error;
	}

	if (unlikely(id >= stream_class->events_by_id->len)) {
		fprintf(stderr, "[error] Event id %" PRIu64 " is outside range.\n", id);
		return -EINVAL;
	}
	event = g_ptr_array_index(stream->events_by_id, id);
	if (unlikely(!event)) {
		fprintf(stderr, "[error] Event id %" PRIu64 " is unknown.\n", id);
		return -EINVAL;
	}

	/* print event-declared event context */
	if (event->event_context) {
		ret = generic_rw(pos, &event->event_context->p);
		if (ret)
			goto error;
	}

	/* Read and print event payload */
	if (likely(event->event_fields)) {
		ret = generic_rw(pos, &event->event_fields->p);
		if (ret)
			goto error;
	}

	return 0;

error:
	fprintf(stderr, "[error] Unexpected end of stream. Either the trace data stream is corrupted or metadata description does not match data layout.\n");
	return ret;
}

void ctf_init_pos(struct ctf_stream_pos *pos, int fd, int open_flags)
{
	pos->fd = fd;
	pos->mmap_offset = 0;
	pos->packet_size = 0;
	pos->content_size = 0;
	pos->content_size_loc = NULL;
	pos->base = NULL;
	pos->offset = 0;
	pos->dummy = false;
	pos->cur_index = 0;
	if (fd >= 0)
		pos->packet_index = g_array_new(FALSE, TRUE,
						sizeof(struct packet_index));
	else
		pos->packet_index = NULL;
	switch (open_flags & O_ACCMODE) {
	case O_RDONLY:
		pos->prot = PROT_READ;
		pos->flags = MAP_PRIVATE;
		pos->parent.rw_table = read_dispatch_table;
		pos->parent.event_cb = ctf_read_event;
		break;
	case O_RDWR:
		pos->prot = PROT_WRITE;	/* Write has priority */
		pos->flags = MAP_SHARED;
		pos->parent.rw_table = write_dispatch_table;
		pos->parent.event_cb = ctf_write_event;
		if (fd >= 0)
			ctf_packet_seek(&pos->parent, 0, SEEK_SET);	/* position for write */
		break;
	default:
		assert(0);
	}
}

void ctf_fini_pos(struct ctf_stream_pos *pos)
{
	int ret;

	if (pos->prot == PROT_WRITE && pos->content_size_loc)
		*pos->content_size_loc = pos->offset;
	if (pos->base) {
		/* unmap old base */
		ret = munmap(pos->base, pos->packet_size / CHAR_BIT);
		if (ret) {
			fprintf(stderr, "[error] Unable to unmap old base: %s.\n",
				strerror(errno));
			assert(0);
		}
	}
	(void) g_array_free(pos->packet_index, TRUE);
}

/*
 * for SEEK_CUR: go to next packet.
 * for SEEK_POS: go to packet numer (index).
 */
void ctf_packet_seek(struct stream_pos *stream_pos, size_t index, int whence)
{
	struct ctf_stream_pos *pos =
		container_of(stream_pos, struct ctf_stream_pos, parent);
	struct ctf_file_stream *file_stream =
		container_of(pos, struct ctf_file_stream, pos);
	int ret;
	off_t off;
	struct packet_index *packet_index;

	if (pos->prot == PROT_WRITE && pos->content_size_loc)
		*pos->content_size_loc = pos->offset;

	if (pos->base) {
		/* unmap old base */
		ret = munmap(pos->base, pos->packet_size / CHAR_BIT);
		if (ret) {
			fprintf(stderr, "[error] Unable to unmap old base: %s.\n",
				strerror(errno));
			assert(0);
		}
		pos->base = NULL;
	}

	/*
	 * The caller should never ask for ctf_move_pos across packets,
	 * except to get exactly at the beginning of the next packet.
	 */
	if (pos->prot == PROT_WRITE) {
		switch (whence) {
		case SEEK_CUR:
			/* The writer will add padding */
			pos->mmap_offset += WRITE_PACKET_LEN / CHAR_BIT;
			break;
		case SEEK_SET:
			assert(index == 0);	/* only seek supported for now */
			pos->cur_index = 0;
			break;
		default:
			assert(0);
		}
		pos->content_size = -1U;	/* Unknown at this point */
		pos->packet_size = WRITE_PACKET_LEN;
		off = posix_fallocate(pos->fd, pos->mmap_offset,
				      pos->packet_size / CHAR_BIT);
		assert(off >= 0);
		pos->offset = 0;
	} else {
	read_next_packet:
		switch (whence) {
		case SEEK_CUR:
		{
			uint32_t events_discarded_diff;

			if (pos->offset == EOF) {
				return;
			}
			/* For printing discarded event count */
			packet_index = &g_array_index(pos->packet_index,
					struct packet_index, pos->cur_index);
			events_discarded_diff = packet_index->events_discarded;
			file_stream->parent.prev_timestamp_end =
						packet_index->timestamp_end;
			if (pos->cur_index > 0) {
				packet_index = &g_array_index(pos->packet_index,
						struct packet_index,
						pos->cur_index - 1);
				events_discarded_diff -= packet_index->events_discarded;
			}
			file_stream->parent.events_discarded = events_discarded_diff;
			file_stream->parent.prev_timestamp = file_stream->parent.timestamp;
			/* The reader will expect us to skip padding */
			++pos->cur_index;
			break;
		}
		case SEEK_SET:
			pos->cur_index = index;
			file_stream->parent.prev_timestamp = 0;
			file_stream->parent.prev_timestamp_end = 0;
			break;
		default:
			assert(0);
		}
		if (pos->cur_index >= pos->packet_index->len) {
			/*
			 * When a stream reaches the end of the
			 * file, we need to show the number of
			 * events discarded ourselves, because
			 * there is no next event scheduled to
			 * be printed in the output.
			 */
			if (file_stream->parent.events_discarded) {
				/*
				 * We need to check if we are in trace
				 * read or called from packet indexing.
				 * In this last case, the collection is
				 * not there, so we cannot print the
				 * timestamps.
				 */
				if ((&file_stream->parent)->stream_class->trace->collection) {
					fflush(stdout);
					fprintf(stderr, "[warning] Tracer discarded %d events at end of stream between [",
							file_stream->parent.events_discarded);
					ctf_print_timestamp(stderr, &file_stream->parent,
							file_stream->parent.prev_timestamp);
					fprintf(stderr, "] and [");
					ctf_print_timestamp(stderr, &file_stream->parent,
							file_stream->parent.prev_timestamp_end);
					fprintf(stderr, "]. You should consider increasing the buffer size.\n");
					fflush(stderr);
				}
				file_stream->parent.events_discarded = 0;
			}
			pos->offset = EOF;
			return;
		}
		packet_index = &g_array_index(pos->packet_index, struct packet_index,
				       pos->cur_index);
		pos->mmap_offset = packet_index->offset;

		/* Lookup context/packet size in index */
		file_stream->parent.timestamp = packet_index->timestamp_begin;
		pos->content_size = packet_index->content_size;
		pos->packet_size = packet_index->packet_size;
		if (packet_index->data_offset < packet_index->content_size) {
			pos->offset = 0;	/* will read headers */
		} else if (packet_index->data_offset == packet_index->content_size) {
			/* empty packet */
			pos->offset = packet_index->data_offset;
			whence = SEEK_CUR;
			goto read_next_packet;
		} else {
			pos->offset = EOF;
			return;
		}
	}
	/* map new base. Need mapping length from header. */
	pos->base = mmap(NULL, pos->packet_size / CHAR_BIT, pos->prot,
			 pos->flags, pos->fd, pos->mmap_offset);
	if (pos->base == MAP_FAILED) {
		fprintf(stderr, "[error] mmap error %s.\n",
			strerror(errno));
		assert(0);
	}

	/* update trace_packet_header and stream_packet_context */
	if (pos->prot != PROT_WRITE && file_stream->parent.trace_packet_header) {
		/* Read packet header */
		ret = generic_rw(&pos->parent, &file_stream->parent.trace_packet_header->p);
		assert(!ret);
	}
	if (pos->prot != PROT_WRITE && file_stream->parent.stream_packet_context) {
		/* Read packet context */
		ret = generic_rw(&pos->parent, &file_stream->parent.stream_packet_context->p);
		assert(!ret);
	}
}

static
int packet_metadata(struct ctf_trace *td, FILE *fp)
{
	uint32_t magic;
	size_t len;
	int ret = 0;

	len = fread(&magic, sizeof(magic), 1, fp);
	if (len != 1) {
		goto end;
	}
	if (magic == TSDL_MAGIC) {
		ret = 1;
		td->byte_order = BYTE_ORDER;
		CTF_TRACE_SET_FIELD(td, byte_order);
	} else if (magic == GUINT32_SWAP_LE_BE(TSDL_MAGIC)) {
		ret = 1;
		td->byte_order = (BYTE_ORDER == BIG_ENDIAN) ?
					LITTLE_ENDIAN : BIG_ENDIAN;
		CTF_TRACE_SET_FIELD(td, byte_order);
	}
end:
	rewind(fp);
	return ret;
}

/*
 * Returns 0 on success, -1 on error.
 */
static
int check_version(unsigned int major, unsigned int minor)
{
	switch (major) {
	case 1:
		switch (minor) {
		case 8:
			return 0;
		default:
			goto warning;
		}
	default:
		goto warning;
		
	}

	/* eventually return an error instead of warning */
warning:
	fprintf(stderr, "[warning] Unsupported CTF specification version %u.%u. Trying anyway.\n",
		major, minor);
	return 0;
}

static
int ctf_open_trace_metadata_packet_read(struct ctf_trace *td, FILE *in,
					FILE *out)
{
	struct metadata_packet_header header;
	size_t readlen, writelen, toread;
	char buf[4096 + 1];	/* + 1 for debug-mode \0 */
	int ret = 0;

	readlen = fread(&header, header_sizeof(header), 1, in);
	if (readlen < 1)
		return -EINVAL;

	if (td->byte_order != BYTE_ORDER) {
		header.magic = GUINT32_SWAP_LE_BE(header.magic);
		header.checksum = GUINT32_SWAP_LE_BE(header.checksum);
		header.content_size = GUINT32_SWAP_LE_BE(header.content_size);
		header.packet_size = GUINT32_SWAP_LE_BE(header.packet_size);
	}
	if (header.checksum)
		fprintf(stderr, "[warning] checksum verification not supported yet.\n");
	if (header.compression_scheme) {
		fprintf(stderr, "[error] compression (%u) not supported yet.\n",
			header.compression_scheme);
		return -EINVAL;
	}
	if (header.encryption_scheme) {
		fprintf(stderr, "[error] encryption (%u) not supported yet.\n",
			header.encryption_scheme);
		return -EINVAL;
	}
	if (header.checksum_scheme) {
		fprintf(stderr, "[error] checksum (%u) not supported yet.\n",
			header.checksum_scheme);
		return -EINVAL;
	}
	if (check_version(header.major, header.minor) < 0)
		return -EINVAL;
	if (!CTF_TRACE_FIELD_IS_SET(td, uuid)) {
		memcpy(td->uuid, header.uuid, sizeof(header.uuid));
		CTF_TRACE_SET_FIELD(td, uuid);
	} else {
		if (uuid_compare(header.uuid, td->uuid))
			return -EINVAL;
	}

	toread = (header.content_size / CHAR_BIT) - header_sizeof(header);

	for (;;) {
		readlen = fread(buf, sizeof(char), min(sizeof(buf) - 1, toread), in);
		if (ferror(in)) {
			ret = -EINVAL;
			break;
		}
		if (babeltrace_debug) {
			buf[readlen] = '\0';
			fprintf(stderr, "[debug] metadata packet read: %s\n",
				buf);
		}

		writelen = fwrite(buf, sizeof(char), readlen, out);
		if (writelen < readlen) {
			ret = -EIO;
			break;
		}
		if (ferror(out)) {
			ret = -EINVAL;
			break;
		}
		toread -= readlen;
		if (!toread) {
			ret = 0;	/* continue reading next packet */
			goto read_padding;
		}
	}
	return ret;

read_padding:
	toread = (header.packet_size - header.content_size) / CHAR_BIT;
	ret = fseek(in, toread, SEEK_CUR);
	if (ret < 0) {
		fprintf(stderr, "[warning] Missing padding at end of file\n");
		ret = 0;
	}
	return ret;
}

static
int ctf_open_trace_metadata_stream_read(struct ctf_trace *td, FILE **fp,
					char **buf)
{
	FILE *in, *out;
	size_t size;
	int ret;

	in = *fp;
	/*
	 * Using strlen on *buf instead of size of open_memstream
	 * because its size includes garbage at the end (after final
	 * \0). This is the allocated size, not the actual string size.
	 */
	out = open_memstream(buf, &size);
	if (out == NULL) {
		perror("Metadata open_memstream");
		return -errno;
	}
	for (;;) {
		ret = ctf_open_trace_metadata_packet_read(td, in, out);
		if (ret) {
			break;
		}
		if (feof(in)) {
			ret = 0;
			break;
		}
	}
	fclose(out);	/* flush the buffer */
	fclose(in);
	/* open for reading */
	*fp = fmemopen(*buf, strlen(*buf), "rb");
	if (!*fp) {
		perror("Metadata fmemopen");
		return -errno;
	}
	return 0;
}

static
int ctf_open_trace_metadata_read(struct ctf_trace *td,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence), FILE *metadata_fp)
{
	struct ctf_scanner *scanner;
	struct ctf_file_stream *metadata_stream;
	FILE *fp;
	char *buf = NULL;
	int ret = 0;

	metadata_stream = g_new0(struct ctf_file_stream, 1);

	if (packet_seek) {
		metadata_stream->pos.packet_seek = packet_seek;
	} else {
		fprintf(stderr, "[error] packet_seek function undefined.\n");
		ret = -1;
		goto end_stream;
	}

	if (metadata_fp) {
		fp = metadata_fp;
	} else {
		td->metadata = &metadata_stream->parent;
		metadata_stream->pos.fd = openat(td->dirfd, "metadata", O_RDONLY);
		if (metadata_stream->pos.fd < 0) {
			fprintf(stderr, "Unable to open metadata.\n");
			return metadata_stream->pos.fd;
		}

		fp = fdopen(metadata_stream->pos.fd, "r");
		if (!fp) {
			fprintf(stderr, "[error] Unable to open metadata stream.\n");
			perror("Metadata stream open");
			ret = -errno;
			goto end_stream;
		}
	}
	if (babeltrace_debug)
		yydebug = 1;

	if (packet_metadata(td, fp)) {
		ret = ctf_open_trace_metadata_stream_read(td, &fp, &buf);
		if (ret)
			goto end_packet_read;
	} else {
		unsigned int major, minor;
		ssize_t nr_items;

		td->byte_order = BYTE_ORDER;

		/* Check text-only metadata header and version */
		nr_items = fscanf(fp, "/* CTF %u.%u", &major, &minor);
		if (nr_items < 2)
			fprintf(stderr, "[warning] Ill-shapen or missing \"/* CTF x.y\" header for text-only metadata.\n");
		if (check_version(major, minor) < 0) {
			ret = -EINVAL;
			goto end_packet_read;
		}
		rewind(fp);
	}

	scanner = ctf_scanner_alloc(fp);
	if (!scanner) {
		fprintf(stderr, "[error] Error allocating scanner\n");
		ret = -ENOMEM;
		goto end_scanner_alloc;
	}
	ret = ctf_scanner_append_ast(scanner);
	if (ret) {
		fprintf(stderr, "[error] Error creating AST\n");
		goto end;
	}

	if (babeltrace_debug) {
		ret = ctf_visitor_print_xml(stderr, 0, &scanner->ast->root);
		if (ret) {
			fprintf(stderr, "[error] Error visiting AST for XML output\n");
			goto end;
		}
	}

	ret = ctf_visitor_semantic_check(stderr, 0, &scanner->ast->root);
	if (ret) {
		fprintf(stderr, "[error] Error in CTF semantic validation %d\n", ret);
		goto end;
	}
	ret = ctf_visitor_construct_metadata(stderr, 0, &scanner->ast->root,
			td, td->byte_order);
	if (ret) {
		fprintf(stderr, "[error] Error in CTF metadata constructor %d\n", ret);
		goto end;
	}
end:
	ctf_scanner_free(scanner);
end_scanner_alloc:
end_packet_read:
	fclose(fp);
	free(buf);
end_stream:
	close(metadata_stream->pos.fd);
	if (ret)
		g_free(metadata_stream);
	return ret;
}

static
struct ctf_stream_event *create_event_definitions(struct ctf_trace *td,
						  struct ctf_stream *stream,
						  struct ctf_event *event)
{
	struct ctf_stream_event *stream_event = g_new0(struct ctf_stream_event, 1);

	if (event->context_decl) {
		struct definition *definition =
			event->context_decl->p.definition_new(&event->context_decl->p,
				stream->parent_def_scope, 0, 0, "event.context");
		if (!definition) {
			goto error;
		}
		stream_event->event_context = container_of(definition,
					struct definition_struct, p);
		stream->parent_def_scope = stream_event->event_context->p.scope;
	}
	if (event->fields_decl) {
		struct definition *definition =
			event->fields_decl->p.definition_new(&event->fields_decl->p,
				stream->parent_def_scope, 0, 0, "event.fields");
		if (!definition) {
			goto error;
		}
		stream_event->event_fields = container_of(definition,
					struct definition_struct, p);
		stream->parent_def_scope = stream_event->event_fields->p.scope;
	}
	return stream_event;

error:
	if (stream_event->event_fields)
		definition_unref(&stream_event->event_fields->p);
	if (stream_event->event_context)
		definition_unref(&stream_event->event_context->p);
	return NULL;
}

static
int create_stream_definitions(struct ctf_trace *td, struct ctf_stream *stream)
{
	struct ctf_stream_class *stream_class;
	int ret;
	int i;

	if (stream->stream_definitions_created)
		return 0;

	stream_class = stream->stream_class;

	if (stream_class->packet_context_decl) {
		struct definition *definition =
			stream_class->packet_context_decl->p.definition_new(&stream_class->packet_context_decl->p,
				stream->parent_def_scope, 0, 0, "stream.packet.context");
		if (!definition) {
			ret = -EINVAL;
			goto error;
		}
		stream->stream_packet_context = container_of(definition,
						struct definition_struct, p);
		stream->parent_def_scope = stream->stream_packet_context->p.scope;
	}
	if (stream_class->event_header_decl) {
		struct definition *definition =
			stream_class->event_header_decl->p.definition_new(&stream_class->event_header_decl->p,
				stream->parent_def_scope, 0, 0, "stream.event.header");
		if (!definition) {
			ret = -EINVAL;
			goto error;
		}
		stream->stream_event_header =
			container_of(definition, struct definition_struct, p);
		stream->parent_def_scope = stream->stream_event_header->p.scope;
	}
	if (stream_class->event_context_decl) {
		struct definition *definition =
			stream_class->event_context_decl->p.definition_new(&stream_class->event_context_decl->p,
				stream->parent_def_scope, 0, 0, "stream.event.context");
		if (!definition) {
			ret = -EINVAL;
			goto error;
		}
		stream->stream_event_context =
			container_of(definition, struct definition_struct, p);
		stream->parent_def_scope = stream->stream_event_context->p.scope;
	}
	stream->events_by_id = g_ptr_array_new();
	g_ptr_array_set_size(stream->events_by_id, stream_class->events_by_id->len);
	for (i = 0; i < stream->events_by_id->len; i++) {
		struct ctf_event *event = g_ptr_array_index(stream_class->events_by_id, i);
		struct ctf_stream_event *stream_event;

		if (!event)
			continue;
		stream_event = create_event_definitions(td, stream, event);
		if (!stream_event)
			goto error_event;
		g_ptr_array_index(stream->events_by_id, i) = stream_event;
	}
	return 0;

error_event:
	for (i = 0; i < stream->events_by_id->len; i++) {
		struct ctf_stream_event *stream_event = g_ptr_array_index(stream->events_by_id, i);
		if (stream_event)
			g_free(stream_event);
	}
	g_ptr_array_free(stream->events_by_id, TRUE);
error:
	if (stream->stream_event_context)
		definition_unref(&stream->stream_event_context->p);
	if (stream->stream_event_header)
		definition_unref(&stream->stream_event_header->p);
	if (stream->stream_packet_context)
		definition_unref(&stream->stream_packet_context->p);
	return ret;
}


static
int create_stream_packet_index(struct ctf_trace *td,
			       struct ctf_file_stream *file_stream)
{
	struct ctf_stream_class *stream;
	int len_index;
	struct ctf_stream_pos *pos;
	struct stat filestats;
	struct packet_index packet_index;
	int first_packet = 1;
	int ret;

	pos = &file_stream->pos;

	ret = fstat(pos->fd, &filestats);
	if (ret < 0)
		return ret;

	if (filestats.st_size < MAX_PACKET_HEADER_LEN / CHAR_BIT)
		return -EINVAL;

	for (pos->mmap_offset = 0; pos->mmap_offset < filestats.st_size; ) {
		uint64_t stream_id = 0;

		if (pos->base) {
			/* unmap old base */
			ret = munmap(pos->base, pos->packet_size / CHAR_BIT);
			if (ret) {
				fprintf(stderr, "[error] Unable to unmap old base: %s.\n",
					strerror(errno));
				return ret;
			}
			pos->base = NULL;
		}
		/* map new base. Need mapping length from header. */
		pos->base = mmap(NULL, MAX_PACKET_HEADER_LEN / CHAR_BIT, PROT_READ,
				 MAP_PRIVATE, pos->fd, pos->mmap_offset);
		pos->content_size = MAX_PACKET_HEADER_LEN;	/* Unknown at this point */
		pos->packet_size = MAX_PACKET_HEADER_LEN;	/* Unknown at this point */
		pos->offset = 0;	/* Position of the packet header */

		packet_index.offset = pos->mmap_offset;
		packet_index.content_size = 0;
		packet_index.packet_size = 0;
		packet_index.timestamp_begin = 0;
		packet_index.timestamp_end = 0;
		packet_index.events_discarded = 0;

		/* read and check header, set stream id (and check) */
		if (file_stream->parent.trace_packet_header) {
			/* Read packet header */
			ret = generic_rw(&pos->parent, &file_stream->parent.trace_packet_header->p);
			if (ret)
				return ret;
			len_index = struct_declaration_lookup_field_index(file_stream->parent.trace_packet_header->declaration, g_quark_from_static_string("magic"));
			if (len_index >= 0) {
				struct definition *field;
				uint64_t magic;

				field = struct_definition_get_field_from_index(file_stream->parent.trace_packet_header, len_index);
				magic = get_unsigned_int(field);
				if (magic != CTF_MAGIC) {
					fprintf(stderr, "[error] Invalid magic number 0x%" PRIX64 " at packet %u (file offset %zd).\n",
							magic,
							file_stream->pos.packet_index->len,
							(ssize_t) pos->mmap_offset);
					return -EINVAL;
				}
			}

			/* check uuid */
			len_index = struct_declaration_lookup_field_index(file_stream->parent.trace_packet_header->declaration, g_quark_from_static_string("uuid"));
			if (len_index >= 0) {
				struct definition_array *defarray;
				struct definition *field;
				uint64_t i;
				uint8_t uuidval[UUID_LEN];

				field = struct_definition_get_field_from_index(file_stream->parent.trace_packet_header, len_index);
				assert(field->declaration->id == CTF_TYPE_ARRAY);
				defarray = container_of(field, struct definition_array, p);
				assert(array_len(defarray) == UUID_LEN);

				for (i = 0; i < UUID_LEN; i++) {
					struct definition *elem;

					elem = array_index(defarray, i);
					uuidval[i] = get_unsigned_int(elem);
				}
				ret = uuid_compare(td->uuid, uuidval);
				if (ret) {
					fprintf(stderr, "[error] Unique Universal Identifiers do not match.\n");
					return -EINVAL;
				}
			}


			len_index = struct_declaration_lookup_field_index(file_stream->parent.trace_packet_header->declaration, g_quark_from_static_string("stream_id"));
			if (len_index >= 0) {
				struct definition *field;

				field = struct_definition_get_field_from_index(file_stream->parent.trace_packet_header, len_index);
				stream_id = get_unsigned_int(field);
			}
		}

		if (!first_packet && file_stream->parent.stream_id != stream_id) {
			fprintf(stderr, "[error] Stream ID is changing within a stream.\n");
			return -EINVAL;
		}
		if (first_packet) {
			file_stream->parent.stream_id = stream_id;
			if (stream_id >= td->streams->len) {
				fprintf(stderr, "[error] Stream %" PRIu64 " is not declared in metadata.\n", stream_id);
				return -EINVAL;
			}
			stream = g_ptr_array_index(td->streams, stream_id);
			if (!stream) {
				fprintf(stderr, "[error] Stream %" PRIu64 " is not declared in metadata.\n", stream_id);
				return -EINVAL;
			}
			file_stream->parent.stream_class = stream;
			ret = create_stream_definitions(td, &file_stream->parent);
			if (ret)
				return ret;
		}
		first_packet = 0;

		if (file_stream->parent.stream_packet_context) {
			/* Read packet context */
			ret = generic_rw(&pos->parent, &file_stream->parent.stream_packet_context->p);
			if (ret)
				return ret;
			/* read content size from header */
			len_index = struct_declaration_lookup_field_index(file_stream->parent.stream_packet_context->declaration, g_quark_from_static_string("content_size"));
			if (len_index >= 0) {
				struct definition *field;

				field = struct_definition_get_field_from_index(file_stream->parent.stream_packet_context, len_index);
				packet_index.content_size = get_unsigned_int(field);
			} else {
				/* Use file size for packet size */
				packet_index.content_size = filestats.st_size * CHAR_BIT;
			}

			/* read packet size from header */
			len_index = struct_declaration_lookup_field_index(file_stream->parent.stream_packet_context->declaration, g_quark_from_static_string("packet_size"));
			if (len_index >= 0) {
				struct definition *field;

				field = struct_definition_get_field_from_index(file_stream->parent.stream_packet_context, len_index);
				packet_index.packet_size = get_unsigned_int(field);
			} else {
				/* Use content size if non-zero, else file size */
				packet_index.packet_size = packet_index.content_size ? : filestats.st_size * CHAR_BIT;
			}

			/* read timestamp begin from header */
			len_index = struct_declaration_lookup_field_index(file_stream->parent.stream_packet_context->declaration, g_quark_from_static_string("timestamp_begin"));
			if (len_index >= 0) {
				struct definition *field;

				field = struct_definition_get_field_from_index(file_stream->parent.stream_packet_context, len_index);
				packet_index.timestamp_begin = get_unsigned_int(field);
			}

			/* read timestamp end from header */
			len_index = struct_declaration_lookup_field_index(file_stream->parent.stream_packet_context->declaration, g_quark_from_static_string("timestamp_end"));
			if (len_index >= 0) {
				struct definition *field;

				field = struct_definition_get_field_from_index(file_stream->parent.stream_packet_context, len_index);
				packet_index.timestamp_end = get_unsigned_int(field);
			}

			/* read events discarded from header */
			len_index = struct_declaration_lookup_field_index(file_stream->parent.stream_packet_context->declaration, g_quark_from_static_string("events_discarded"));
			if (len_index >= 0) {
				struct definition *field;

				field = struct_definition_get_field_from_index(file_stream->parent.stream_packet_context, len_index);
				packet_index.events_discarded = get_unsigned_int(field);
			}
		} else {
			/* Use file size for packet size */
			packet_index.content_size = filestats.st_size * CHAR_BIT;
			/* Use content size if non-zero, else file size */
			packet_index.packet_size = packet_index.content_size ? : filestats.st_size * CHAR_BIT;
		}

		/* Validate content size and packet size values */
		if (packet_index.content_size > packet_index.packet_size) {
			fprintf(stderr, "[error] Content size (%" PRIu64 " bits) is larger than packet size (%" PRIu64 " bits).\n",
				packet_index.content_size, packet_index.packet_size);
			return -EINVAL;
		}

		if (packet_index.packet_size > ((uint64_t)filestats.st_size - packet_index.offset) * CHAR_BIT) {
			fprintf(stderr, "[error] Packet size (%" PRIu64 " bits) is larger than remaining file size (%" PRIu64 " bits).\n",
				packet_index.content_size, ((uint64_t)filestats.st_size - packet_index.offset) * CHAR_BIT);
			return -EINVAL;
		}

		/* Save position after header and context */
		packet_index.data_offset = pos->offset;

		/* add index to packet array */
		g_array_append_val(file_stream->pos.packet_index, packet_index);

		pos->mmap_offset += packet_index.packet_size / CHAR_BIT;
	}

	/* Move pos back to beginning of file */
	ctf_packet_seek(&pos->parent, 0, SEEK_SET);	/* position for write */

	return 0;
}

static
int create_trace_definitions(struct ctf_trace *td, struct ctf_stream *stream)
{
	int ret;

	if (td->packet_header_decl) {
		struct definition *definition =
			td->packet_header_decl->p.definition_new(&td->packet_header_decl->p,
				stream->parent_def_scope, 0, 0, "trace.packet.header");
		if (!definition) {
			ret = -EINVAL;
			goto error;
		}
		stream->trace_packet_header = 
			container_of(definition, struct definition_struct, p);
		stream->parent_def_scope = stream->trace_packet_header->p.scope;
	}

	return 0;

error:
	return ret;
}

/*
 * Note: many file streams can inherit from the same stream class
 * description (metadata).
 */
static
int ctf_open_file_stream_read(struct ctf_trace *td, const char *path, int flags,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence))
{
	int ret;
	struct ctf_file_stream *file_stream;

	ret = openat(td->dirfd, path, flags);
	if (ret < 0) {
		perror("File stream openat()");
		goto error;
	}
	file_stream = g_new0(struct ctf_file_stream, 1);

	if (packet_seek) {
		file_stream->pos.packet_seek = packet_seek;
	} else {
		fprintf(stderr, "[error] packet_seek function undefined.\n");
		ret = -1;
		goto error_def;
	}

	ctf_init_pos(&file_stream->pos, ret, flags);
	ret = create_trace_definitions(td, &file_stream->parent);
	if (ret)
		goto error_def;
	/*
	 * For now, only a single slock is supported.
	 */
	file_stream->parent.current_clock = td->single_clock;
	ret = create_stream_packet_index(td, file_stream);
	if (ret)
		goto error_index;
	/* Add stream file to stream class */
	g_ptr_array_add(file_stream->parent.stream_class->streams,
			&file_stream->parent);
	return 0;

error_index:
	if (file_stream->parent.trace_packet_header)
		definition_unref(&file_stream->parent.trace_packet_header->p);
error_def:
	ctf_fini_pos(&file_stream->pos);
	close(file_stream->pos.fd);
	g_free(file_stream);
error:
	return ret;
}

static
int ctf_open_trace_read(struct ctf_trace *td,
		const char *path, int flags,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence), FILE *metadata_fp)
{
	int ret;
	struct dirent *dirent;
	struct dirent *diriter;
	size_t dirent_len;

	td->flags = flags;

	/* Open trace directory */
	td->dir = opendir(path);
	if (!td->dir) {
		fprintf(stderr, "[error] Unable to open trace directory.\n");
		ret = -ENOENT;
		goto error;
	}

	td->dirfd = open(path, 0);
	if (td->dirfd < 0) {
		fprintf(stderr, "[error] Unable to open trace directory file descriptor.\n");
		perror("Trace directory open");
		ret = -errno;
		goto error_dirfd;
	}
	strncpy(td->path, path, sizeof(td->path));
	td->path[sizeof(td->path) - 1] = '\0';

	/*
	 * Keep the metadata file separate.
	 */

	ret = ctf_open_trace_metadata_read(td, packet_seek, metadata_fp);
	if (ret) {
		goto error_metadata;
	}

	/*
	 * Open each stream: for each file, try to open, check magic
	 * number, and get the stream ID to add to the right location in
	 * the stream array.
	 */

	dirent_len = offsetof(struct dirent, d_name) +
			fpathconf(td->dirfd, _PC_NAME_MAX) + 1;

	dirent = malloc(dirent_len);

	for (;;) {
		ret = readdir_r(td->dir, dirent, &diriter);
		if (ret) {
			fprintf(stderr, "[error] Readdir error.\n");
			goto readdir_error;
		}
		if (!diriter)
			break;
		/* Ignore hidden files, ., .. and metadata. */
		if (!strncmp(diriter->d_name, ".", 1)
				|| !strcmp(diriter->d_name, "..")
				|| !strcmp(diriter->d_name, "metadata"))
			continue;
		ret = ctf_open_file_stream_read(td, diriter->d_name,
					flags, packet_seek);
		if (ret) {
			fprintf(stderr, "[error] Open file stream error.\n");
			goto readdir_error;
		}
	}

	free(dirent);
	return 0;

readdir_error:
	free(dirent);
error_metadata:
	close(td->dirfd);
error_dirfd:
	closedir(td->dir);
error:
	return ret;
}

static
struct trace_descriptor *ctf_open_trace(const char *path, int flags,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence), FILE *metadata_fp)
{
	struct ctf_trace *td;
	int ret;

	/*
	 * If packet_seek is NULL, we provide our default version.
	 */
	if (!packet_seek)
		packet_seek = ctf_packet_seek;

	td = g_new0(struct ctf_trace, 1);

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		if (!path) {
			fprintf(stderr, "[error] Path missing for input CTF trace.\n");
			goto error;
		}
		ret = ctf_open_trace_read(td, path, flags, packet_seek, metadata_fp);
		if (ret)
			goto error;
		break;
	case O_RDWR:
		fprintf(stderr, "[error] Opening CTF traces for output is not supported yet.\n");
		goto error;
	default:
		fprintf(stderr, "[error] Incorrect open flags.\n");
		goto error;
	}

	return &td->parent;
error:
	g_free(td);
	return NULL;
}


void ctf_init_mmap_pos(struct ctf_stream_pos *pos,
		struct mmap_stream *mmap_info)
{
	pos->mmap_offset = 0;
	pos->packet_size = 0;
	pos->content_size = 0;
	pos->content_size_loc = NULL;
	pos->fd = mmap_info->fd;
	pos->base = 0;
	pos->offset = 0;
	pos->dummy = false;
	pos->cur_index = 0;
	pos->packet_index = NULL;
	pos->prot = PROT_READ;
	pos->flags = MAP_PRIVATE;
	pos->parent.rw_table = read_dispatch_table;
	pos->parent.event_cb = ctf_read_event;
}

static
int prepare_mmap_stream_definition(struct ctf_trace *td,
		struct ctf_file_stream *file_stream)
{
	struct ctf_stream_class *stream;
	uint64_t stream_id = 0;
	int ret;

	file_stream->parent.stream_id = stream_id;
	if (stream_id >= td->streams->len) {
		fprintf(stderr, "[error] Stream %" PRIu64 " is not declared "
				"in metadata.\n", stream_id);
		ret = -EINVAL;
		goto end;
	}
	stream = g_ptr_array_index(td->streams, stream_id);
	if (!stream) {
		fprintf(stderr, "[error] Stream %" PRIu64 " is not declared "
				"in metadata.\n", stream_id);
		ret = -EINVAL;
		goto end;
	}
	file_stream->parent.stream_class = stream;
	ret = create_stream_definitions(td, &file_stream->parent);
end:
	return ret;
}

static
int ctf_open_mmap_stream_read(struct ctf_trace *td,
		struct mmap_stream *mmap_info,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence))
{
	int ret;
	struct ctf_file_stream *file_stream;

	file_stream = g_new0(struct ctf_file_stream, 1);
	ctf_init_mmap_pos(&file_stream->pos, mmap_info);

	file_stream->pos.packet_seek = packet_seek;

	ret = create_trace_definitions(td, &file_stream->parent);
	if (ret) {
		goto error_def;
	}

	ret = prepare_mmap_stream_definition(td, file_stream);
	if (ret)
		goto error_index;

	/* Add stream file to stream class */
	g_ptr_array_add(file_stream->parent.stream_class->streams,
			&file_stream->parent);
	return 0;

error_index:
	if (file_stream->parent.trace_packet_header)
		definition_unref(&file_stream->parent.trace_packet_header->p);
error_def:
	g_free(file_stream);
	return ret;
}

int ctf_open_mmap_trace_read(struct ctf_trace *td,
		struct mmap_stream_list *mmap_list,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence),
		FILE *metadata_fp)
{
	int ret;
	struct mmap_stream *mmap_info;

	ret = ctf_open_trace_metadata_read(td, ctf_packet_seek, metadata_fp);
	if (ret) {
		goto error;
	}

	/*
	 * for each stream, try to open, check magic number, and get the
	 * stream ID to add to the right location in the stream array.
	 */
	bt_list_for_each_entry(mmap_info, &mmap_list->head, list) {
		ret = ctf_open_mmap_stream_read(td, mmap_info, packet_seek);
		if (ret) {
			fprintf(stderr, "[error] Open file mmap stream error.\n");
			goto error;
		}
	}

	return 0;

error:
	return ret;
}

static
struct trace_descriptor *ctf_open_mmap_trace(
		struct mmap_stream_list *mmap_list,
		void (*packet_seek)(struct stream_pos *pos, size_t index,
			int whence),
		FILE *metadata_fp)
{
	struct ctf_trace *td;
	int ret;

	if (!metadata_fp) {
		fprintf(stderr, "[error] No metadata file pointer associated, "
				"required for mmap parsing\n");
		goto error;
	}
	if (!packet_seek) {
		fprintf(stderr, "[error] packet_seek function undefined.\n");
		goto error;
	}
	td = g_new0(struct ctf_trace, 1);
	ret = ctf_open_mmap_trace_read(td, mmap_list, packet_seek, metadata_fp);
	if (ret)
		goto error_free;

	return &td->parent;

error_free:
	g_free(td);
error:
	return NULL;
}

static
void ctf_close_file_stream(struct ctf_file_stream *file_stream)
{
	ctf_fini_pos(&file_stream->pos);
	close(file_stream->pos.fd);
}

static
void ctf_close_trace(struct trace_descriptor *tdp)
{
	struct ctf_trace *td = container_of(tdp, struct ctf_trace, parent);
	int i;

	if (td->streams) {
		for (i = 0; i < td->streams->len; i++) {
			struct ctf_stream_class *stream;
			int j;

			stream = g_ptr_array_index(td->streams, i);
			if (!stream)
				continue;
			for (j = 0; j < stream->streams->len; j++) {
				struct ctf_file_stream *file_stream;
				file_stream = container_of(g_ptr_array_index(stream->streams, j), struct ctf_file_stream, parent);
				ctf_close_file_stream(file_stream);
			}

		}
		g_ptr_array_free(td->streams, TRUE);
	}
	closedir(td->dir);
	g_free(td);
}

void __attribute__((constructor)) ctf_init(void)
{
	int ret;

	ctf_format.name = g_quark_from_static_string("ctf");
	ret = bt_register_format(&ctf_format);
	assert(!ret);
}

/* TODO: finalize */
