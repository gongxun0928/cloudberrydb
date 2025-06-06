#pragma once

#include "comm/cbdb_api.h"

typedef struct PaxCustomScanState {
    CustomScanState cscan_state;
    Oid aggfnoid;          
    Oid aggtype;           
    Oid aggcollid;         
    Oid aggtranstype;      
    int64 aggresult;       
    bool is_initialized;   
} PaxCustomScanState;

extern void pax_planner_init(void);
extern void pax_planner_fini(void);

extern void pax_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte);
extern RelOptInfo *pax_join_search(PlannerInfo *root, int levels_needed, List *initial_rels);
extern PlannedStmt *pax_planner(Query *parse, const char *query_string, int cursor_opts, ParamListInfo bound_params);



