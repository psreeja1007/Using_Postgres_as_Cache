#include "postgres.h"
#include "executor/executor.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "optimizer/planner.h"
#include "tcop/utility.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "catalog/pg_class.h"
#include "catalog/namespace.h"
#include "access/htup_details.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "parser/parser.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"

PG_MODULE_MAGIC;



typedef struct CacheRouterTag {
    char *table;
    char *pk_col;
    char *pk_val;
} CacheRouterTag;

static char          *cache_router_master_connstr = NULL;
static bool           cache_router_active         = false;


static CacheRouterTag *pending_tag = NULL;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static planner_hook_type       prev_planner       = NULL;

void _PG_init(void);
void _PG_fini(void);

static void         cache_router_ExecutorStart(QueryDesc *queryDesc, int eflags);
static bool         is_pk_equality_select(PlannedStmt *pstmt,
                                          char **out_table, char **out_pk_col,
                                          char **out_pk_val);
static bool         try_serve_from_cache(const char *table, const char *pk_col,
                                         const char *pk_val);
static void         populate_cache_from_master(const char *table, const char *pk_col,
                                               const char *pk_val);
static PlannedStmt *cache_router_planner(Query *parse,
                                         const char *query_string,
                                         int cursorOptions,
                                         ParamListInfo boundParams);
static PlannedStmt *rewrite_to_dblink(const char *query_string,
                                      const char *table_name,
                                      const char *result_schema);
static PlannedStmt *rewrite_write_to_dblink_exec(const char *query_string,
                                                  const char *table_name);
static PlannedStmt *rewrite_write_returning_to_dblink(const char *query_string,
                                                       const char *table_name,
                                                       const char *result_schema);
static bool         is_pk_equality_select_query(Query *query,
                                                char **out_table,
                                                char **out_pk_col,
                                                char **out_pk_val);
static char        *get_targetlist_schema_for_dblink(List *targetList);
static char        *get_result_relation_name(Query *parse);

typedef struct {
    const char *table_name;
    const char *pk_col;
} TablePKMap;

static const char *master_routed_tables[] = {
    "student",
    "instructor",
    "course",
    "department",
    "takes",
    "advisor",
    "time_slot",
    "prereq",
    "teaches",
    "section",
    "classroom",
    NULL
};

static const TablePKMap pk_map[] = {
    { "student",    "id"        },
    { "instructor", "id"        },
    { "course",     "course_id" },
    { "department", "dept_name" },
    { NULL, NULL }
};

static const char *
get_pk_col(const char *table_name)
{
    int i;
    for (i = 0; pk_map[i].table_name != NULL; i++)
        if (strcmp(pk_map[i].table_name, table_name) == 0)
            return pk_map[i].pk_col;
    return NULL;
}

static bool
is_master_routed_table(const char *table_name)
{
    int i;
    if (table_name == NULL)
        return false;
    for (i = 0; master_routed_tables[i] != NULL; i++)
        if (strcmp(master_routed_tables[i], table_name) == 0)
            return true;
    return false;
}

static char *
get_table_name(Query *parse)
{
    RangeTblEntry *rte;
    if (list_length(parse->rtable) != 1)
        return NULL;
    rte = linitial(parse->rtable);
    if (rte->rtekind != RTE_RELATION)
        return NULL;
    return get_rel_name(rte->relid);
}

static char *
get_result_relation_name(Query *parse)
{
    RangeTblEntry *rte;
    if (parse->resultRelation <= 0)
        return NULL;
    if (parse->resultRelation > list_length(parse->rtable))
        return NULL;
    rte = (RangeTblEntry *) list_nth(parse->rtable, parse->resultRelation - 1);
    if (rte == NULL || rte->rtekind != RTE_RELATION)
        return NULL;
    return get_rel_name(rte->relid);
}

static char *
get_table_schema_for_dblink(const char *table)
{
    StringInfoData  buf;
    StringInfoData  sql;
    int             ret;
    int             i;

    initStringInfo(&buf);
    initStringInfo(&sql);

    appendStringInfo(&sql,
        "SELECT attname, format_type(atttypid, atttypmod) "
        "FROM pg_attribute a "
        "JOIN pg_class c ON a.attrelid = c.oid "
        "WHERE c.relname = '%s' AND a.attnum > 0 AND NOT attisdropped",
        table);

    SPI_connect();
    ret = SPI_exec(sql.data, 0);
    (void) ret;

    for (i = 0; i < SPI_processed; i++)
    {
        char *col = SPI_getvalue(SPI_tuptable->vals[i],
                                 SPI_tuptable->tupdesc, 1);
        char *typ = SPI_getvalue(SPI_tuptable->vals[i],
                                 SPI_tuptable->tupdesc, 2);
        if (i > 0)
            appendStringInfoString(&buf, ", ");
        appendStringInfo(&buf, "%s %s", col, typ);
    }

    SPI_finish();
    return buf.data;
}

static char *
get_targetlist_schema_for_dblink(List *targetList)
{
    StringInfoData  buf;
    ListCell       *lc;
    int             colno = 1;

    initStringInfo(&buf);

    foreach(lc, targetList)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(lc);
        Oid          typid;
        int32        typmod;
        char        *typname;
        const char  *colname;
        char         fallback_name[NAMEDATALEN];

        if (tle->resjunk)
            continue;

        typid   = exprType((Node *) tle->expr);
        typmod  = exprTypmod((Node *) tle->expr);
        typname = format_type_with_typemod(typid, typmod);

        if (tle->resname != NULL && tle->resname[0] != '\0')
        {
            colname = quote_identifier(tle->resname);
        }
        else
        {
            snprintf(fallback_name, sizeof(fallback_name), "col%d", colno);
            colname = quote_identifier(fallback_name);
        }

        if (buf.len > 0)
            appendStringInfoString(&buf, ", ");
        appendStringInfo(&buf, "%s %s", colname, typname);
        colno++;
    }

    return buf.data;
}

void
_PG_init(void)
{
    DefineCustomStringVariable(
        "cache_router.master_connstr",
        "libpq connection string for the master PostgreSQL instance",
        NULL,
        &cache_router_master_connstr,
        "host=localhost port=5432 dbname=universitydb user=postgres",
        PGC_SUSET,
        0, NULL, NULL, NULL
    );

    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = cache_router_ExecutorStart;

    prev_planner = planner_hook;
    planner_hook = cache_router_planner;

    ereport(LOG, (errmsg("pg_cache_router loaded: master = %s",
                          cache_router_master_connstr)));
}

void
_PG_fini(void)
{
    ExecutorStart_hook = prev_ExecutorStart;
    planner_hook       = prev_planner;
}


static PlannedStmt *
cache_router_planner(Query *parse,
                     const char *query_string,
                     int cursorOptions,
                     ParamListInfo boundParams)
{
    PlannedStmt    *result;
    char           *table;
    char           *pk_col;
    char           *pk_val;
    char           *table_name;
    bool            old;
    char           *result_schema;
    PlannedStmt    *ps;
    CacheRouterTag *tag;

    table      = NULL;
    pk_col     = NULL;
    pk_val     = NULL;
    table_name = NULL;

    if (parse->commandType == CMD_SELECT && !cache_router_active &&
        parse->rtable != NIL)
    {
        table_name = get_table_name(parse);

        if (!is_pk_equality_select_query(parse, &table, &pk_col, &pk_val))
        {
            /* Not a cacheable PK lookup */
            if (!is_master_routed_table(table_name))
            {
                if (prev_planner)
                    return prev_planner(parse, query_string, cursorOptions, boundParams);
                else
                    return standard_planner(parse, query_string, cursorOptions, boundParams);
            }

            /* Routed table but not a cacheable PK lookup -> route to master */
            ereport(LOG, (errmsg("cache_router: routing to master")));
            
            old = cache_router_active;
            cache_router_active = true;
            result_schema = get_targetlist_schema_for_dblink(parse->targetList);
            ps = rewrite_to_dblink(query_string, table_name, result_schema);
            cache_router_active = old;
            return ps;
        }

        if (get_pk_col(table_name) == NULL && is_master_routed_table(table_name))
        {
            ereport(LOG, (errmsg("cache_router: non-cached table -> master routing")));
            old = cache_router_active;
            cache_router_active = true;
            result_schema = get_targetlist_schema_for_dblink(parse->targetList);
            ps = rewrite_to_dblink(query_string, table_name, result_schema);
            cache_router_active = old;
            return ps;
        }

        
        if (prev_planner)
            result = prev_planner(parse, query_string, cursorOptions, boundParams);
        else
            result = standard_planner(parse, query_string, cursorOptions, boundParams);

        tag         = (CacheRouterTag *) palloc(sizeof(CacheRouterTag));
        tag->table  = pstrdup(table);
        tag->pk_col = pstrdup(pk_col);
        tag->pk_val = pstrdup(pk_val);
        pending_tag = tag;

        return result;
    }
    else if (!cache_router_active &&
             (parse->commandType == CMD_INSERT ||
              parse->commandType == CMD_UPDATE ||
              parse->commandType == CMD_DELETE))
    {
        table_name = get_result_relation_name(parse);

        if (is_master_routed_table(table_name))
        {
            ereport(LOG,
                    (errmsg("cache_router: routing write to master table=%s",
                             table_name)));

            old = cache_router_active;
            cache_router_active = true;

            if (parse->returningList != NIL)
            {
                result_schema = get_targetlist_schema_for_dblink(parse->returningList);
                ps = rewrite_write_returning_to_dblink(query_string,
                                                       table_name,
                                                       result_schema);
            }
            else
            {
                ps = rewrite_write_to_dblink_exec(query_string, table_name);
            }

            cache_router_active = old;
            return ps;
        }
    }

    if (prev_planner)
        result = prev_planner(parse, query_string, cursorOptions, boundParams);
    else
        result = standard_planner(parse, query_string, cursorOptions, boundParams);

    return result;
}

static void
cache_router_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    CacheRouterTag *tag;
    bool            hit;
    char           *result_schema;
    PlannedStmt    *newplan;
    bool            old;
    char           *fb_table;
    char           *fb_pk_col;
    char           *fb_pk_val;

    tag = NULL;

    if (!cache_router_active &&
        queryDesc->operation == CMD_SELECT)
    {
        if (pending_tag != NULL)
        {
            tag         = pending_tag;
            pending_tag = NULL;  
        }
        else
        {
            
            fb_table  = NULL;
            fb_pk_col = NULL;
            fb_pk_val = NULL;

            if (is_pk_equality_select(queryDesc->plannedstmt,
                                      &fb_table, &fb_pk_col, &fb_pk_val))
            {
                tag         = (CacheRouterTag *) palloc(sizeof(CacheRouterTag));
                tag->table  = fb_table;
                tag->pk_col = fb_pk_col;
                tag->pk_val = fb_pk_val;
            }
        }

        if (tag != NULL)
        {
            cache_router_active = true;

            PG_TRY();
            {
                hit = try_serve_from_cache(tag->table, tag->pk_col, tag->pk_val);

                if (!hit)
                {
                    populate_cache_from_master(tag->table, tag->pk_col, tag->pk_val);
                    CommandCounterIncrement();

                    ereport(LOG,
                            (errmsg("cache_router: MISS -> routing to master for result")));

                    old = cache_router_active;
                    cache_router_active = true;

                    result_schema =
                        get_targetlist_schema_for_dblink(
                            queryDesc->plannedstmt->planTree->targetlist);

                    newplan = rewrite_to_dblink(queryDesc->sourceText,
                                               tag->table,
                                               result_schema);

                    cache_router_active = old;
                    queryDesc->plannedstmt = newplan;
                }
            }
            PG_CATCH();
            {
                cache_router_active = false;
                pending_tag = NULL;
                PG_RE_THROW();
            }
            PG_END_TRY();

            cache_router_active = false;
        }
    }

    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

static bool
is_pk_equality_select(PlannedStmt *pstmt,
                      char **out_table, char **out_pk_col, char **out_pk_val)
{
    RangeTblEntry *rte;
    Oid            relid;
    char          *rel_name;
    const char    *pk_col;
    Plan          *top_plan;
    List          *quals;
    Node          *qual_node;
    OpExpr        *opexpr;
    char          *opname;
    Node          *left, *right;
    Var           *var_node   = NULL;
    Const         *const_node = NULL;
    AttrNumber     pk_attno;
    Oid            typoutput;
    bool           typisvarlena;
    char          *val_str;

    if (pstmt == NULL || pstmt->commandType != CMD_SELECT)
        return false;

    if (list_length(pstmt->rtable) != 1)
        return false;

    rte = (RangeTblEntry *) linitial(pstmt->rtable);
    if (rte->rtekind != RTE_RELATION)
        return false;

    relid    = rte->relid;
    rel_name = get_rel_name(relid);
    if (rel_name == NULL)
        return false;

    pk_col = get_pk_col(rel_name);
    if (pk_col == NULL)
        return false;

    top_plan = pstmt->planTree;

    if (IsA(top_plan, SeqScan))
    {
        quals = ((SeqScan *) top_plan)->scan.plan.qual;
    }
    else if (IsA(top_plan, IndexScan))
    {
        IndexScan *idx = (IndexScan *) top_plan;
        quals = idx->indexqual ? idx->indexqual : idx->scan.plan.qual;
    }
    else if (IsA(top_plan, IndexOnlyScan))
    {
        IndexOnlyScan *idx = (IndexOnlyScan *) top_plan;
        quals = idx->indexqual ? idx->indexqual : idx->scan.plan.qual;
    }
    else if (IsA(top_plan, BitmapHeapScan))
    {
        quals = ((BitmapHeapScan *) top_plan)->scan.plan.qual;
    }
    else
    {
        return false;
    }

    if (list_length(quals) != 1)
        return false;

    qual_node = (Node *) linitial(quals);
    if (!IsA(qual_node, OpExpr))
        return false;

    opexpr = (OpExpr *) qual_node;
    opname = get_opname(opexpr->opno);
    if (opname == NULL || strcmp(opname, "=") != 0)
        return false;

    if (list_length(opexpr->args) != 2)
        return false;

    left  = (Node *) linitial(opexpr->args);
    right = (Node *) lsecond(opexpr->args);

    if (IsA(left, Var) && IsA(right, Const))
    {
        var_node   = (Var *)   left;
        const_node = (Const *) right;
    }
    else if (IsA(right, Var) && IsA(left, Const))
    {
        var_node   = (Var *)   right;
        const_node = (Const *) left;
    }
    else
        return false;

    pk_attno = get_attnum(relid, pk_col);
    if (pk_attno == InvalidAttrNumber)
        return false;
    if (var_node->varattno != pk_attno)
        return false;

    getTypeOutputInfo(const_node->consttype, &typoutput, &typisvarlena);
    val_str = OidOutputFunctionCall(typoutput, const_node->constvalue);

    *out_table  = pstrdup(rel_name);
    *out_pk_col = pstrdup(pk_col);
    *out_pk_val = val_str;
    return true;
}


static bool
try_serve_from_cache(const char *table, const char *pk_col, const char *pk_val)
{
    int             ret;
    bool            found;
    StringInfoData  sql;
    char           *safe_pk;
    char           *safe_tbl;
    const char     *safe_table_ident;
    const char     *safe_pk_col_ident;

    safe_pk           = quote_literal_cstr(pk_val);
    safe_tbl          = quote_literal_cstr(table);
    safe_table_ident  = quote_identifier(table);
    safe_pk_col_ident = quote_identifier(pk_col);

    initStringInfo(&sql);

  
    appendStringInfo(&sql,
        "WITH hit AS MATERIALIZED ("
        "  SELECT 1"
        "  FROM cache_lru_meta m"
        "  JOIN %s d ON d.%s::TEXT = m.pk_value"
        "  WHERE m.table_name = %s AND m.pk_value = %s"
        ") "
        "UPDATE cache_lru_meta SET last_access = now()"
        "  WHERE table_name = %s AND pk_value = %s"
        "    AND EXISTS (SELECT 1 FROM hit)"
        " RETURNING 1",
        safe_table_ident, safe_pk_col_ident,
        safe_tbl, safe_pk,
        safe_tbl, safe_pk);

    SPI_connect();
    ret   = SPI_exec(sql.data, 1);
    found = (ret == SPI_OK_UPDATE_RETURNING && SPI_processed > 0);

    if (!found)
    {
        /*
         * Miss or stale meta.  Clean up stale meta in the same session
         * so the hot (hit) path pays zero extra cost.
         */
        // resetStringInfo(&sql);
        // appendStringInfo(&sql,
        //     "DELETE FROM cache_lru_meta"
        //     "  WHERE table_name = %s AND pk_value = %s"
        //     "    AND NOT EXISTS ("
        //     "        SELECT 1 FROM %s WHERE %s::TEXT = %s"
        //     "    )",
        //     safe_tbl, safe_pk,
        //     safe_table_ident, safe_pk_col_ident, safe_pk);

        // SPI_exec(sql.data, 0);   /* fire-and-forget */

        ereport(DEBUG1,
                (errmsg("cache_router: MISS table=%s pk=%s", table, pk_val)));
    }
    else
    {
        ereport(DEBUG1,
                (errmsg("cache_router: HIT  table=%s pk=%s", table, pk_val)));
    }

    SPI_finish();
    return found;
}


static void
populate_cache_from_master(const char *table, const char *pk_col,
                           const char *pk_val)
{
    StringInfoData  sql;
    StringInfoData  remote_sql;
    int             ret;
    bool            isnull;
    Datum           json_datum;
    char           *json;
    char           *json_copy;
    char           *safe_connstr;
    char           *safe_pk;
    size_t          len;

    safe_connstr = quote_literal_cstr(cache_router_master_connstr);
    safe_pk      = quote_literal_cstr(pk_val);

    len = strlen(safe_pk);
    safe_pk[len - 1] = '\0';
    safe_pk++;          /* skip leading quote */

    initStringInfo(&remote_sql);
    appendStringInfo(&remote_sql,
        "SELECT row_to_json(t) FROM %s t WHERE %s = '%s'",
        table, pk_col, safe_pk);

    initStringInfo(&sql);
    appendStringInfo(&sql,
        "SELECT * FROM dblink(%s, %s) AS t(row_data json)",
        safe_connstr,
        quote_literal_cstr(remote_sql.data));

    {
        int chk;
        SPI_connect();
        chk = SPI_exec(
            "SELECT 1 FROM pg_proc p "
            "JOIN pg_namespace n ON n.oid = p.pronamespace "
            "WHERE p.proname = 'dblink' AND n.nspname = 'public'", 1);
        if (chk != SPI_OK_SELECT || SPI_processed == 0)
        {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION),
                     errmsg("cache_router: dblink extension is not installed"),
                     errhint("Run: CREATE EXTENSION IF NOT EXISTS dblink; "
                             "as a superuser, then retry.")));
            return;
        }
        SPI_finish();
    }

    SPI_connect();
    ret = SPI_exec(sql.data, 1);
    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        SPI_finish();
        return;
    }

    json_datum = SPI_getbinval(SPI_tuptable->vals[0],
                               SPI_tuptable->tupdesc,
                               1, &isnull);
    if (isnull)
    {
        SPI_finish();
        return;
    }

    json      = TextDatumGetCString(json_datum);
    json_copy = pstrdup(json);   /* survive SPI_finish */
    SPI_finish();

    {
        StringInfoData  insert_sql;
        char           *safe_json = quote_literal_cstr(json_copy);

        initStringInfo(&insert_sql);

        if (strcmp(table, "student") == 0)
        {
            appendStringInfo(&insert_sql,
                "SELECT cache_insert_student("
                "  d->>'id',"
                "  d->>'name',"
                "  d->>'dept_name',"
                "  (d->>'tot_cred')::NUMERIC"
                ") FROM (SELECT %s::json AS d) t",
                safe_json);
        }
        else if (strcmp(table, "instructor") == 0)
        {
            appendStringInfo(&insert_sql,
                "SELECT cache_insert_instructor("
                "  d->>'id',"
                "  d->>'name',"
                "  d->>'dept_name',"
                "  (d->>'salary')::NUMERIC"
                ") FROM (SELECT %s::json AS d) t",
                safe_json);
        }
        else if (strcmp(table, "course") == 0)
        {
            appendStringInfo(&insert_sql,
                "SELECT cache_insert_course("
                "  d->>'course_id',"
                "  d->>'title',"
                "  d->>'dept_name',"
                "  (d->>'credits')::NUMERIC"
                ") FROM (SELECT %s::json AS d) t",
                safe_json);
        }
        else if (strcmp(table, "department") == 0)
        {
            appendStringInfo(&insert_sql,
                "SELECT cache_insert_department("
                "  d->>'dept_name',"
                "  d->>'building',"
                "  (d->>'budget')::NUMERIC"
                ") FROM (SELECT %s::json AS d) t",
                safe_json);
        }

        if (insert_sql.len > 0)
        {
            SPI_connect();
            SPI_exec(insert_sql.data, 0);
            SPI_finish();
            ereport(DEBUG1,
                    (errmsg("cache_router: populated table=%s pk=%s from master",
                             table, pk_val)));
        }
    }
}

static PlannedStmt *
rewrite_to_dblink(const char *query_string,
                  const char *table_name,
                  const char *result_schema)
{
    StringInfoData  new_sql;
    List           *raw;
    List           *analyzed;
    List           *planned;

    initStringInfo(&new_sql);

    if (result_schema == NULL || result_schema[0] == '\0')
        result_schema = get_table_schema_for_dblink(table_name);

    if (result_schema == NULL || result_schema[0] == '\0')
        ereport(ERROR,
                (errmsg("cache_router: could not determine schema for table \"%s\"",
                        table_name)));

    appendStringInfo(&new_sql,
        "SELECT * FROM dblink(%s, %s) AS t(%s)",
        quote_literal_cstr(cache_router_master_connstr),
        quote_literal_cstr(query_string),
        result_schema);

    raw = pg_parse_query(new_sql.data);
    if (list_length(raw) == 0)
        ereport(ERROR, (errmsg("cache_router: parse failed")));

    analyzed = pg_analyze_and_rewrite_fixedparams(
        linitial(raw), new_sql.data, NULL, 0, NULL);

    planned = pg_plan_queries(analyzed, new_sql.data, 0, NULL);
    return linitial(planned);
}

static PlannedStmt *
rewrite_write_to_dblink_exec(const char *query_string, const char *table_name)
{
    StringInfoData  new_sql;
    List           *raw;
    List           *analyzed;
    List           *planned;

    initStringInfo(&new_sql);

    // if (get_pk_col(table_name) != NULL)
    // {
    //     appendStringInfo(&new_sql,
    //         "WITH remote AS MATERIALIZED ("
    //         "  SELECT dblink_exec(%s, %s) AS status"
    //         "), purge_rows AS ("
    //         "  DELETE FROM %s"
    //         "), purge_meta AS ("
    //         "  DELETE FROM cache_lru_meta WHERE table_name = %s"
    //         ") "
    //         "SELECT status FROM remote",
    //         quote_literal_cstr(cache_router_master_connstr),
    //         quote_literal_cstr(query_string),
    //         quote_identifier(table_name),
    //         quote_literal_cstr(table_name));
    // }
    // else
    // {
    //     appendStringInfo(&new_sql,
    //         "SELECT dblink_exec(%s, %s) AS status",
    //         quote_literal_cstr(cache_router_master_connstr),
    //         quote_literal_cstr(query_string));
    // }

    if (get_pk_col(table_name) != NULL)
    {
        appendStringInfo(&new_sql,
            "SELECT dblink_exec(%s, %s) AS status",
            quote_literal_cstr(cache_router_master_connstr),
            quote_literal_cstr(query_string));
    }
    else
    {
        appendStringInfo(&new_sql,
            "SELECT dblink_exec(%s, %s) AS status",
            quote_literal_cstr(cache_router_master_connstr),
            quote_literal_cstr(query_string));
    }

    raw = pg_parse_query(new_sql.data);
    if (list_length(raw) == 0)
        ereport(ERROR, (errmsg("cache_router: parse failed")));

    analyzed = pg_analyze_and_rewrite_fixedparams(
        linitial(raw), new_sql.data, NULL, 0, NULL);

    planned = pg_plan_queries(analyzed, new_sql.data, 0, NULL);
    return linitial(planned);
}

static PlannedStmt *
rewrite_write_returning_to_dblink(const char *query_string,
                                   const char *table_name,
                                   const char *result_schema)
{
    StringInfoData  new_sql;
    List           *raw;
    List           *analyzed;
    List           *planned;

    initStringInfo(&new_sql);

    if (result_schema == NULL || result_schema[0] == '\0')
        result_schema = get_table_schema_for_dblink(table_name);

    if (result_schema == NULL || result_schema[0] == '\0')
        ereport(ERROR,
                (errmsg("cache_router: could not determine schema for table \"%s\"",
                        table_name)));

    // if (get_pk_col(table_name) != NULL)
    // {
    //     appendStringInfo(&new_sql,
    //         "WITH remote AS MATERIALIZED ("
    //         "  SELECT * FROM dblink(%s, %s) AS t(%s)"
    //         "), purge_rows AS ("
    //         "  DELETE FROM %s"
    //         "), purge_meta AS ("
    //         "  DELETE FROM cache_lru_meta WHERE table_name = %s"
    //         ") "
    //         "SELECT * FROM remote",
    //         quote_literal_cstr(cache_router_master_connstr),
    //         quote_literal_cstr(query_string),
    //         result_schema,
    //         quote_identifier(table_name),
    //         quote_literal_cstr(table_name));
    // }
    // else
    // {
    //     appendStringInfo(&new_sql,
    //         "SELECT * FROM dblink(%s, %s) AS t(%s)",
    //         quote_literal_cstr(cache_router_master_connstr),
    //         quote_literal_cstr(query_string),
    //         result_schema);
    // }

    if (get_pk_col(table_name) != NULL)
    {
        appendStringInfo(&new_sql,
            "SELECT * FROM dblink(%s, %s) AS t(%s)",
            quote_literal_cstr(cache_router_master_connstr),
            quote_literal_cstr(query_string),
            result_schema);
    }
    else
    {
        appendStringInfo(&new_sql,
            "SELECT * FROM dblink(%s, %s) AS t(%s)",
            quote_literal_cstr(cache_router_master_connstr),
            quote_literal_cstr(query_string),
            result_schema);
    }

    raw = pg_parse_query(new_sql.data);
    if (list_length(raw) == 0)
        ereport(ERROR, (errmsg("cache_router: parse failed")));

    analyzed = pg_analyze_and_rewrite_fixedparams(
        linitial(raw), new_sql.data, NULL, 0, NULL);

    planned = pg_plan_queries(analyzed, new_sql.data, 0, NULL);
    return linitial(planned);
}

static bool
is_pk_equality_select_query(Query *query,
                             char **out_table,
                             char **out_pk_col,
                             char **out_pk_val)
{
    RangeTblEntry  *rte;
    char           *rel_name;
    const char     *pk_col;
    Node           *qual;
    OpExpr         *op;
    Node           *left;
    Node           *right;
    char           *opname;
    Var            *var;
    Const          *con;
    AttrNumber      pk_attno;
    Oid             typoutput;
    bool            typisvarlena;
    char           *val;

    if (query->commandType != CMD_SELECT)
        return false;
    if (list_length(query->rtable) != 1)
        return false;

    rte = linitial(query->rtable);
    if (rte->rtekind != RTE_RELATION)
        return false;

    rel_name = get_rel_name(rte->relid);
    pk_col   = get_pk_col(rel_name);
    if (!pk_col)
        return false;

    if (!query->jointree || !query->jointree->quals)
        return false;

    qual = query->jointree->quals;
    if (!IsA(qual, OpExpr))
        return false;

    op = (OpExpr *) qual;
    if (list_length(op->args) != 2)
        return false;

    left  = linitial(op->args);
    right = lsecond(op->args);

    /* unwrap RelabelType if present */
    if (IsA(right, RelabelType))
        right = (Node *) ((RelabelType *) right)->arg;
    if (IsA(left, RelabelType))
        left = (Node *) ((RelabelType *) left)->arg;

    if (!(IsA(left, Var) && IsA(right, Const)))
        return false;

    opname = get_opname(op->opno);
    if (!opname || strcmp(opname, "=") != 0)
        return false;

    var = (Var *)   left;
    con = (Const *) right;

    pk_attno = get_attnum(rte->relid, pk_col);
    if (var->varattno != pk_attno)
        return false;

    getTypeOutputInfo(con->consttype, &typoutput, &typisvarlena);
    val = OidOutputFunctionCall(typoutput, con->constvalue);

    *out_table  = pstrdup(rel_name);
    *out_pk_col = pstrdup(pk_col);
    *out_pk_val = val;
    return true;
}