#include "storage/plan/pax_plan.h"

static planner_hook_type prev_planner = NULL;
static join_search_hook_type prev_join_search = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;

static Plan *pax_custom_plan_create(PlannerInfo *root, RelOptInfo *rel,
                                  CustomPath *best_path pg_attribute_unused(),
                                  List *tlist, List *clauses,
                                  List *custom_plans);

static void pax_custom_scan_begin(CustomScanState *node, EState *estate, int eflags) {
    PaxCustomScanState *pax_cscan_state = (PaxCustomScanState *)node;
    
    pax_cscan_state->aggresult = 0;
    pax_cscan_state->is_initialized = false;
    
    List *custom_private = ((CustomScan *)node->ss.ps.plan)->custom_private;
    if (custom_private != NIL) {
        ListCell *lc;
        foreach(lc, custom_private) {
            Node *n = (Node *)lfirst(lc);
            if (IsA(n, Const)) {
                Const *c = (Const *)n;
                if (c->consttype == OIDOID) {
                    pax_cscan_state->aggfnoid = DatumGetObjectId(c->constvalue);
                }
            }
        }
    }
}

static void pax_custom_scan_end(CustomScanState *node) {
    PaxCustomScanState *pax_cscan_state = (PaxCustomScanState *)node;
    pax_cscan_state->is_initialized = false;
}

static void pax_custom_scan_rescan(CustomScanState *node) {
    PaxCustomScanState *pax_cscan_state = (PaxCustomScanState *)node;
    pax_cscan_state->aggresult = 0;
    pax_cscan_state->is_initialized = false;
}

static void pax_custom_scan_explain(CustomScanState *node, List *ancestors, ExplainState *es) {
    PaxCustomScanState *pax_cscan_state = (PaxCustomScanState *)node;
    ExplainPropertyText("Custom Scan", "PaxCustomScan", es);
    ExplainPropertyText("Aggregate Function", get_func_name(pax_cscan_state->aggfnoid), es);
}

static TupleTableSlot *pax_custom_scan_exec(CustomScanState *node) {
    PaxCustomScanState *pax_cscan_state = (PaxCustomScanState *)node;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    
    if (pax_cscan_state->is_initialized) {
        return NULL;
    }
    
    // todo: read from pre-computed data
    pax_cscan_state->aggresult = 1000; 
    
    ExecClearTuple(slot);
    slot->tts_values[0] = Int64GetDatum(pax_cscan_state->aggresult);
    slot->tts_isnull[0] = false;
    ExecStoreVirtualTuple(slot);
    
    pax_cscan_state->is_initialized = true;
    return slot;
}

// exec methods
static struct CustomExecMethods pax_custom_scan_state_methods = {
    .CustomName = "PaxCustomScanState",
    .BeginCustomScan = pax_custom_scan_begin,
    .ExecCustomScan = pax_custom_scan_exec,
    .EndCustomScan = pax_custom_scan_end,
    .ReScanCustomScan = pax_custom_scan_rescan,
    .ExplainCustomScan = pax_custom_scan_explain,
};

static Node *pax_custom_scan_state_create(CustomScan *cscan) {
    PaxCustomScanState *pax_cscan_state = (PaxCustomScanState *)newNode(
        sizeof(PaxCustomScanState), T_CustomScanState);
    pax_cscan_state->cscan_state.methods = &pax_custom_scan_state_methods;
    return (Node *)pax_cscan_state;
}

// scan methods
static CustomScanMethods pax_custom_scan_methods = {
    .CustomName = "PaxCustomScan",
    .CreateCustomScanState = pax_custom_scan_state_create,
};

// path methods
static CustomPathMethods pax_custom_path_methods = {
    .CustomName = "PaxCustomScan",
    .PlanCustomPath = pax_custom_plan_create,
};

static Plan *pax_custom_plan_create(PlannerInfo *root, RelOptInfo *rel,
                                  CustomPath *best_path pg_attribute_unused(),
                                  List *tlist, List *clauses,
                                  List *custom_plans) {
    CustomScan *cscan = makeNode(CustomScan);
    cscan->scan.plan.targetlist = copyObject(tlist);
    cscan->scan.plan.qual = clauses;
    cscan->methods = &pax_custom_scan_methods;

    cscan->scan.plan.startup_cost = Cost(0.0);
    cscan->scan.plan.total_cost = Cost(1.0);
    cscan->scan.plan.plan_rows = 1.0;
    cscan->scan.plan.plan_width = 1;
    

    // save aggfnoid to custom_private
    List *custom_private = NIL;
    if (best_path->custom_private != NIL) {
        custom_private = list_copy(best_path->custom_private);
    }
    cscan->custom_private = custom_private;
    
    return &cscan->scan.plan;
}


static void find_aggregate_refs_walker(Node *node, List **aggrefs) {
    if (node == NULL)
        return;
        
    if (IsA(node, TargetEntry)) {
        TargetEntry *tle = (TargetEntry *)node;
    
        if (IsA(tle->expr, Aggref)) {
            *aggrefs = lappend(*aggrefs, tle->expr);
            return;
        }
        
        find_aggregate_refs_walker((Node *)tle->expr, aggrefs);
        return;
    }
    
    if (IsA(node, Aggref)) {
        *aggrefs = lappend(*aggrefs, node);
        return;
    }
    
    if (IsA(node, SubLink)) {
        SubLink *sublink = (SubLink *)node;
        find_aggregate_refs_walker(sublink->subselect, aggrefs);
        return;
    }
    
    if (IsA(node, Query)) {
        Query *query = (Query *)node;
        ListCell *lc;
        foreach(lc, query->targetList) {
            TargetEntry *tle = (TargetEntry *)lfirst(lc);
            find_aggregate_refs_walker((Node *)tle, aggrefs);
        }
        return;
    }
    
    if (IsA(node, List)) {
        List *list = (List *)node;
        ListCell *lc;
        foreach(lc, list) {
            find_aggregate_refs_walker((Node *)lfirst(lc), aggrefs);
        }
        return;
    }
}

static void find_aggregate_refs(Node *node, List **aggrefs) {
    find_aggregate_refs_walker(node, aggrefs);
}


static bool can_convert_to_custom_scan(PlannerInfo *root, RelOptInfo *rel) {
    if (!root->parse->hasAggs) {
        return false;
    }


    List *aggrefs = NIL;
    find_aggregate_refs((Node *)root->parse->targetList, &aggrefs);
    if (list_length(aggrefs) != 1) {
        return false;
    }

    // check if is count, sum, or other aggregate functions??
    Aggref *aggref = (Aggref *)linitial(aggrefs);
    if (aggref->aggfnoid != F_COUNT_ANY && 
        aggref->aggfnoid != F_SUM_INT4 && 
        aggref->aggfnoid != F_SUM_INT8) {
        return false;
    }

    return true;
}

static CustomPath *create_pax_custom_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath) {
    CustomPath *custom_path = makeNode(CustomPath);
    custom_path->path.pathtype = T_CustomScan;
    custom_path->path.parent = rel;
    custom_path->path.pathtarget = rel->reltarget;
    custom_path->path.param_info = subpath->param_info;
    custom_path->path.parallel_aware = false;
    custom_path->path.parallel_safe = subpath->parallel_safe;
    custom_path->path.parallel_workers = subpath->parallel_workers;
    custom_path->path.rows = subpath->rows;
    custom_path->path.startup_cost = subpath->startup_cost;
    custom_path->path.total_cost = subpath->total_cost;
    custom_path->path.pathkeys = subpath->pathkeys;
    custom_path->methods = &pax_custom_path_methods;

    custom_path->path.locus = subpath->locus;
    custom_path->path.motionHazard = subpath->motionHazard;
    custom_path->path.rescannable = subpath->rescannable;

    List *aggrefs = NIL;
    find_aggregate_refs((Node *)root->parse->targetList, &aggrefs);
    Aggref *aggref = (Aggref *)linitial(aggrefs);

    Const *const_node = makeNode(Const);
    const_node->consttype = OIDOID;
    const_node->consttypmod = -1;
    const_node->constcollid = InvalidOid;
    const_node->constlen = sizeof(Oid);
    const_node->constvalue = ObjectIdGetDatum(aggref->aggfnoid);
    const_node->constisnull = false;
    const_node->constbyval = true;
    
    custom_path->custom_private = list_make1(const_node);
    
    return custom_path;
}

void pax_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte) {
    if (prev_set_rel_pathlist) {
        prev_set_rel_pathlist(root, rel, rti, rte);
    }

    // check if can convert to custom scan
    if (can_convert_to_custom_scan(root, rel)) {
        // find seqscan path
        Path *seqscan_path = NULL;
        List *new_pathlist pg_attribute_unused() = NULL ;
        ListCell *lc;
        
        foreach(lc, rel->pathlist) {
            Path *path = (Path *)lfirst(lc);
            if (IsA(path, Path) && path->pathtype == T_SeqScan) {
                seqscan_path = path;
                break;
             } else {
                new_pathlist = lappend(new_pathlist, path);
            }
        }

        if (seqscan_path) {
            // create custom path
            CustomPath *custom_path = create_pax_custom_path(root, rel, seqscan_path);

            custom_path->path.startup_cost = Cost(0.0);
            custom_path->path.total_cost = Cost(1.0);
            
            rel->pathlist = new_pathlist;
            add_path(rel, (Path *)custom_path, root);
        }
    }
}

// join search
RelOptInfo *pax_join_search(PlannerInfo *root, int levels_needed, List *initial_rels) {
    if (prev_join_search) {
        return prev_join_search(root, levels_needed, initial_rels);
    } else {
        return standard_join_search(root, levels_needed, initial_rels);
    }
}

PlannedStmt *pax_planner(Query *parse, const char *query_string, int cursor_opts, ParamListInfo bound_params) {
    if (prev_planner) {
        return prev_planner(parse, query_string, cursor_opts, bound_params);
    } else {
        return standard_planner(parse, query_string, cursor_opts, bound_params);
    }
}

void pax_planner_init(void) {
    prev_planner = planner_hook;
    prev_join_search = join_search_hook;
    prev_set_rel_pathlist = set_rel_pathlist_hook;

    planner_hook = pax_planner;
    join_search_hook = pax_join_search;
    set_rel_pathlist_hook = pax_set_rel_pathlist;

    // register custom scan methods
    RegisterCustomScanMethods(&pax_custom_scan_methods);
}


void pax_planner_fini(void) {
    planner_hook = prev_planner;
    join_search_hook = prev_join_search;
    set_rel_pathlist_hook = prev_set_rel_pathlist;
}
