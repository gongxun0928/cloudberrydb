-- @Description Tests the visibility when a cursor has been created before the update.
-- 
DROP TABLE IF EXISTS pax_tbl;
CREATE TABLE pax_tbl (a INT, b INT);
INSERT INTO pax_tbl SELECT i as a, i as b  FROM generate_series(1,100) AS i;

1: BEGIN;
1: DECLARE cur CURSOR FOR SELECT a,b FROM pax_tbl ORDER BY a;
1: FETCH NEXT IN cur;
1: FETCH NEXT IN cur;
2: BEGIN;
2: UPDATE pax_tbl SET b = 8 WHERE a < 5;
2: COMMIT;
1: FETCH NEXT IN cur;
1: FETCH NEXT IN cur;
1: FETCH NEXT IN cur;
1: CLOSE cur;
1: COMMIT;
3: BEGIN;
3: DECLARE cur CURSOR FOR SELECT a,b FROM pax_tbl ORDER BY a;
3: FETCH NEXT IN cur;
3: CLOSE cur;
3: COMMIT;
