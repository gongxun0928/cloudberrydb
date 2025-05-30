/*-------------------------------------------------------------------------
 *
 * copyfromparse.c
 *		Parse CSV/text/binary format for COPY FROM.
 *
 * This file contains routines to parse the text, CSV and binary input
 * formats.  The main entry point is NextCopyFrom(), which parses the
 * next input line and returns it as Datums.
 *
 * In text/CSV mode, the parsing happens in multiple stages:
 *
 * [data source] --> raw_buf --> input_buf --> line_buf --> attribute_buf
 *                1.          2.            3.           4.
 *
 * 1. CopyLoadRawBuf() reads raw data from the input file or client, and
 *    places it into 'raw_buf'.
 *
 * 2. CopyConvertBuf() calls the encoding conversion function to convert
 *    the data in 'raw_buf' from client to server encoding, placing the
 *    converted result in 'input_buf'.
 *
 * 3. CopyReadLine() parses the data in 'input_buf', one line at a time.
 *    It is responsible for finding the next newline marker, taking quote and
 *    escape characters into account according to the COPY options.  The line
 *    is copied into 'line_buf', with quotes and escape characters still
 *    intact.
 *
 * 4. CopyReadAttributesText/CSV() function takes the input line from
 *    'line_buf', and splits it into fields, unescaping the data as required.
 *    The fields are stored in 'attribute_buf', and 'raw_fields' array holds
 *    pointers to each field.
 *
 * If encoding conversion is not required, a shortcut is taken in step 2 to
 * avoid copying the data unnecessarily.  The 'input_buf' pointer is set to
 * point directly to 'raw_buf', so that CopyLoadRawBuf() loads the raw data
 * directly into 'input_buf'.  CopyConvertBuf() then merely validates that
 * the data is valid in the current encoding.
 *
 * In binary mode, the pipeline is much simpler.  Input is loaded into
 * into 'raw_buf', and encoding conversion is done in the datatype-specific
 * receive functions, if required.  'input_buf' and 'line_buf' are not used,
 * but 'attribute_buf' is used as a temporary buffer to hold one attribute's
 * data when it's passed the receive function.
 *
 * 'raw_buf' is always 64 kB in size (RAW_BUF_SIZE).  'input_buf' is also
 * 64 kB (INPUT_BUF_SIZE), if encoding conversion is required.  'line_buf'
 * and 'attribute_buf' are expanded on demand, to hold the longest line
 * encountered so far.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyfromparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

/*
 * These macros centralize code used to process line_buf and input_buf buffers.
 * They are macros because they often do continue/break control and to avoid
 * function call overhead in tight COPY loops.
 *
 * We must use "if (1)" because the usual "do {...} while(0)" wrapper would
 * prevent the continue/break processing from working.  We end the "if (1)"
 * with "else ((void) 0)" to ensure the "if" does not unintentionally match
 * any "else" in the calling code, and to avoid any compiler warnings about
 * empty statements.  See http://www.cit.gu.edu.au/~anthony/info/C/C.macros.
 */

/*
 * This keeps the character read at the top of the loop in the buffer
 * even if there is more than one read-ahead.
 */
#define IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(extralen) \
if (1) \
{ \
	if (input_buf_ptr + (extralen) >= copy_buf_len && !hit_eof) \
	{ \
		input_buf_ptr = prev_raw_ptr; /* undo fetch */ \
		need_data = true; \
		continue; \
	} \
} else ((void) 0)

/* This consumes the remainder of the buffer and breaks */
#define IF_NEED_REFILL_AND_EOF_BREAK(extralen) \
if (1) \
{ \
	if (input_buf_ptr + (extralen) >= copy_buf_len && hit_eof) \
	{ \
		if (extralen) \
			input_buf_ptr = copy_buf_len; /* consume the partial character */ \
		/* backslash just before EOF, treat as data char */ \
		result = true; \
		break; \
	} \
} else ((void) 0)

/*
 * Transfer any approved data to line_buf; must do this to be sure
 * there is some room in input_buf.
 */
#define REFILL_LINEBUF \
if (1) \
{ \
	if (input_buf_ptr > cstate->input_buf_index) \
	{ \
		appendBinaryStringInfo(&cstate->line_buf, \
							 cstate->input_buf + cstate->input_buf_index, \
							   input_buf_ptr - cstate->input_buf_index); \
		cstate->input_buf_index = input_buf_ptr; \
	} \
} else ((void) 0)

/* Undo any read-ahead and jump out of the block. */
#define NO_END_OF_COPY_GOTO \
if (1) \
{ \
	input_buf_ptr = prev_raw_ptr + 1; \
	goto not_end_of_copy; \
} else ((void) 0)

/* NOTE: there's a copy of this in copyto.c */
static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";

/* Low-level communications functions */
static void CopyLoadInputBuf(CopyFromState cstate);
static void RemoveInvalidDataInBuf(CopyFromState cstate);
static bool FindEolInUnverifyRawBuf(const CopyFromState cstate, int *scanidx);

void
ReceiveCopyBegin(CopyFromState cstate)
{
	StringInfoData buf;
	int			natts = list_length(cstate->attnumlist);
	int16		format = (cstate->opts.binary ? 1 : 0);
	int			i;

	pq_beginmessage(&buf, 'G');
	pq_sendbyte(&buf, format);	/* overall format */
	pq_sendint16(&buf, natts);
	for (i = 0; i < natts; i++)
		pq_sendint16(&buf, format); /* per-column formats */
	pq_endmessage(&buf);
	cstate->copy_src = COPY_FRONTEND;
	cstate->fe_msgbuf = makeStringInfo();
	/* We *must* flush here to ensure FE knows it can send. */
	pq_flush();
}

void
ReceiveCopyBinaryHeader(CopyFromState cstate)
{
	char		readSig[11];
	int32		tmp;

	/* Signature */
	if (CopyReadBinaryData(cstate, readSig, 11) != 11 ||
		memcmp(readSig, BinarySignature, 11) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("COPY file signature not recognized")));
	/* Flags field */
	if (!CopyGetInt32(cstate, &tmp))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (missing flags)")));
	if ((tmp & (1 << 16)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (WITH OIDS)")));
	tmp &= ~(1 << 16);
	if ((tmp >> 16) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unrecognized critical flags in COPY file header")));
	/* Header extension length */
	if (!CopyGetInt32(cstate, &tmp) ||
		tmp < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (missing length)")));
	/* Skip extension header, if present */
	while (tmp-- > 0)
	{
		if (CopyReadBinaryData(cstate, readSig, 1) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (wrong length)")));
	}
}

/*
 * CopyGetData reads data from the source (file or frontend)
 *
 * We attempt to read at least minread, and at most maxread, bytes from
 * the source.  The actual number of bytes read is returned; if this is
 * less than minread, EOF was detected.
 *
 * Note: when copying from the frontend, we expect a proper EOF mark per
 * protocol; if the frontend simply drops the connection, we raise error.
 * It seems unwise to allow the COPY IN to complete normally in that case.
 *
 * NB: no data conversion is applied here.
 */
int
CopyGetData(CopyFromState cstate, void *databuf, int minread, int maxread)
{
	int			bytesread = 0;

	switch (cstate->copy_src)
	{
		case COPY_FILE:
			bytesread = fread(databuf, 1, maxread, cstate->copy_file);
			if (ferror(cstate->copy_file))
			{
				if (cstate->is_program)
				{
					int olderrno = errno;
					if (cstate->copy_file)
					{
						fclose(cstate->copy_file);
						cstate->copy_file = NULL;
					}
					close_program_pipes(cstate->program_pipes, true);

					/*
					 * If close_program_pipes() didn't throw an error,
					 * the program terminated normally, but closed the
					 * pipe first. Restore errno, and throw an error.
					 */
					errno = olderrno;

					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not read from COPY program: %m")));
				}
				else
					ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read from COPY file: %m")));
			}
			if (bytesread == 0)
				cstate->raw_reached_eof = true;
			break;
		case COPY_FRONTEND:
			while (maxread > 0 && bytesread < minread && !cstate->raw_reached_eof)
			{
				int			avail;

				while (cstate->fe_msgbuf->cursor >= cstate->fe_msgbuf->len)
				{
					/* Try to receive another message */
					int			mtype;
					int			maxmsglen;

			readmessage:
					HOLD_CANCEL_INTERRUPTS();
					pq_startmsgread();
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					/* Validate message type and set packet size limit */
					switch (mtype)
					{
						case 'd':	/* CopyData */
							maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
							break;
						case 'c':	/* CopyDone */
						case 'f':	/* CopyFail */
						case 'H':	/* Flush */
						case 'S':	/* Sync */
							maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("unexpected message type 0x%02X during COPY from stdin",
											mtype)));
							maxmsglen = 0;	/* keep compiler quiet */
							break;
					}
					/* Now collect the message body */
					if (pq_getmessage(cstate->fe_msgbuf, maxmsglen))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					RESUME_CANCEL_INTERRUPTS();
					/* ... and process it */
					switch (mtype)
					{
						case 'd':	/* CopyData */
							break;
						case 'c':	/* CopyDone */
							/* COPY IN correctly terminated by frontend */
							cstate->raw_reached_eof = true;
							return bytesread;
						case 'f':	/* CopyFail */
							ereport(ERROR,
									(errcode(ERRCODE_QUERY_CANCELED),
									 errmsg("COPY from stdin failed: %s",
											pq_getmsgstring(cstate->fe_msgbuf))));
							break;
						case 'H':	/* Flush */
						case 'S':	/* Sync */

							/*
							 * Ignore Flush/Sync for the convenience of client
							 * libraries (such as libpq) that may send those
							 * without noticing that the command they just
							 * sent was COPY.
							 */
							goto readmessage;
						default:
							Assert(false);	/* NOT REACHED */
					}
				}
				avail = cstate->fe_msgbuf->len - cstate->fe_msgbuf->cursor;
				if (avail > maxread)
					avail = maxread;
				pq_copymsgbytes(cstate->fe_msgbuf, databuf, avail);
				databuf = (void *) ((char *) databuf + avail);
				maxread -= avail;
				bytesread += avail;
			}
			break;
		case COPY_CALLBACK:
			bytesread = cstate->data_source_cb(databuf, minread, maxread,
											   cstate->data_source_cb_extra);
			break;
	}

	return bytesread;
}

/*
 * Perform encoding conversion on data in 'raw_buf', writing the converted
 * data into 'input_buf'.
 *
 * On entry, there must be some data to convert in 'raw_buf'.
 */
static void
CopyConvertBuf(CopyFromState cstate)
{
	/*
	 * If the file and server encoding are the same, no encoding conversion is
	 * required.  However, we still need to verify that the input is valid for
	 * the encoding.
	 */
	if (!cstate->need_transcoding)
	{
		/*
		 * When conversion is not required, input_buf and raw_buf are the
		 * same.  raw_buf_len is the total number of bytes in the buffer, and
		 * input_buf_len tracks how many of those bytes have already been
		 * verified.
		 */
		int			preverifiedlen = cstate->input_buf_len;
		int			unverifiedlen = cstate->raw_buf_len - cstate->input_buf_len;
		int			nverified;

		if (unverifiedlen == 0)
		{
			/*
			 * If no more raw data is coming, report the EOF to the caller.
			 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
		 * Verify the new data, including any residual unverified bytes from
		 * previous round.
		 */
		nverified = pg_encoding_verifymbstr(cstate->file_encoding,
											cstate->raw_buf + preverifiedlen,
											unverifiedlen);
		if (nverified == 0)
		{
			/*
			 * Could not verify anything.
			 *
			 * If there is no more raw input data coming, it means that there
			 * was an incomplete multi-byte sequence at the end.  Also, if
			 * there's "enough" input left, we should be able to verify at
			 * least one character, and a failure to do so means that we've
			 * hit an invalid byte sequence.
			 */
			if (cstate->raw_reached_eof || unverifiedlen >= pg_encoding_max_length(cstate->file_encoding))
				cstate->input_reached_error = true;
			return;
		}
		cstate->input_buf_len += nverified;
	}
	else
	{
		/*
		 * Encoding conversion is needed.
		 */
		int			nbytes;
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;
		int			convertedlen;

		if (RAW_BUF_BYTES(cstate) == 0)
		{
			/*
			 * If no more raw data is coming, report the EOF to the caller.
			 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
		 * First, copy down any unprocessed data.
		 */
		nbytes = INPUT_BUF_BYTES(cstate);
		if (nbytes > 0 && cstate->input_buf_index > 0)
			memmove(cstate->input_buf, cstate->input_buf + cstate->input_buf_index,
					nbytes);
		cstate->input_buf_index = 0;
		cstate->input_buf_len = nbytes;
		cstate->input_buf[nbytes] = '\0';

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		/*
		 * Do the conversion.  This might stop short, if there is an invalid
		 * byte sequence in the input.  We'll convert as much as we can in
		 * that case.
		 *
		 * Note: Even if we hit an invalid byte sequence, we don't report the
		 * error until all the valid bytes have been consumed.  The input
		 * might contain an end-of-input marker (\.), and we don't want to
		 * report an error if the invalid byte sequence is after the
		 * end-of-input marker.  We might unnecessarily convert some data
		 * after the end-of-input marker as long as it's valid for the
		 * encoding, but that's harmless.
		 */
		convertedlen = pg_do_encoding_conversion_buf(cstate->conversion_proc,
													 cstate->file_encoding,
													 GetDatabaseEncoding(),
													 src, srclen,
													 dst, dstlen,
													 true);
		if (convertedlen == 0)
		{
			/*
			 * Could not convert anything.  If there is no more raw input data
			 * coming, it means that there was an incomplete multi-byte
			 * sequence at the end.  Also, if there is plenty of input left,
			 * we should be able to convert at least one character, so a
			 * failure to do so must mean that we've hit a byte sequence
			 * that's invalid.
			 */
			if (cstate->raw_reached_eof || srclen >= MAX_CONVERSION_INPUT_LENGTH)
				cstate->input_reached_error = true;
			return;
		}
		cstate->raw_buf_index += convertedlen;
		cstate->input_buf_len += strlen((char *) dst);
	}
}

/*
 * Report an encoding or conversion error.
 */
static void
CopyConversionError(CopyFromState cstate)
{
	Assert(cstate->raw_buf_len > 0);
	Assert(cstate->input_reached_error);

	if (!cstate->need_transcoding)
	{
		/*
		 * Everything up to input_buf_len was successfully verified, and
		 * input_buf_len points to the invalid or incomplete character.
		 */
		report_invalid_encoding(cstate->file_encoding,
								cstate->raw_buf + cstate->input_buf_len,
								cstate->raw_buf_len - cstate->input_buf_len);
	}
	else
	{
		/*
		 * raw_buf_index points to the invalid or untranslatable character. We
		 * let the conversion routine report the error, because it can provide
		 * a more specific error message than we could here.  An earlier call
		 * to the conversion routine in CopyConvertBuf() detected that there
		 * is an error, now we call the conversion routine again with
		 * noError=false, to have it throw the error.
		 */
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		(void) pg_do_encoding_conversion_buf(cstate->conversion_proc,
											 cstate->file_encoding,
											 GetDatabaseEncoding(),
											 src, srclen,
											 dst, dstlen,
											 false);

		/*
		 * The conversion routine should have reported an error, so this
		 * should not be reached.
		 */
		elog(ERROR, "encoding conversion failed without error");
	}
}

/*
 * Load more data from data source to raw_buf.
 *
 * If RAW_BUF_BYTES(cstate) > 0, the unprocessed bytes are moved to the
 * beginning of the buffer, and we load new data after that.
 */
static void
CopyLoadRawBuf(CopyFromState cstate)
{
	int			nbytes;
	int			inbytes;

	/*
	 * In text mode, if encoding conversion is not required, raw_buf and
	 * input_buf point to the same buffer.  Their len/index better agree, too.
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->raw_buf_index == cstate->input_buf_index);
		Assert(cstate->input_buf_len <= cstate->raw_buf_len);
	}

	/*
	 * Copy down the unprocessed data if any.
	 */
	nbytes = RAW_BUF_BYTES(cstate);
	if (nbytes > 0 && cstate->raw_buf_index > 0)
		memmove(cstate->raw_buf, cstate->raw_buf + cstate->raw_buf_index,
				nbytes);
	cstate->raw_buf_len -= cstate->raw_buf_index;
	cstate->raw_buf_index = 0;

	/*
	 * If raw_buf and input_buf are in fact the same buffer, adjust the
	 * input_buf variables, too.
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		cstate->input_buf_len -= cstate->input_buf_index;
		cstate->input_buf_index = 0;
	}

	/* Load more data */
	inbytes = CopyGetData(cstate, cstate->raw_buf + cstate->raw_buf_len,
						  1, RAW_BUF_SIZE - cstate->raw_buf_len);
	nbytes += inbytes;
	cstate->raw_buf[nbytes] = '\0';
	cstate->raw_buf_len = nbytes;

	cstate->bytes_processed += inbytes;
	pgstat_progress_update_param(PROGRESS_COPY_BYTES_PROCESSED, cstate->bytes_processed);

	if (inbytes == 0)
		cstate->raw_reached_eof = true;
}

/*
 * CopyLoadInputBuf loads some more data into input_buf
 *
 * On return, at least one more input character is loaded into
 * input_buf, or input_reached_eof is set.
 *
 * If INPUT_BUF_BYTES(cstate) > 0, the unprocessed bytes are moved to the start
 * of the buffer and then we load more data after that.
 */
static void
CopyLoadInputBuf(CopyFromState cstate)
{
	int			nbytes = INPUT_BUF_BYTES(cstate);

	/*
	 * The caller has updated input_buf_index to indicate how much of the
	 * input has been consumed and isn't needed anymore.  If input_buf is the
	 * same physical area as raw_buf, update raw_buf_index accordingly.
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->input_buf_index >= cstate->raw_buf_index);
		cstate->raw_buf_index = cstate->input_buf_index;
	}

	for (;;)
	{
		/* If we now have some unconverted data, try to convert it */
sreh_conversion_error:
		CopyConvertBuf(cstate);

		/* If we now have some more input bytes ready, return them */
		if (INPUT_BUF_BYTES(cstate) > nbytes)
			return;

		/*
		 * If we reached an invalid byte sequence, or we're at an incomplete
		 * multi-byte character but there is no more raw input data, report
		 * conversion error.
		 */
		if (cstate->input_reached_error)
		{
			/* so far, we only support no transcoding conversion error handling */
			if (cstate->cdbsreh && !cstate->need_transcoding)
			{
				MemoryContext oldcontext = CurrentMemoryContext;
				PG_TRY();
				{
					CopyConversionError(cstate);
				}
				PG_CATCH();
				{
					HandleCopyError(cstate);
					RemoveInvalidDataInBuf(cstate);
					MemoryContextSwitchTo(oldcontext);
				}
				PG_END_TRY();
				/* clean illegal character do conversion again */
				goto sreh_conversion_error;
			}
			else
				CopyConversionError(cstate);
		}

		/* no more input, and everything has been converted */
		if (cstate->input_reached_eof)
			break;

		/* Try to load more raw data */
		Assert(!cstate->raw_reached_eof);
		CopyLoadRawBuf(cstate);

		while (cstate->find_eol_with_rawreading && !cstate->raw_reached_eof)
		{
			int seekid;
			if (FindEolInUnverifyRawBuf(cstate, &seekid))
			{
				cstate->raw_buf_index += seekid;
				nbytes = RAW_BUF_BYTES(cstate);
				if (nbytes > 0 && cstate->raw_buf_index > 0)
					memmove(cstate->raw_buf,
							cstate->raw_buf + cstate->raw_buf_index, nbytes);

				cstate->raw_buf_len -= cstate->raw_buf_index;
				cstate->raw_buf_index = 0;
				cstate->find_eol_with_rawreading = false;
			}
			else
			{
				/*
				 * if EOL can not find, continuous read raw page until eof or
				 * find out.
				 */
				CopyLoadRawBuf(cstate);
			}
		}
	}
}

/*
 * CopyReadBinaryData
 *
 * Reads up to 'nbytes' bytes from cstate->copy_file via cstate->raw_buf
 * and writes them to 'dest'.  Returns the number of bytes read (which
 * would be less than 'nbytes' only if we reach EOF).
 */
int
CopyReadBinaryData(CopyFromState cstate, char *dest, int nbytes)
{
	int			copied_bytes = 0;

	if (RAW_BUF_BYTES(cstate) >= nbytes)
	{
		/* Enough bytes are present in the buffer. */
		memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, nbytes);
		cstate->raw_buf_index += nbytes;
		copied_bytes = nbytes;
	}
	else
	{
		/*
		 * Not enough bytes in the buffer, so must read from the file.  Need
		 * to loop since 'nbytes' could be larger than the buffer size.
		 */
		do
		{
			int			copy_bytes;

			/* Load more data if buffer is empty. */
			if (RAW_BUF_BYTES(cstate) == 0)
			{
				CopyLoadRawBuf(cstate);
				if (cstate->raw_reached_eof)
					break;		/* EOF */
			}

			/* Transfer some bytes. */
			copy_bytes = Min(nbytes - copied_bytes, RAW_BUF_BYTES(cstate));
			memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, copy_bytes);
			cstate->raw_buf_index += copy_bytes;
			dest += copy_bytes;
			copied_bytes += copy_bytes;
		} while (copied_bytes < nbytes);
	}

	return copied_bytes;
}

/*
 * Read the next input line and stash it in line_buf.
 *
 * Result is true if read was terminated by EOF, false if terminated
 * by newline.  The terminating newline or EOF marker is not included
 * in the final value of line_buf.
 */
bool
CopyReadLine(CopyFromState cstate)
{
	bool		result;

	resetStringInfo(&cstate->line_buf);
	cstate->line_buf_valid = false;

	/* Parse data and transfer into line_buf */
	result = CopyReadLineText(cstate);

	if (result)
	{
		/*
		 * Reached EOF.  In protocol version 3, we should ignore anything
		 * after \. up to the protocol end of copy data.  (XXX maybe better
		 * not to treat \. as special?)
		 */
		if (cstate->copy_src == COPY_FRONTEND)
		{
			int			inbytes;

			do
			{
				inbytes = CopyGetData(cstate, cstate->input_buf,
									  1, INPUT_BUF_SIZE);
			} while (inbytes > 0);
			cstate->input_buf_index = 0;
			cstate->input_buf_len = 0;
			cstate->raw_buf_index = 0;
			cstate->raw_buf_len = 0;
		}
	}
	else
	{
		/*
		 * If we didn't hit EOF, then we must have transferred the EOL marker
		 * to line_buf along with the data.  Get rid of it.
		 */
		switch (cstate->eol_type)
		{
			case EOL_NL:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CR:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\r');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CRNL:
				Assert(cstate->line_buf.len >= 2);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 2] == '\r');
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len -= 2;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_UNKNOWN:
				/* shouldn't get here */
				Assert(false);
				break;
		}
	}

	/* Now it's safe to use the buffer in error messages */
	cstate->line_buf_valid = true;

	return result;
}

/*
 * CopyReadLineText - inner loop of CopyReadLine for text mode
 */
bool
CopyReadLineText(CopyFromState cstate)
{
	char	   *copy_input_buf;
	int			input_buf_ptr;
	int			copy_buf_len;
	bool		need_data = false;
	bool		hit_eof = false;
	bool		result = false;

	/* CSV variables */
	bool		first_char_in_line = true;
	bool		in_quote = false,
				last_was_esc = false;
	char		quotec = '\0';
	char		escapec = '\0';

	if (cstate->opts.csv_mode)
	{
		quotec = cstate->opts.quote[0];
		escapec = cstate->opts.escape[0];
		/* ignore special escape processing if it's the same as quotec */
		if (quotec == escapec)
			escapec = '\0';
	}

	/*
	 * The objective of this loop is to transfer the entire next input line
	 * into line_buf.  Hence, we only care for detecting newlines (\r and/or
	 * \n) and the end-of-copy marker (\.).
	 *
	 * In CSV mode, \r and \n inside a quoted field are just part of the data
	 * value and are put in line_buf.  We keep just enough state to know if we
	 * are currently in a quoted field or not.
	 *
	 * These four characters, and the CSV escape and quote characters, are
	 * assumed the same in frontend and backend encodings.
	 *
	 * For speed, we try to move data from raw_buf to line_buf in chunks
	 * rather than one character at a time.  input_buf_ptr points to the next
	 * character to examine; any characters from raw_buf_index to input_buf_ptr
	 * have been determined to be part of the line, but not yet transferred to
	 * line_buf.
	 *
	 * For a little extra speed within the loop, we copy raw_buf and
	 * raw_buf_len into local variables.
	 */
	copy_input_buf = cstate->input_buf;
	input_buf_ptr = cstate->input_buf_index;
	copy_buf_len = cstate->input_buf_len;

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/*
		 * Load more data if needed.  Ideally we would just force four bytes
		 * of read-ahead and avoid the many calls to
		 * IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(), but the COPY_OLD_FE protocol
		 * does not allow us to read too far ahead or we might read into the
		 * next data, so we read-ahead only as far we know we can.  One
		 * optimization would be to read-ahead four byte here if
		 * cstate->copy_src != COPY_FRONTEND, but it hardly seems worth it,
		 * considering the size of the buffer.
		 */
		if (input_buf_ptr >= copy_buf_len || need_data)
		{
			REFILL_LINEBUF;

			CopyLoadInputBuf(cstate);

			/* update our local variables */
			hit_eof = cstate->input_reached_eof;
			input_buf_ptr = cstate->input_buf_index;
			copy_buf_len = cstate->input_buf_len;

			/*
			 * If we are completely out of data, break out of the loop,
			 * reporting EOF.
			 */
			if (INPUT_BUF_BYTES(cstate) <= 0)
			{
				result = true;
				break;
			}
			need_data = false;
		}

		/* OK to fetch a character */
		prev_raw_ptr = input_buf_ptr;
		c = copy_input_buf[input_buf_ptr++];

		if (cstate->opts.csv_mode)
		{
			/*
			 * If character is '\\' or '\r', we may need to look ahead below.
			 * Force fetch of the next character if we don't already have it.
			 * We need to do this before changing CSV state, in case one of
			 * these characters is also the quote or escape character.
			 *
			 * Note: old-protocol does not like forced prefetch, but it's OK
			 * here since we cannot validly be at EOF.
			 */
			if (c == '\\' || c == '\r')
			{
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			}

			/*
			 * Dealing with quotes and escapes here is mildly tricky. If the
			 * quote char is also the escape char, there's no problem - we
			 * just use the char as a toggle. If they are different, we need
			 * to ensure that we only take account of an escape inside a
			 * quoted field and immediately preceding a quote char, and not
			 * the second in an escape-escape sequence.
			 */
			if (in_quote && c == escapec)
				last_was_esc = !last_was_esc;
			if (c == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (c != escapec)
				last_was_esc = false;

			/*
			 * Updating the line count for embedded CR and/or LF chars is
			 * necessarily a little fragile - this test is probably about the
			 * best we can do.  (XXX it's arguable whether we should do this
			 * at all --- is cur_lineno a physical or logical count?)
			 */
			if (in_quote && c == (cstate->eol_type == EOL_NL ? '\n' : '\r'))
				cstate->cur_lineno++;
		}

		/* Process \r */
		if (c == '\r' && (!cstate->opts.csv_mode || !in_quote))
		{
			/* Check for \r\n on first line, _and_ handle \r\n. */
			if (cstate->eol_type == EOL_UNKNOWN ||
				cstate->eol_type == EOL_CRNL)
			{
				/*
				 * If need more data, go back to loop top to load it.
				 *
				 * Note that if we are at EOF, c will wind up as '\0' because
				 * of the guaranteed pad of raw_buf.
				 */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);

				/* get next char */
				c = copy_input_buf[input_buf_ptr];

				if (c == '\n')
				{
					input_buf_ptr++;	/* eat newline */
					cstate->eol_type = EOL_CRNL;	/* in case not set yet */

					/*
					 * GPDB: end of line. Since we don't error out if we find a
					 * bare CR or LF in CRLF mode, break here instead.
					 */
					break;
				}
				else
				{
					/*
					 * GPDB_91_MERGE_FIXME: these commented-out blocks (as well
					 * as the restructured newline checks) are here because we
					 * allow the user to manually set the newline mode, and
					 * therefore don't error out on bare CR/LF in the middle of
					 * a column. Instead, they will be included verbatim.
					 *
					 * This probably has other fallout -- but so does changing
					 * the behavior. Discuss.
					 */
#if 0
					/* found \r, but no \n */
					if (cstate->eol_type == EOL_CRNL)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 !cstate->opts.csv_mode ?
								 errmsg("literal carriage return found in data") :
								 errmsg("unquoted carriage return found in data"),
								 !cstate->opts.csv_mode ?
								 errhint("Use \"\\r\" to represent carriage return.") :
								 errhint("Use quoted CSV field to represent carriage return.")));
#endif

					/* GPDB: only reset eol_type if it's currently unknown. */
					if (cstate->eol_type == EOL_UNKNOWN  && (cstate->opts.eol_type == EOL_UNKNOWN || cstate->opts.eol_type == EOL_CR))
					{
						/*
						 * if we got here, it is the first line and we didn't find
						 * \n, so don't consume the peeked character
						 */
						cstate->eol_type = EOL_CR;
					}
				}
			}
#if 0 /* GPDB_91_MERGE_FIXME: see above. */
			else if (cstate->eol_type == EOL_NL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !cstate->opts.csv_mode ?
						 errmsg("literal carriage return found in data") :
						 errmsg("unquoted carriage return found in data"),
						 !cstate->opts.csv_mode ?
						 errhint("Use \"\\r\" to represent carriage return.") :
						 errhint("Use quoted CSV field to represent carriage return.")));
#endif
			/* GPDB: a CR only ends the line in CR mode. */
			if (cstate->eol_type == EOL_CR)
			{
				/* If reach here, we have found the line terminator */
				break;
			}
		}

		/* Process \n */
		if (c == '\n' && (!cstate->opts.csv_mode || !in_quote))
		{
#if 0 /* GPDB_91_MERGE_FIXME: see above. */
			if (cstate->eol_type == EOL_CR || cstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !cstate->opts.csv_mode ?
						 errmsg("literal newline found in data") :
						 errmsg("unquoted newline found in data"),
						 !cstate->opts.csv_mode ?
						 errhint("Use \"\\n\" to represent newline.") :
						 errhint("Use quoted CSV field to represent newline.")));
#endif
			/* GPDB: only reset eol_type if it's currently unknown. */
			if (cstate->eol_type == EOL_UNKNOWN && (cstate->opts.eol_type == EOL_UNKNOWN || cstate->opts.eol_type == EOL_NL))
				cstate->eol_type = EOL_NL;	/* in case not set yet */

			/* GPDB: a LF only ends the line in LF mode. */
			if (cstate->eol_type == EOL_NL)
			{
				/* If reach here, we have found the line terminator */
				break;
			}
		}

		/*
		 * In CSV mode, we only recognize \. alone on a line.  This is because
		 * \. is a valid CSV data value.
		 */
		if (c == '\\' && (!cstate->opts.csv_mode || first_char_in_line))
		{
			char		c2;

			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			IF_NEED_REFILL_AND_EOF_BREAK(0);

			/* -----
			 * get next character
			 * Note: we do not change c so if it isn't \., we can fall
			 * through and continue processing for file encoding.
			 * -----
			 */
			c2 = copy_input_buf[input_buf_ptr];

			/*
			* We need to recognize the EOL.
			* Github issue: https://github.com/greenplum-db/gpdb/issues/12454
			*/
			if(c2 == '\n')
			{
				if(cstate->eol_type == EOL_UNKNOWN)
				{
				/* We still do not found the first EOL.
				* The current '\n' will be recongnized as EOL
				* in next loop of c1.
				*/
					goto not_end_of_copy;
				}
				else if(cstate->eol_type == EOL_NL)
				{
					// found a new line with '\n'
					input_buf_ptr++;
					break;
				}
			}
			if (c2 == '\r')
			{
				if(cstate->eol_type == EOL_UNKNOWN)
				{
					goto not_end_of_copy;
				}
				else if(cstate->eol_type == EOL_CR)
				{
					// found a new line wirh '\r'
					input_buf_ptr++;
					break;
				}
				else if(cstate->eol_type == EOL_CRNL)
				{
					/*
					* Because the eol is '\r\n', we need another character c3
					* which comes after c2 if exists.
					*/
					char c3;
					input_buf_ptr++;
					IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
					IF_NEED_REFILL_AND_EOF_BREAK(0);
					c3 = copy_input_buf[input_buf_ptr];
					if(c3 == '\n')
					{
						// found a new line with '\r\n'
						input_buf_ptr++;
						break;
					} else {
						NO_END_OF_COPY_GOTO;
					}
				}
			}
			if (c2 == '.')
			{
				input_buf_ptr++;	/* consume the '.' */

				/*
				 * Note: if we loop back for more data here, it does not
				 * matter that the CSV state change checks are re-executed; we
				 * will come back here with no important state changed.
				 */
				if (cstate->eol_type == EOL_CRNL)
				{
					/* Get the next character */
					IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
					/* if hit_eof, c2 will become '\0' */
					c2 = copy_input_buf[input_buf_ptr++];

					if (c2 == '\n')
					{
						if (!cstate->opts.csv_mode)
						{
							cstate->input_buf_index = input_buf_ptr;
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker does not match previous newline style")));
						}
						else
							NO_END_OF_COPY_GOTO;
					}
					else if (c2 != '\r')
					{
						if (!cstate->opts.csv_mode)
						{
							cstate->input_buf_index = input_buf_ptr;
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker corrupt")));
						}
						else
							NO_END_OF_COPY_GOTO;
					}
				}

				/* Get the next character */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
				/* if hit_eof, c2 will become '\0' */
				c2 = copy_input_buf[input_buf_ptr++];

				if (c2 != '\r' && c2 != '\n')
				{
					if (!cstate->opts.csv_mode)
					{
						cstate->input_buf_index = input_buf_ptr;
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker corrupt")));
					}
					else
						NO_END_OF_COPY_GOTO;
				}

				if ((cstate->eol_type == EOL_NL && c2 != '\n') ||
					(cstate->eol_type == EOL_CRNL && c2 != '\n') ||
					(cstate->eol_type == EOL_CR && c2 != '\r'))
				{
					cstate->input_buf_index = input_buf_ptr;
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));
				}

				/*
				 * Transfer only the data before the \. into line_buf, then
				 * discard the data and the \. sequence.
				 */
				if (prev_raw_ptr > cstate->input_buf_index)
					appendBinaryStringInfo(&cstate->line_buf,
										   cstate->raw_buf + cstate->input_buf_index,
										   prev_raw_ptr - cstate->input_buf_index);
				cstate->input_buf_index = input_buf_ptr;
				result = true;	/* report EOF */
				break;
			}
			else if (!cstate->opts.csv_mode)
			{
				/*
				 * If we are here, it means we found a backslash followed by
				 * something other than a period.  In non-CSV mode, anything
				 * after a backslash is special, so we skip over that second
				 * character too.  If we didn't do that \\. would be
				 * considered an eof-of copy, while in non-CSV mode it is a
				 * literal backslash followed by a period.  In CSV mode,
				 * backslashes are not special, so we want to process the
				 * character after the backslash just like a normal character,
				 * so we don't increment in those cases.
				 */
				input_buf_ptr++;
			}
		}
		/*
		 * This label is for CSV cases where \. appears at the start of a
		 * line, but there is more text after it, meaning it was a data value.
		 * We are more strict for \. in CSV mode because \. could be a data
		 * value, while in non-CSV mode, \. cannot be a data value.
		 */
not_end_of_copy:
		first_char_in_line = false;
	}							/* end of outer loop */

	/*
	 * Transfer any still-uncopied data to line_buf.
	 */
	REFILL_LINEBUF;

	return result;
}

/*
 *	Return decimal value for a hexadecimal digit
 */
static int
GetDecimalFromHex(char hex)
{
	if (isdigit((unsigned char) hex))
		return hex - '0';
	else
		return tolower((unsigned char) hex) - 'a' + 10;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.
 *
 * The input is in line_buf.  We use attribute_buf to hold the result
 * strings.  cstate->raw_fields[k] is set to point to the k'th attribute
 * string, or NULL when the input matches the null marker string.
 * This array is expanded as necessary.
 *
 * (Note that the caller cannot check for nulls since the returned
 * string would be the post-de-escaping equivalent, which may look
 * the same as some valid data string.)
 *
 * delim is the column delimiter string (must be just one byte for now).
 * null_print is the null marker string.  Note that this is compared to
 * the pre-de-escaped input string.
 *
 * The return value is the number of fields actually read.
 */
int
CopyReadAttributesText(CopyFromState cstate, int stop_processing_at_field)
{
	char		delimc = cstate->opts.delim[0];
	char		escapec = cstate->opts.escape[0];
	bool		delim_off = cstate->opts.delim_off;
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (cstate->max_fields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.  We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into cstate->raw_fields[].
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* set pointer variables for loop */
	cur_ptr = cstate->line_buf.data + cstate->line_buf.cursor;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* Outer loop iterates over fields */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;
		bool		saw_non_ascii = false;

		/*
		 * In QD, stop once we have processed the last field we need in the QD.
		 */
		if (fieldno == stop_processing_at_field)
		{
			cstate->stopped_processing_at_delim = true;
			break;
		}

		/* Make sure there is enough space for the next value */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

		/*
		 * Scan data for field.
		 *
		 * Note that in this loop, we are scanning to locate the end of field
		 * and also speculatively performing de-escaping.  Once we find the
		 * end-of-field, we can match the raw field contents against the null
		 * marker string.  Only after that comparison fails do we know that
		 * de-escaping is actually the right thing to do; therefore we *must
		 * not* throw any syntax errors before we've done the null-marker
		 * check.
		 */
		for (;;)
		{
			char		c;

			end_ptr = cur_ptr;
			if (cur_ptr >= line_end_ptr)
				break;
			c = *cur_ptr++;
			if (c == delimc && !delim_off)
			{
				found_delim = true;
				break;
			}
			if (c == escapec && !cstate->escape_off)
			{
				if (cur_ptr >= line_end_ptr)
					break;
				c = *cur_ptr++;
				switch (c)
				{
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
						{
							/* handle \013 */
							int			val;

							val = OCTVALUE(c);
							if (cur_ptr < line_end_ptr)
							{
								c = *cur_ptr;
								if (ISOCTAL(c))
								{
									cur_ptr++;
									val = (val << 3) + OCTVALUE(c);
									if (cur_ptr < line_end_ptr)
									{
										c = *cur_ptr;
										if (ISOCTAL(c))
										{
											cur_ptr++;
											val = (val << 3) + OCTVALUE(c);
										}
									}
								}
							}
							c = val & 0377;
							if (c == '\0' || IS_HIGHBIT_SET(c))
								saw_non_ascii = true;
						}
						break;
					case 'x':
						/* Handle \x3F */
						if (cur_ptr < line_end_ptr)
						{
							char		hexchar = *cur_ptr;

							if (isxdigit((unsigned char) hexchar))
							{
								int			val = GetDecimalFromHex(hexchar);

								cur_ptr++;
								if (cur_ptr < line_end_ptr)
								{
									hexchar = *cur_ptr;
									if (isxdigit((unsigned char) hexchar))
									{
										cur_ptr++;
										val = (val << 4) + GetDecimalFromHex(hexchar);
									}
								}
								c = val & 0xff;
								if (c == '\0' || IS_HIGHBIT_SET(c))
									saw_non_ascii = true;
							}
						}
						break;
					case 'b':
						c = '\b';
						break;
					case 'f':
						c = '\f';
						break;
					case 'n':
						c = '\n';
						break;
					case 'r':
						c = '\r';
						break;
					case 't':
						c = '\t';
						break;
					case 'v':
						c = '\v';
						break;

						/*
						 * in all other cases, take the char after '\'
						 * literally
						 */
				}
			}

			/* Add c to output string */
			*output_ptr++ = c;
		}

		/* Check whether raw input matched null marker */
		input_len = end_ptr - start_ptr;
		if (input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;
		else
		{
			/*
			 * At this point we know the field is supposed to contain data.
			 *
			 * If we de-escaped any non-7-bit-ASCII chars, make sure the
			 * resulting string is valid data for the db encoding.
			 */
			if (saw_non_ascii)
			{
				char	   *fld = cstate->raw_fields[fieldno];

				pg_verifymbstr(fld, output_ptr - fld, false);
			}
		}

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		if (!found_delim)
		{
			cstate->stopped_processing_at_delim = false;
			break;
		}
	}

	/*
	 * Make note of the stopping point in 'line_buf.cursor', so that we
	 * can send the rest to the QE later.
	 */
	cstate->line_buf.cursor = cur_ptr - cstate->line_buf.data;

	/* Clean up state of attribute_buf */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.  This has exactly the same API as
 * CopyReadAttributesText, except we parse the fields according to
 * "standard" (i.e. common) CSV usage.
 */
int
CopyReadAttributesCSV(CopyFromState cstate, int stop_processing_at_field)
{
	char		delimc = cstate->opts.delim[0];
	bool		delim_off = cstate->opts.delim_off;
	char		quotec = cstate->opts.quote[0];
	char		escapec = cstate->opts.escape[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (cstate->max_fields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.  We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into cstate->raw_fields[].
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* set pointer variables for loop */
	cur_ptr = cstate->line_buf.data + cstate->line_buf.cursor;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* Outer loop iterates over fields */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		bool		saw_quote = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;

		/*
		 * In QD, stop once we have processed the last field we need in the QD.
		 */
		if (fieldno == stop_processing_at_field)
		{
			cstate->stopped_processing_at_delim = true;
			break;
		}

		/* Make sure there is enough space for the next value */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

		/*
		 * Scan data for field,
		 *
		 * The loop starts in "not quote" mode and then toggles between that
		 * and "in quote" mode. The loop exits normally if it is in "not
		 * quote" mode and a delimiter or line end is seen.
		 */
		for (;;)
		{
			char		c;

			/* Not in quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					goto endfield;
				c = *cur_ptr++;
				/* unquoted field delimiter */
				if (c == delimc && !delim_off)
				{
					found_delim = true;
					goto endfield;
				}
				/* start of quoted field (or part of field) */
				if (c == quotec)
				{
					saw_quote = true;
					break;
				}
				/* Add c to output string */
				*output_ptr++ = c;
			}

			/* In quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("unterminated CSV quoted field")));

				c = *cur_ptr++;

				/* escape within a quoted field */
				if (c == escapec)
				{
					/*
					 * peek at the next char if available, and escape it if it
					 * is an escape char or a quote char
					 */
					if (cur_ptr < line_end_ptr)
					{
						char		nextc = *cur_ptr;

						if (nextc == escapec || nextc == quotec)
						{
							*output_ptr++ = nextc;
							cur_ptr++;
							continue;
						}
					}
				}

				/*
				 * end of quoted field. Must do this test after testing for
				 * escape in case quote char and escape char are the same
				 * (which is the common case).
				 */
				if (c == quotec)
					break;

				/* Add c to output string */
				*output_ptr++ = c;
			}
		}
endfield:

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		/* Check whether raw input matched null marker */
		input_len = end_ptr - start_ptr;
		if (!saw_quote && input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		if (!found_delim)
		{
			cstate->stopped_processing_at_delim = false;
			break;
		}
	}

	/*
	 * Make note of the stopping point in 'line_buf.cursor', so that we
	 * can send the rest to the QE later.
	 */
	cstate->line_buf.cursor = cur_ptr - cstate->line_buf.data;

	/* Clean up state of attribute_buf */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}

/*
 * Read a binary attribute
 */
Datum
CopyReadBinaryAttribute(CopyFromState cstate, FmgrInfo *flinfo,
						Oid typioparam, int32 typmod,
						bool *isnull)
{
	int32		fld_size;
	Datum		result;

	if (!CopyGetInt32(cstate, &fld_size))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	if (fld_size == -1)
	{
		*isnull = true;
		return ReceiveFunctionCall(flinfo, NULL, typioparam, typmod);
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));

	/* reset attribute_buf to empty, and load raw data in it */
	resetStringInfo(&cstate->attribute_buf);

	enlargeStringInfo(&cstate->attribute_buf, fld_size);
	if (CopyReadBinaryData(cstate, cstate->attribute_buf.data, fld_size) != fld_size)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));

	cstate->attribute_buf.len = fld_size;
	cstate->attribute_buf.data[fld_size] = '\0';

	/* Call the column type's binary input converter */
	result = ReceiveFunctionCall(flinfo, &cstate->attribute_buf,
								 typioparam, typmod);

	/* Trouble if it didn't eat the whole buffer */
	if (cstate->attribute_buf.cursor != cstate->attribute_buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("incorrect binary data format")));

	*isnull = false;
	return result;
}

void
RemoveInvalidDataInBuf(CopyFromState cstate)
{
	int nbytes;
	int scanidx;

	if (cstate->errMode == ALL_OR_NOTHING)
		CopyConversionError(cstate);

	/* reach EOF, do not need clean */
	if (cstate->input_reached_eof)
		return;

	if (!cstate->need_transcoding)
	{
		/*
		 * According to `BeginCopyFrom`, if not need_transcoding these two
		 * pointer share one memory space.
		 */
		Assert(cstate->raw_buf == cstate->input_buf);

		if (FindEolInUnverifyRawBuf(cstate, &scanidx))
		{
			cstate->raw_buf_index += scanidx;
			nbytes = RAW_BUF_BYTES(cstate);
			if (nbytes > 0 && cstate->raw_buf_index > 0)
				memmove(cstate->raw_buf,
						cstate->raw_buf + cstate->raw_buf_index, nbytes);

			cstate->raw_buf_len -= cstate->raw_buf_index;
			cstate->raw_buf_index = 0;
		}
		else
		{
			/* Current page can not find eol, to skip current raw buffer */
			cstate->raw_buf_len = 0;
			cstate->raw_buf_index = 0;

			/* leave a hint to identify find eol after next raw page read */
			cstate->find_eol_with_rawreading = true;
		}

		/* reset input buf, so we can redo conversion/verification */
		cstate->input_reached_error = false;
		cstate->input_buf_index = 0;
		cstate->input_buf_len = 0;

		/* reset line_buf */
		resetStringInfo(&cstate->line_buf);
		cstate->line_buf_valid = false;
		cstate->cdbsreh->rejectcount++;
	}
	else
	{
		ereport(ERROR, (errmsg("Data validation error: since the source data "
							   "need transcoding sreh can not handle yet.")));
	}
}

static bool
FindEolInUnverifyRawBuf(const CopyFromState cstate, int *scanidx)
{
	bool found = false;
	EolType eol_type = cstate->opts.eol_type;

	for (*scanidx = 0; *scanidx < RAW_BUF_BYTES(cstate); (*scanidx)++)
	{
		if ('\r' == cstate->raw_buf[cstate->raw_buf_index + *scanidx])
		{
			if (eol_type == EOL_CR || eol_type == EOL_UNKNOWN)
			{
				*scanidx += 1;
				found = true;
				break;
			}

			if (eol_type == EOL_CRNL || eol_type == EOL_UNKNOWN)
			{
				if (*scanidx + 1 < RAW_BUF_BYTES(cstate) &&
					'\n' == cstate->raw_buf[cstate->raw_buf_index + *scanidx + 1])
				{
					*scanidx += 2;
					found = true;
					break;
				}
			}
		}
		else if ('\n' == cstate->raw_buf[cstate->raw_buf_index + *scanidx])
		{
			if (eol_type == EOL_NL || eol_type == EOL_UNKNOWN)
			{
				*scanidx += 1;
				found = true;
				break;
			}
		}
	}

	return found;
}
