/*-------------------------------------------------------------------------
 *
 * waldatacomm.h
 *
 * Shared data structures and functions for WAL data communication between
 * walsender and walreceiver processes. This file contains definitions that
 * are shared between replication components but should not be included in
 * the main PAX extension headers.
 *
 * Portions Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/replication/waldatacomm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALDATACOMM_H
#define WALDATACOMM_H

#include "access/xlogreader.h"
#include "storage/relfilenode.h"
#include "lib/stringinfo.h"

/* PAX WAL record manager ID - should be different from other storage engines */
#define PAX_RMGR_ID 199

/* PAX WAL record types */
#define XLOG_PAX_INSERT 0x00
#define XLOG_PAX_CREATE_DIRECTORY 0x10
#define XLOG_PAX_TRUNCATE 0x20
#define XLOG_PAX_INSERT_REFERENCE_DATA 0x30

/* Maximum filename length for PAX files */
#define MAX_PATH_FILE_NAME_LEN 64

/*
 * Structure for PAX target information
 */
typedef struct xl_pax_target
{
	RelFileNode node;
	uint16		file_name_len;
	int64		offset;
} xl_pax_target;

#define SizeOfPAXTarget (sizeof(xl_pax_target))

/*
 * Structure for PAX INSERT WAL record
 *
 * Layout:
 * +-----------------+
 * |  RelFileNode    |
 * +-----------------+
 * |  file_name_len  |
 * +-----------------+
 * |  offset         |
 * +-----------------+
 * |  file_name      |
 * +-----------------+
 * |  data           |
 * +-----------------+
 */
typedef struct xl_pax_insert
{
	xl_pax_target target;
	/* BLOCK DATA FOLLOWS AT END OF STRUCT */
} xl_pax_insert;

#define SizeOfPAXInsert (sizeof(xl_pax_insert))

/*
 * Structure for PAX INSERT REFERENCE DATA WAL record
 */
typedef struct xl_pax_insert_reference_data
{
	xl_pax_target target;
	int64		buffer_len;
} xl_pax_insert_reference_data;

#define SizeOfPAXInsertReferenceData (sizeof(xl_pax_insert_reference_data))

/*
 * Structure for PAX directory operations
 */
typedef struct xl_pax_directory
{
	RelFileNode node;
} xl_pax_directory;

#define SizeOfPAXDirectory sizeof(xl_pax_directory)

/*
 * Function declarations for PAX WAL record processing
 */
/*
 * Check if a WAL record is a PAX record that should be processed specially.
 */
extern bool IsPaxRecord(XLogReaderState *record);

/*
 * Get the PAX record type from a WAL record.
 */
extern uint8 GetPaxRecordType(XLogReaderState *record);

extern char *BuildPaxDirectoryPath(RelFileNode node, BackendId backend);

#endif							/* WALDATACOMM_H */ 