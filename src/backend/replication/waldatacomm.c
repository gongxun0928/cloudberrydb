/*-------------------------------------------------------------------------
 *
 * waldatacomm.c
 *
 * Implementation of shared data structures and functions for WAL data
 * communication between walsender and walreceiver processes.
 *
 * Portions Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/waldatacomm.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xlogreader.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/pg_lsn.h"

#include "replication/waldatacomm.h"

/*
 * Check if a WAL record is a PAX record that should be processed specially.
 */
bool
IsPaxRecord(XLogReaderState *record)
{
	return XLogRecGetRmid(record) == PAX_RMGR_ID;
}

/*
 * Get the PAX record type from a WAL record.
 */
uint8
GetPaxRecordType(XLogReaderState *record)
{
	if (!IsPaxRecord(record))
		return 0xFF; /* Invalid record type */
	
	return XLogRecGetInfo(record) & ~XLR_INFO_MASK;
}

/*
 * Build PAX directory path for a given RelFileNode.
 * This is a simplified version that doesn't depend on PAX extension internals.
 */
char *
BuildPaxDirectoryPath(RelFileNode node, BackendId backend)
{
	char	   *relpath;
	char	   *paxpath;
	
	/* Get the base relation path */
	relpath = relpathbackend(node, backend, MAIN_FORKNUM);
	
	/* Append "_pax" suffix for PAX storage */
	paxpath = psprintf("%s_pax", relpath);
	pfree(relpath);
	
	return paxpath;
}
