/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <assert.h>
#include <apr_optional.h>
#include <apr_strings.h>
#include <apr_date.h>

#include <httpd.h>
#include <http_core.h>
#include <http_protocol.h>
#include <http_request.h>
#include <http_log.h>

#include "mod_watchdog.h"

#include "md.h"
#include "md_curl.h"
#include "md_crypt.h"
#include "md_http.h"
#include "md_json.h"
#include "md_store.h"
#include "md_store_fs.h"
#include "md_log.h"
#include "md_reg.h"
#include "md_util.h"
#include "md_version.h"
#include "md_acme.h"
#include "md_acme_authz.h"

#include "mod_md.h"
#include "mod_md_private.h"
#include "mod_md_config.h"
#include "mod_md_status.h"
#include "mod_md_drive.h"

/**************************************************************************************************/
/* drive job persistence */

static void md_drive_job_from_json(md_drive_job_t *job, const md_json_t *json, apr_pool_t *p)
{
    const char *s;
    /* not good, this is malloced from a temp pool */
    /*job->name = md_json_gets(json, MD_KEY_NAME, NULL);*/
    job->finished = md_json_getb(json, MD_KEY_FINISHED, NULL);
    s = md_json_dups(p, json, MD_KEY_NEXT_RUN, NULL);
    if (s && *s) job->next_run = apr_date_parse_rfc(s);
    s = md_json_dups(p, json, MD_KEY_VALID_FROM, NULL);
    if (s && *s) job->valid_from = apr_date_parse_rfc(s);
    job->notified = md_json_getb(json, MD_KEY_NOTIFIED, NULL);
    job->error_runs = (int)md_json_getl(json, MD_KEY_ERRORS, NULL);
}

static void md_drive_job_to_json(md_json_t *json, const md_drive_job_t *job)
{
    char ts[APR_RFC822_DATE_LEN];

    md_json_sets(job->name, json, MD_KEY_NAME, NULL);
    md_json_setb(job->finished, json, MD_KEY_FINISHED, NULL);
    if (job->next_run > 0) {
        apr_rfc822_date(ts, job->next_run);
        md_json_sets(ts, json, MD_KEY_NEXT_RUN, NULL);
    }
    if (job->valid_from > 0) {
        apr_rfc822_date(ts, job->valid_from);
        md_json_sets(ts, json, MD_KEY_VALID_FROM, NULL);
    }
    md_json_setb(job->notified, json, MD_KEY_NOTIFIED, NULL);
    md_json_setl(job->error_runs, json, MD_KEY_ERRORS, NULL);
}

apr_status_t md_drive_job_load(md_drive_job_t *job, md_reg_t *reg, apr_pool_t *p)
{
    md_store_t *store = md_reg_store_get(reg);
    md_json_t *jprops;
    apr_status_t rv;
    
    rv = md_store_load_json(store, MD_SG_STAGING, job->name, MD_FN_JOB, &jprops, p);
    if (APR_SUCCESS == rv) {
        md_drive_job_from_json(job, jprops, p);
        job->dirty = 0;
    }
    return rv;
}

static apr_status_t md_drive_job_save(md_drive_job_t *job, md_reg_t *reg, apr_pool_t *p)
{
    md_store_t *store = md_reg_store_get(reg);
    md_json_t *jprops;
    apr_status_t rv;
    
    jprops = md_json_create(p);
    md_drive_job_to_json(jprops, job);
    rv = md_store_save_json(store, p, MD_SG_STAGING, job->name, MD_FN_JOB, jprops, 1);
    if (APR_SUCCESS == rv) job->dirty = 0;
    return rv;
}

/**************************************************************************************************/
/* watchdog based impl. */

#define MD_WATCHDOG_NAME   "_md_"

static APR_OPTIONAL_FN_TYPE(ap_watchdog_get_instance) *wd_get_instance;
static APR_OPTIONAL_FN_TYPE(ap_watchdog_register_callback) *wd_register_callback;
static APR_OPTIONAL_FN_TYPE(ap_watchdog_set_callback_interval) *wd_set_interval;

struct md_drive_ctx {
    apr_pool_t *p;
    server_rec *s;
    md_mod_conf_t *mc;
    ap_watchdog_t *watchdog;
    
    apr_array_header_t *jobs;
};

static apr_status_t process_job(md_drive_ctx *dctx, md_drive_job_t *job, apr_pool_t *ptemp)
{
    apr_status_t rv = APR_SUCCESS;
    apr_time_t valid_from, delay, next_run;
    const md_t *md;
    char ts[APR_RFC822_DATE_LEN];

    /* Does this job want to run now? */
    if (apr_time_now() < job->next_run) {
        next_run = job->next_run;
        rv = APR_EAGAIN; goto leave;
    }
    
    md_drive_job_load(job, dctx->mc->reg, ptemp);
    /* Evaluate again on loaded value. */
    if (apr_time_now() < job->next_run) {
        next_run = job->next_run;
        rv = APR_EAGAIN; goto leave;
    }
    
    next_run = 0; /* 0 is default and means at the regular intervals */
    
    md = md_get_by_name(dctx->mc->mds, job->name);
    AP_DEBUG_ASSERT(md);
    if (md->state == MD_S_MISSING) {
        /* Missing information, this will not change until configuration
         * is changed and server reloaded. */
        rv = APR_INCOMPLETE;
        ++job->error_runs;
        job->dirty = 1;
        goto leave;
    }
    
    if (job->finished) {
        if (apr_time_now() < job->valid_from) next_run = job->valid_from;
    }
    else if (md_should_renew(md)) {
        ap_log_error( APLOG_MARK, APLOG_DEBUG, 0, dctx->s, APLOGNO(10052) 
                     "md(%s): state=%d, driving", job->name, md->state);
        
        rv = md_reg_stage(dctx->mc->reg, md, NULL, dctx->mc->env, 0, &valid_from, ptemp);
        job->dirty = 1;
        
        if (APR_SUCCESS == rv) {
            job->finished = 1;
            job->valid_from = valid_from;
            job->error_runs = 0;

            apr_rfc822_date(ts, job->valid_from);
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, dctx->s, APLOGNO(10051) 
                         "%s: has been renewed successfully and should be activated at %s"
                         " (this requires a server restart latest in %s)", 
                         job->name, ts, md_print_duration(ptemp, job->valid_from - apr_time_now()));
        }
        else {
            ap_log_error( APLOG_MARK, APLOG_ERR, rv, dctx->s, APLOGNO(10056) 
                         "processing %s", job->name);
            ++job->error_runs;
            job->dirty = 1;
            /* back off duration, depending on the errors we encounter in a row */
            delay = apr_time_from_sec(5 << (job->error_runs - 1));
            if (delay > apr_time_from_sec(60*60)) {
                delay = apr_time_from_sec(60*60);
            }
            job->next_run = apr_time_now() + delay;
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, dctx->s, APLOGNO(10057) 
                         "%s: encountered error for the %d. time, next run in %s",
                         job->name, job->error_runs, md_print_duration(ptemp, delay));
        }
    }
    else if (md->expires > 0) {
        /* Renew is not necessary yet, leave job->next_run as 0 since 
         * that keeps the default schedule of running twice a day. */
        apr_rfc822_date(ts, md->expires);
        ap_log_error( APLOG_MARK, APLOG_DEBUG, 0, dctx->s, APLOGNO(10053) 
                     "md(%s): no need to renew yet, cert expires %s", job->name, ts);
    }
    
leave:
    if (next_run != job->next_run) {
        job->next_run = next_run;
        job->dirty = 1;
    }
    if (job->dirty) {
        apr_status_t rv2 = md_drive_job_save(job, dctx->mc->reg, ptemp);
        ap_log_error(APLOG_MARK, APLOG_TRACE1, rv2, dctx->s, "%s: saving job props", job->name);
    }
    return rv;
}

static apr_time_t next_run_default()
{
    /* we'd like to run at least twice a day by default - keep an eye on things */
    return apr_time_now() + apr_time_from_sec(MD_SECS_PER_DAY / 2);
}

static void send_notifications(md_drive_ctx *dctx, apr_pool_t *ptemp)
{
    md_drive_job_t *job;
    const char *names = "";
    int i, n;
    apr_time_t now;
    apr_status_t rv;
    
    /* Find jobs that are finished and we have not notified about.
     * 
     * We notify the user about such restart requests, since
     * a) on most setups, we lack the privileges to cause a server reload
     * b) the admin might have her own ideas about when a reload is suitable
     * 
     * Collect the Job names we have not send out notifications about yet.
     */
    n = 0;
    now = apr_time_now();
    for (i = 0; i < dctx->jobs->nelts; ++i) {
        job = APR_ARRAY_IDX(dctx->jobs, i, md_drive_job_t *);
        if (job->finished && !job->notified && now >= job->valid_from) {
            names = apr_psprintf(ptemp, "%s%s%s", names, n? " " : "", job->name);
            ++n;
        }
    }
    if (n <= 0) return;
    
    rv = APR_SUCCESS;
    if (dctx->mc->notify_cmd) {
        const char * const *argv;
        const char *cmdline;
        int exit_code;
        
        cmdline = apr_psprintf(ptemp, "%s %s", dctx->mc->notify_cmd, names); 
        apr_tokenize_to_argv(cmdline, (char***)&argv, ptemp);
        if (APR_SUCCESS == (rv = md_util_exec(ptemp, argv[0], argv, &exit_code))) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, dctx->s, APLOGNO(10108) 
                         "notify command '%s' returned %d", 
                         dctx->mc->notify_cmd, exit_code);
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_ERR, (APR_EINCOMPLETE == rv && exit_code)? 0 : rv, 
                         dctx->s, APLOGNO(10109) 
                         "executing MDNotifyCmd %s returned %d. This is sad, as"
                         " I wanted to tell you that the Manged Domain%s %s"
                         " are ready for a server reload", 
                         dctx->mc->notify_cmd, exit_code, 
                         (n > 1)? "s" : "", names);
        } 
    }
    
    if (APR_SUCCESS == rv) {
        /* mark jobs as notified and persist this. Note, the next run may be
         * in another child process */
        for (i = 0, n = 0; i < dctx->jobs->nelts; ++i) {
            job = APR_ARRAY_IDX(dctx->jobs, i, md_drive_job_t *);
            if (job->finished && !job->notified && now >= job->valid_from) {
                job->notified = 1;
                md_drive_job_save(job, dctx->mc->reg, ptemp);
            }
        }
    }
    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, dctx->s, APLOGNO(10059) 
                 "The Managed Domain%s %s %s been setup and changes "
                 "will be activated on next (graceful) server restart.",
                 (n > 1)? "s" : "", names, (n > 1)? "have" : "has");
}

static apr_status_t run_watchdog(int state, void *baton, apr_pool_t *ptemp)
{
    md_drive_ctx *dctx = baton;
    apr_status_t rv = APR_SUCCESS;
    md_drive_job_t *job;
    apr_time_t next_run, wait_time;
    int i;
    
    /* mod_watchdog invoked us as a single thread inside the whole server (on this machine).
     * This might be a repeated run inside the same child (mod_watchdog keeps affinity as
     * long as the child lives) or another/new child.
     */
    switch (state) {
        case AP_WATCHDOG_STATE_STARTING:
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, dctx->s, APLOGNO(10054)
                         "md watchdog start, auto drive %d mds", dctx->jobs->nelts);
            break;
            
        case AP_WATCHDOG_STATE_RUNNING:
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, dctx->s, APLOGNO(10055)
                         "md watchdog run, auto drive %d mds", dctx->jobs->nelts);
                         
            /* Process all drive jobs. They will update their next_run property
             * and we schedule ourself at the earliest of all. A job may specify 0
             * as next_run to indicate that it wants to participate in the normal
             * regular runs. */
            next_run = next_run_default();
            for (i = 0; i < dctx->jobs->nelts; ++i) {
                job = APR_ARRAY_IDX(dctx->jobs, i, md_drive_job_t *);
                
                rv = process_job(dctx, job, ptemp);
                
                if (job->next_run && job->next_run < next_run) {
                    next_run = job->next_run;
                }
            }

            wait_time = next_run - apr_time_now();
            if (APLOGdebug(dctx->s)) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, dctx->s, APLOGNO(10107)
                             "next run in %s", md_print_duration(ptemp, wait_time));
            }
            wd_set_interval(dctx->watchdog, wait_time, dctx, run_watchdog);
            break;
            
        case AP_WATCHDOG_STATE_STOPPING:
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, dctx->s, APLOGNO(10058)
                         "md watchdog stopping");
            break;
    }
    send_notifications(dctx, ptemp);
    
    return APR_SUCCESS;
}

apr_status_t md_start_driving(md_mod_conf_t *mc, server_rec *s, apr_pool_t *p)
{
    apr_allocator_t *allocator;
    md_drive_ctx *dctx;
    apr_pool_t *dctxp;
    apr_status_t rv;
    const char *name;
    md_t *md;
    md_drive_job_t *job;
    int i;
    
    /* We use mod_watchdog to run a single thread in one of the child processes
     * to monitor the MDs in mc->drive_names, using the const data in the list
     * mc->mds of our MD structures.
     *
     * The data in mc cannot be changed, as we may spawn copies in new child processes
     * of the original data at any time. The child which hosts the watchdog thread
     * may also die or be recycled, which causes a new watchdog thread to run
     * in another process with the original data.
     * 
     * Instead, we use our store to persist changes in group STAGING. This is
     * kept writable to child processes, but the data stored there is not live.
     * However, mod_watchdog makes sure that we only ever have a single thread in
     * our server (on this machine) that writes there. Other processes, e.g. informing
     * the user about progress, only read from there.
     *
     * All changes during driving an MD are stored as files in MG_SG_STAGING/<MD.name>.
     * All will have "md.json" and "job.json". There may be a range of other files used
     * by the protocol obtaining the certificate/keys.
     * 
     * 
     */
    wd_get_instance = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_get_instance);
    wd_register_callback = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_register_callback);
    wd_set_interval = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_set_callback_interval);
    
    if (!wd_get_instance || !wd_register_callback || !wd_set_interval) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, APLOGNO(10061) "mod_watchdog is required");
        return !OK;
    }
    
    /* We want our own pool with own allocator to keep data across watchdog invocations.
     * Since we'll run in a single watchdog thread, using our own allocator will prevent 
     * any confusion in the parent pool. */
    apr_allocator_create(&allocator);
    apr_allocator_max_free_set(allocator, 1);
    rv = apr_pool_create_ex(&dctxp, p, NULL, allocator);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s, APLOGNO(10062) "md_drive_ctx: create pool");
        return rv;
    }
    apr_allocator_owner_set(allocator, dctxp);
    apr_pool_tag(dctxp, "md_drive_ctx");

    dctx = apr_pcalloc(dctxp, sizeof(*dctx));
    dctx->p = dctxp;
    dctx->s = s;
    dctx->mc = mc;
    
    dctx->jobs = apr_array_make(dctx->p, mc->drive_names->nelts, sizeof(md_drive_job_t *));
    for (i = 0; i < mc->drive_names->nelts; ++i) {
        name = APR_ARRAY_IDX(mc->drive_names, i, const char *);
        md = md_get_by_name(mc->mds, name);
        if (md) {
            if (md->state == MD_S_ERROR) {
                ap_log_error( APLOG_MARK, APLOG_WARNING, 0, dctx->s, APLOGNO() 
                             "md(%s): in error state, unable to drive forward. This "
                             "indicates an incomplete or inconsistent configuration. "
                             "Please check the log for warnings in this regard.", md->name);
            }
            else {
                if (md->state == MD_S_COMPLETE && !md->expires) {
                    ap_log_error( APLOG_MARK, APLOG_WARNING, 0, dctx->s, APLOGNO() 
                                 "md(%s): is complete but has no expiration date. This "
                                 "means it will never be renewed and should not happen.", md->name);
                }
                
                job = apr_pcalloc(dctx->p, sizeof(*job));
                job->name = md->name;
                APR_ARRAY_PUSH(dctx->jobs, md_drive_job_t*) = job;
                ap_log_error( APLOG_MARK, APLOG_TRACE1, 0, dctx->s,  
                             "md(%s): state=%d, created drive job", name, md->state);
                
                md_drive_job_load(job, mc->reg, dctx->p);
                if (job->error_runs) {
                    /* Server has just restarted. If we encounter an MD job with errors
                     * on a previous driving, we purge its STAGING area.
                     * This will reset the driving for the MD. It may run into the same
                     * error again, or in case of race/confusion/our error/CA error, it
                     * might allow the MD to succeed by a fresh start.
                     */
                    ap_log_error( APLOG_MARK, APLOG_INFO, 0, dctx->s, APLOGNO(10064) 
                                 "md(%s): previous drive job showed %d errors, purging STAGING "
                                 "area to reset.", name, job->error_runs);
                    md_store_purge(md_reg_store_get(dctx->mc->reg), p, MD_SG_STAGING, md->name);
                    md_store_purge(md_reg_store_get(dctx->mc->reg), p, MD_SG_CHALLENGES, md->name);
                    job->error_runs = 0;
                }
            }
        }
    }

    if (!dctx->jobs->nelts) {
        ap_log_error( APLOG_MARK, APLOG_DEBUG, 0, s, APLOGNO(10065)
                     "no managed domain in state to drive, no watchdog needed, "
                     "will check again on next server (graceful) restart");
        apr_pool_destroy(dctx->p);
        return APR_SUCCESS;
    }
    
    if (APR_SUCCESS != (rv = wd_get_instance(&dctx->watchdog, MD_WATCHDOG_NAME, 0, 1, dctx->p))) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s, APLOGNO(10066) 
                     "create md watchdog(%s)", MD_WATCHDOG_NAME);
        return rv;
    }
    rv = wd_register_callback(dctx->watchdog, 0, dctx, run_watchdog);
    ap_log_error(APLOG_MARK, rv? APLOG_CRIT : APLOG_DEBUG, rv, s, APLOGNO(10067) 
                 "register md watchdog(%s)", MD_WATCHDOG_NAME);
    return rv;
}