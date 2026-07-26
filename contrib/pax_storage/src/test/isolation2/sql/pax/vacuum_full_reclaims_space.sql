-- Regression coverage for https://github.com/apache/cloudberry/issues/1766.
--
-- PAX stores deletes in visibility maps.  Lazy VACUUM cannot compact the
-- immutable micro-partitions, while VACUUM FULL must rewrite only live rows.

CREATE EXTENSION IF NOT EXISTS gp_inject_fault;

DROP TABLE IF EXISTS pax_vacuum_full_repro;

CREATE TABLE pax_vacuum_full_repro (
    id int,
    payload text
) USING PAX DISTRIBUTED BY (id);

CREATE UNIQUE INDEX pax_vacuum_full_repro_id_idx
    ON pax_vacuum_full_repro(id);

INSERT INTO pax_vacuum_full_repro
SELECT g, repeat(md5(g::text), 20)
FROM generate_series(1, 100000) AS g;

CREATE TEMP TABLE pax_vacuum_full_size_before (
    bytes bigint
) USING heap DISTRIBUTED RANDOMLY;

INSERT INTO pax_vacuum_full_size_before
SELECT pg_total_relation_size('pax_vacuum_full_repro');

DELETE FROM pax_vacuum_full_repro WHERE id <= 90000;

-- Lazy VACUUM cannot compact immutable micro-partitions and leaves the
-- physical size unchanged.
VACUUM pax_vacuum_full_repro;

SELECT pg_total_relation_size('pax_vacuum_full_repro') =
       (SELECT bytes FROM pax_vacuum_full_size_before) AS lazy_size_unchanged;

-- An aborted rewrite must discard its unfinished PAX DML state.  The next
-- VACUUM FULL in the same test session must be able to rewrite the table.
-- start_ignore
SELECT gp_inject_fault('orc_writer_write_tuple', 'error', dbid)
FROM gp_segment_configuration
WHERE role = 'p' AND content >= 0;
-- end_ignore

VACUUM FULL pax_vacuum_full_repro;

-- start_ignore
SELECT gp_inject_fault('orc_writer_write_tuple', 'reset', dbid)
FROM gp_segment_configuration
WHERE role = 'p' AND content >= 0;
-- end_ignore

VACUUM FULL pax_vacuum_full_repro;

-- The rewrite keeps exactly the visible rows and reclaims the deleted rows'
-- storage.
SELECT count(*) AS live_rows, min(id) AS min_id, max(id) AS max_id
FROM pax_vacuum_full_repro;

SELECT pg_total_relation_size('pax_vacuum_full_repro') <
       (SELECT bytes FROM pax_vacuum_full_size_before) AS full_size_reduced;

-- relation_copy_for_cluster reports the copied tuple count on each segment.
SELECT sum(reltuples)::int AS segment_reltuples
FROM gp_dist_random('pg_class')
WHERE relname = 'pax_vacuum_full_repro';

-- VACUUM FULL rebuilds indexes after swapping in the rewritten relation.
SELECT indisvalid, indisready
FROM pg_index
WHERE indexrelid = 'pax_vacuum_full_repro_id_idx'::regclass;

SET optimizer = off;
SET enable_seqscan = off;

SELECT id, length(payload) AS payload_length
FROM pax_vacuum_full_repro
WHERE id = 95000;

RESET enable_seqscan;
RESET optimizer;

DROP TABLE pax_vacuum_full_repro;

-- Rewriting must also preserve values stored in PAX external-toast files.
CREATE TABLE pax_vacuum_full_toast (
    id int,
    payload text
) USING PAX DISTRIBUTED BY (id);

ALTER TABLE pax_vacuum_full_toast
    ALTER COLUMN payload SET STORAGE EXTERNAL;

INSERT INTO pax_vacuum_full_toast
SELECT g, repeat(md5(g::text), 350000)
FROM generate_series(1, 3) AS g;

DELETE FROM pax_vacuum_full_toast WHERE id < 3;
VACUUM FULL pax_vacuum_full_toast;

SELECT count(*) AS live_rows, min(id) AS min_id, max(id) AS max_id,
       min(length(payload)) AS min_payload_length,
       max(length(payload)) AS max_payload_length
FROM pax_vacuum_full_toast;

DROP TABLE pax_vacuum_full_toast;
