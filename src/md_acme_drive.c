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
#include <stdlib.h>

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_buckets.h>
#include <apr_hash.h>
#include <apr_uri.h>

#include "md.h"
#include "md_crypt.h"
#include "md_json.h"
#include "md_jws.h"
#include "md_http.h"
#include "md_log.h"
#include "md_reg.h"
#include "md_store.h"
#include "md_util.h"

#include "md_acme.h"
#include "md_acme_acct.h"
#include "md_acme_authz.h"
#include "md_acme_order.h"

#include "md_acme_drive.h"
#include "md_acmev1_drive.h"
#include "md_acmev2_drive.h"

/**************************************************************************************************/
/* account setup */

static apr_status_t use_staged_acct(md_acme_t *acme, struct md_store_t *store, 
                                    const char *md_name, apr_pool_t *p)
{
    md_acme_acct_t *acct;
    md_pkey_t *pkey;
    apr_status_t rv;
    
    if (APR_SUCCESS == (rv = md_acme_acct_load(&acct, &pkey, store, 
                                               MD_SG_STAGING, md_name, acme->p))) {
        acme->acct_id = NULL;
        acme->acct = acct;
        acme->acct_key = pkey;
        rv = md_acme_acct_validate(acme, NULL, p);
    }
    return rv;
}

static apr_status_t save_acct_staged(md_acme_t *acme, md_store_t *store, 
                                     const char *md_name, apr_pool_t *p)
{
    md_json_t *jacct;
    apr_status_t rv;
    
    jacct = md_acme_acct_to_json(acme->acct, p);
    
    rv = md_store_save(store, p, MD_SG_STAGING, md_name, MD_FN_ACCOUNT, MD_SV_JSON, jacct, 0);
    if (APR_SUCCESS == rv) {
        rv = md_store_save(store, p, MD_SG_STAGING, md_name, MD_FN_ACCT_KEY, 
                           MD_SV_PKEY, acme->acct_key, 0);
    }
    return rv;
}

apr_status_t md_acme_drive_set_acct(md_proto_driver_t *d) 
{
    md_acme_driver_t *ad = d->baton;
    md_t *md = ad->md;
    apr_status_t rv = APR_SUCCESS;
    int update_md = 0, update_acct = 0;
    
    ad->phase = "choose account";
    md_acme_clear_acct(ad->acme);
    
    /* Do we have a staged (modified) account? */
    if (APR_SUCCESS == (rv = use_staged_acct(ad->acme, d->store, md->name, d->p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "re-using staged account");
    }
    else if (!APR_STATUS_IS_ENOENT(rv)) {
        goto out;
    }
    
    /* Get an account for the ACME server for this MD */
    if (!ad->acme->acct && md->ca_account) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "re-use account '%s'", md->ca_account);
        rv = md_acme_use_acct(ad->acme, d->store, d->p, md->ca_account);
        if (APR_STATUS_IS_ENOENT(rv) || APR_STATUS_IS_EINVAL(rv)) {
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "rejected %s", md->ca_account);
            md->ca_account = NULL;
            update_md = 1;
        }
        else if (APR_SUCCESS != rv) {
            goto out;
        }
    }

    if (!ad->acme->acct && !md->ca_account) {
        /* Find a local account for server, store at MD */ 
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: looking at existing accounts",
                      d->proto->protocol);
        if (APR_SUCCESS == (rv = md_acme_find_acct(ad->acme, d->store))) {
            md->ca_account = md_acme_acct_id_get(ad->acme);
            update_md = 1;
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: using account %s (id=%s)",
                          d->proto->protocol, ad->acme->acct->url, md->ca_account);
        }
    }
    
    if (!ad->acme->acct) {
        /* No account staged, no suitable found in store, register a new one */
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: creating new account", 
                      d->proto->protocol);
        
        if (!ad->md->contacts || apr_is_empty_array(md->contacts)) {
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, APR_EINVAL, d->p, 
                          "no contact information for md %s", md->name);            
            rv = APR_EINVAL;
            goto out;
        }
        
        /* ACMEv1 allowed registration of accounts without accepted Terms-of-Service.
         * ACMEv2 requires it. Fail early in this case with a meaningful error message.
         */ 
        if (!md->ca_agreement && MD_ACME_VERSION_MAJOR(ad->acme->version) > 1) {
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, 
                          "%s: the CA requires you to accept the terms-of-service "
                          "as specified in <%s>. "
                          "Please read the document that you find at that URL and, "
                          "if you agree to the conditions, configure "
                          "\"MDCertificateAgreement accepted\" "
                          "in your Apache. Then (graceful) restart the server to activate.", 
                          md->name, ad->acme->ca_agreement);
            rv = APR_EINVAL;
            goto out;
        }
    
        rv = md_acme_acct_register(ad->acme, d->store, d->p, md->contacts, md->ca_agreement);
        if (APR_SUCCESS == rv) {
            md->ca_account = NULL;
            update_md = 1;
            update_acct = 1;
        }
    }
    
out:
    /* Persist MD changes in STAGING, so we pick them up on next run */
    if (APR_SUCCESS == rv&& update_md) {
        rv = md_save(d->store, d->p, MD_SG_STAGING, ad->md, 0);
    }
    /* Persist account changes in STAGING, so we pick them up on next run */
    if (APR_SUCCESS == rv&& update_acct) {
        rv = save_acct_staged(ad->acme, d->store, md->name, d->p);
    }
    return rv;
}

/**************************************************************************************************/
/* poll cert */

static void get_up_link(md_proto_driver_t *d, apr_table_t *headers)
{
    md_acme_driver_t *ad = d->baton;

    ad->next_up_link = md_link_find_relation(headers, d->p, "up");
    if (ad->next_up_link) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, d->p, 
                      "server reports up link as %s", ad->next_up_link);
    }
} 

static apr_status_t add_http_certs(apr_array_header_t *chain, apr_pool_t *p,
                                   const md_http_response_t *res)
{
    apr_status_t rv = APR_SUCCESS;
    const char *ct;
    
    ct = apr_table_get(res->headers, "Content-Type");
    if (ct && !strcmp("application/x-pkcs7-mime", ct)) {
        /* this looks like a root cert and we do not want those in our chain */
        goto out; 
    }

    /* Lets try to read one or more certificates */
    if (APR_SUCCESS != (rv = md_cert_chain_read_http(chain, p, res))
        && APR_STATUS_IS_ENOENT(rv)) {
        rv = APR_EAGAIN;
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, p, 
                      "cert not in response from %s", res->req->url);
    }
out:
    return rv;
}

static apr_status_t on_add_cert(md_acme_t *acme, const md_http_response_t *res, void *baton)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv = APR_SUCCESS;
    int count;
    
    (void)acme;
    count = ad->certs->nelts;
    if (APR_SUCCESS == (rv = add_http_certs(ad->certs, d->p, res))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%d certs parsed", 
                      ad->certs->nelts - count);
        get_up_link(d, res->headers);
    }
    return rv;
}

static apr_status_t get_cert(void *baton, int attempt)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    
    (void)attempt;
    md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, 0, d->p, "retrieving cert from %s",
                  ad->order->certificate);
    return md_acme_GET(ad->acme, ad->order->certificate, NULL, NULL, on_add_cert, d);
}

apr_status_t md_acme_drive_cert_poll(md_proto_driver_t *d, int only_once)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;
    
    assert(ad->md);
    assert(ad->acme);
    assert(ad->order);
    assert(ad->order->certificate);
    
    ad->phase = "poll certificate";
    if (only_once) {
        rv = get_cert(d, 0);
    }
    else {
        rv = md_util_try(get_cert, d, 1, ad->cert_poll_timeout, 0, 0, 1);
    }
    
    md_log_perror(MD_LOG_MARK, MD_LOG_INFO, 0, d->p, "poll for cert at %s", ad->order->certificate);
    return rv;
}

/**************************************************************************************************/
/* order finalization */

static apr_status_t on_init_csr_req(md_acme_req_t *req, void *baton)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    md_json_t *jpayload;

    jpayload = md_json_create(req->p);
    if (MD_ACME_VERSION_MAJOR(req->acme->version) == 1) {
        md_json_sets("new-cert", jpayload, MD_KEY_RESOURCE, NULL);
    }
    md_json_sets(ad->csr_der_64, jpayload, MD_KEY_CSR, NULL);
    
    return md_acme_req_body_init(req, jpayload);
} 

static apr_status_t csr_req(md_acme_t *acme, const md_http_response_t *res, void *baton)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    const char *location;
    md_cert_t *cert;
    apr_status_t rv = APR_SUCCESS;
    
    (void)acme;
    location = apr_table_get(res->headers, "location");
    if (!location) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, APR_EINVAL, d->p, 
                      "cert created without giving its location header");
        return APR_EINVAL;
    }
    ad->order->certificate = apr_pstrdup(d->p, location);
    if (APR_SUCCESS != (rv = md_acme_order_save(d->store, d->p, MD_SG_STAGING, 
                                                ad->md->name, ad->order, 0))) { 
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, APR_EINVAL, d->p, 
                      "%s: saving cert url %s", ad->md->name, location);
        return rv;
    }
    
    /* Check if it already was sent with this response */
    ad->next_up_link = NULL;
    if (APR_SUCCESS == (rv = md_cert_read_http(&cert, d->p, res))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "cert parsed");
        if (ad->certs) {
            apr_array_clear(ad->certs);
        }
        else {
            ad->certs = apr_array_make(d->p, 5, sizeof(md_cert_t*));
        }
        APR_ARRAY_PUSH(ad->certs, md_cert_t*) = cert;
        
        if (APR_SUCCESS == rv) {
            get_up_link(d, res->headers);
        }
    }
    else if (APR_STATUS_IS_ENOENT(rv)) {
        rv = APR_SUCCESS;
        if (location) {
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, 
                          "cert not in response, need to poll %s", location);
        }
    }
    
    return rv;
}

/**
 * Pre-Req: all domains have been validated by the ACME server, e.g. all have AUTHZ
 * resources that have status 'valid'
 * - Setup private key, if not already there
 * - Generate a CSR with org, contact, etc
 * - Optionally enable must-staple OCSP extension
 * - Submit CSR, expect 201 with location
 * - POLL location for certificate
 * - store certificate
 * - retrieve cert chain information from cert
 * - GET cert chain
 * - store cert chain
 */
apr_status_t md_acme_drive_setup_certificate(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    md_pkey_t *privkey;
    apr_status_t rv;

    ad->phase = "setup cert privkey";
    
    rv = md_pkey_load(d->store, MD_SG_STAGING, ad->md->name, &privkey, d->p);
    if (APR_STATUS_IS_ENOENT(rv)) {
        if (APR_SUCCESS == (rv = md_pkey_gen(&privkey, d->p, d->md->pkey_spec))) {
            rv = md_pkey_save(d->store, d->p, MD_SG_STAGING, ad->md->name, privkey, 1);
        }
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: generate privkey", ad->md->name);
    }
    if (APR_SUCCESS != rv) goto out;
    
    ad->phase = "setup csr";
    rv = md_cert_req_create(&ad->csr_der_64, ad->md->name, ad->domains, 
                            ad->md->must_staple, privkey, d->p);
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: create CSR", ad->md->name);
    if (APR_SUCCESS != rv) goto out;

    ad->phase = "submit csr";
    switch (MD_ACME_VERSION_MAJOR(ad->acme->version)) {
        case 1:
            rv = md_acme_POST(ad->acme, ad->acme->api.v1.new_cert, on_init_csr_req, NULL, csr_req, d);
            break;
        default:
            assert(ad->order->finalize);
            rv = md_acme_POST(ad->acme, ad->order->finalize, on_init_csr_req, NULL, csr_req, d);
            break;
    }
    if (APR_SUCCESS != rv) goto out;

out:
    return rv;
}

/**************************************************************************************************/
/* cert chain retrieval */

static apr_status_t on_add_chain(md_acme_t *acme, const md_http_response_t *res, void *baton)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv = APR_SUCCESS;
    const char *ct;
    
    (void)acme;
    ct = apr_table_get(res->headers, "Content-Type");
    if (ct && !strcmp("application/x-pkcs7-mime", ct)) {
        /* root cert most likely, end it here */
        return APR_SUCCESS;
    }
    
    if (APR_SUCCESS == (rv = add_http_certs(ad->certs, d->p, res))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "chain cert parsed");
        get_up_link(d, res->headers);
    }
    return rv;
}

static apr_status_t get_chain(void *baton, int attempt)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    const char *prev_link = NULL;
    apr_status_t rv = APR_SUCCESS;

    while (APR_SUCCESS == rv && ad->certs->nelts < 10) {
        int nelts = ad->certs->nelts;
        
        if (ad->next_up_link && (!prev_link || strcmp(prev_link, ad->next_up_link))) {
            prev_link = ad->next_up_link;

            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, 
                          "next chain cert at  %s", ad->next_up_link);
            rv = md_acme_GET(ad->acme, ad->next_up_link, NULL, NULL, on_add_chain, d);
            
            if (APR_SUCCESS == rv && nelts == ad->certs->nelts) {
                break;
            }
        }
        else if (ad->certs->nelts <= 1) {
            /* This cannot be the complete chain (no one signs new web certs with their root)
             * and we did not see a "Link: ...rel=up", so we do not know how to continue. */
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, 
                          "no link header 'up' for new certificate, unable to retrieve chain");
            rv = APR_EINVAL;
            break;
        }
        else {
            rv = APR_SUCCESS;
            break;
        }
    }
    md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, rv, d->p, 
                  "got chain with %d certs (%d. attempt)", ad->certs->nelts, attempt);
    return rv;
}

static apr_status_t ad_chain_retrieve(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;
    
    /* This may be called repeatedly and needs to progress. The relevant state is in
     * ad->certs                the certificate chain, starting with the new cert for the md
     * ad->order->certificate   the url where ACME offers us the new md certificate. This may
     *                          be a single one or even the complete chain
     * ad->next_up_link         in case the last certificate retrieval did not end the chain,
     *                          the link header with relation "up" gives us the location
     *                          for the next cert in the chain
     */
    if (!ad->certs) {
        ad->certs = apr_array_make(d->p, 5, sizeof(md_cert_t *));
    }
    if (md_array_is_empty(ad->certs)) {
        /* Need to start at the order */
        ad->next_up_link = NULL;
        if (!ad->order) {
            rv = APR_EGENERAL;
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, 
                "%s: asked to retrieve chain, but no order in context", d->md->name);
            goto out;
        }
        if (!ad->order->certificate) {
            rv = APR_EGENERAL;
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, 
                "%s: asked to retrieve chain, but no certificate url part of order", d->md->name);
            goto out;
        }
        
        if (APR_SUCCESS != (rv = md_acme_drive_cert_poll(d, 0))) {
            goto out;
        }
    }
    
    rv = md_util_try(get_chain, d, 0, ad->cert_poll_timeout, 0, 0, 0);
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "chain retrieved");
    
out:
    return rv;
}

/**************************************************************************************************/
/* ACME driver init */

static apr_status_t acme_driver_init(md_proto_driver_t *d)
{
    md_acme_driver_t *ad;
    apr_status_t rv = APR_SUCCESS;
    int configured_count;
    const char *challenge;
    
    ad = apr_pcalloc(d->p, sizeof(*ad));
    
    d->baton = ad;
    ad->driver = d;
    
    ad->authz_monitor_timeout = apr_time_from_sec(30);
    ad->cert_poll_timeout = apr_time_from_sec(30);
    
    /* We can only support challenges if the server is reachable from the outside
     * via port 80 and/or 443. These ports might be mapped for httpd to something
     * else, but a mapping needs to exist. */
    ad->ca_challenges = apr_array_make(d->p, 3, sizeof(const char *));
    challenge = apr_table_get(d->env, MD_KEY_CHALLENGE); 
    if (challenge) {
        APR_ARRAY_PUSH(ad->ca_challenges, const char*) = apr_pstrdup(d->p, challenge);
    }
    else if (d->md->ca_challenges && d->md->ca_challenges->nelts > 0) {
        /* pre-configured set for this managed domain */
        apr_array_cat(ad->ca_challenges, d->md->ca_challenges);
    }
    else {
        /* free to chose. Add all we support and see what we get offered */
        APR_ARRAY_PUSH(ad->ca_challenges, const char*) = MD_AUTHZ_TYPE_HTTP01;
        APR_ARRAY_PUSH(ad->ca_challenges, const char*) = MD_AUTHZ_TYPE_TLSALPN01;
        if (apr_table_get(d->env, MD_KEY_CMD_DNS01)) {
            APR_ARRAY_PUSH(ad->ca_challenges, const char*) = MD_AUTHZ_TYPE_DNS01;
        }
    }
    
    configured_count = ad->ca_challenges->nelts;
    if (!d->can_http && !d->can_https 
        && md_array_str_index(ad->ca_challenges, MD_AUTHZ_TYPE_DNS01, 0, 0) < 0) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, d->p, "%s: the server seems neither "
                      "reachable via http (port 80) nor https (port 443). The ACME protocol "
                      "needs at least one of those so the CA can talk to the server and verify "
                      "a domain ownership. Alternatively, you may configure support "
                      "for the %s challenge method, if your CA supports it.", 
                      d->md->name, MD_AUTHZ_TYPE_DNS01);
        return APR_EGENERAL;
    }
    
    if (!d->can_http) {
        ad->ca_challenges = md_array_str_remove(d->p, ad->ca_challenges, MD_AUTHZ_TYPE_HTTP01, 0);
    }
    if (!d->can_https) {
        ad->ca_challenges = md_array_str_remove(d->p, ad->ca_challenges, MD_AUTHZ_TYPE_TLSALPN01, 0);
    }
    if (!d->md->can_acme_tls_1) {
        ad->ca_challenges = md_array_str_remove(d->p, ad->ca_challenges, MD_AUTHZ_TYPE_TLSALPN01, 0);
    }

    if (apr_is_empty_array(ad->ca_challenges)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, d->p, "%s: from the %d CA challenge methods "
                      "configured for this domain, none are suitable. There are preconditions "
                      "that must be met, for example: "
                      "'http-01' needs a server reachable on port 80, 'tls-alpn-01'"
                      " needs port 443%s. 'dns-01' needs a MDChallengeDns01 command to be "
                      "configured. Please consult the documentation for details.", 
                      d->md->name, configured_count, (d->md->can_acme_tls_1? "" :
                      " and the protocol 'acme-tls/1' allowed on this server"));
        return APR_EGENERAL;
    }
    
    md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, 0, d->p, "%s: init driver", d->md->name);
    
    return rv;
}

/**************************************************************************************************/
/* ACME staging */

static apr_status_t acme_renew(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    int reset_staging = d->reset;
    apr_status_t rv = APR_SUCCESS;
    apr_time_t now;
    apr_interval_time_t max_delay, delay_activation; 

    if (md_log_is_level(d->p, MD_LOG_DEBUG)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, d->p, "%s: staging started, "
                      "state=%d, can_http=%d, can_https=%d, challenges='%s'",
                      d->md->name, d->md->state, d->can_http, d->can_https,
                      apr_array_pstrcat(d->p, ad->ca_challenges, ' '));
    }

    /* When not explicitly told to reset, we check the existing data. If
     * it is incomplete or old, we trigger the reset for a clean start. */
    if (!reset_staging) {
        rv = md_load(d->store, MD_SG_STAGING, d->md->name, &ad->md, d->p);
        if (APR_SUCCESS == rv) {
            /* So, we have a copy in staging, but is it a recent or an old one? */
            if (md_is_newer(d->store, MD_SG_DOMAINS, MD_SG_STAGING, d->md->name, d->p)) {
                reset_staging = 1;
            }
        }
        else if (APR_STATUS_IS_ENOENT(rv)) {
            reset_staging = 1;
            rv = APR_SUCCESS;
        }
    }
    
    if (reset_staging) {
        /* reset the staging area for this domain */
        rv = md_store_purge(d->store, d->p, MD_SG_STAGING, d->md->name);
        md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, rv, d->p, 
                      "%s: reset staging area, will", d->md->name);
        if (APR_SUCCESS != rv && !APR_STATUS_IS_ENOENT(rv)) {
            return rv;
        }
        rv = APR_SUCCESS;
        ad->md = NULL;
        ad->order = NULL;
    }
    
    if (ad->md && ad->md->state == MD_S_MISSING) {
        /* There is config information missing. It makes no sense to drive this MD further */
        rv = APR_INCOMPLETE;
        goto out;
    }
    
    if (ad->md) {
        /* staging in progress. look for new ACME account information collected there */
        rv = md_reg_creds_get(&ad->ncreds, d->reg, MD_SG_STAGING, d->md, d->p);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: checked creds", d->md->name);
        if (APR_STATUS_IS_ENOENT(rv)) {
            rv = APR_SUCCESS;
        }
    }
    
    /* Find out where we're at with this managed domain */
    if (ad->ncreds && ad->ncreds->privkey && ad->ncreds->pubcert) {
        /* There is a full set staged, to be loaded */
        md_log_perror(MD_LOG_MARK, MD_LOG_INFO, 0, d->p, "%s: all data staged", d->md->name);
        goto out;
    }

    /* Need to renew */
    if (APR_SUCCESS != (rv = md_acme_create(&ad->acme, d->p, d->md->ca_url, d->proxy_url)) 
        || APR_SUCCESS != (rv = md_acme_setup(ad->acme))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "%s: setup ACME(%s)", 
                      d->md->name, d->md->ca_url);
        goto out;
    }
    
    if (!ad->md || strcmp(ad->md->ca_url, d->md->ca_url)) {
        /* re-initialize staging */
        md_log_perror(MD_LOG_MARK, MD_LOG_INFO, 0, d->p, "%s: setup staging", d->md->name);
        md_store_purge(d->store, d->p, MD_SG_STAGING, d->md->name);
        ad->md = md_copy(d->p, d->md);
        ad->order = NULL;
        rv = md_save(d->store, d->p, MD_SG_STAGING, ad->md, 0);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: save staged md", 
                      ad->md->name);
        if (APR_SUCCESS != rv) goto out;
    }
    if (!ad->domains) {
        ad->domains = md_dns_make_minimal(d->p, ad->md->domains);
    }
    if (md_array_is_empty(ad->certs)) {
        /* have we created this already? */
        md_pubcert_load(d->store, MD_SG_STAGING, ad->md->name, &ad->certs, d->p);
    }
    
    if (md_array_is_empty(ad->certs)) {
        /* The process of setting up challenges and verifying domain
         * names differs between ACME versions. */
        switch (MD_ACME_VERSION_MAJOR(ad->acme->version)) {
                case 1:
                rv = md_acmev1_drive_renew(ad, d);
                break;
                case 2:
                rv = md_acmev2_drive_renew(ad, d);
                break;
            default:
                rv = APR_EINVAL;
                break;
        }
        if (APR_SUCCESS != rv) goto out;
    }
    
    if (md_array_is_empty(ad->certs) || ad->next_up_link) {
        ad->phase = "retrieve certificate chain";
        md_log_perror(MD_LOG_MARK, MD_LOG_INFO, 0, d->p, 
                      "%s: retrieving certificate chain", d->md->name);
        rv = ad_chain_retrieve(d);
        
        if (APR_SUCCESS == rv && !md_array_is_empty(ad->certs)) {
            rv = md_pubcert_save(d->store, d->p, MD_SG_STAGING, ad->md->name, ad->certs, 0);
        }
        if (APR_SUCCESS != rv) goto out;
    }
    
    /* we should have the complete cert chain now */
    assert(!md_array_is_empty(ad->certs));
    assert(ad->certs->nelts > 1);
    
    /* determine when this cert should be activated */
    now = apr_time_now();
    d->renew_valid_from = md_cert_get_not_before(APR_ARRAY_IDX(ad->certs, 0, md_cert_t*));
    if (d->md->state == MD_S_COMPLETE && d->md->expires > now) {            
        /* The MD is complete and un-expired. This is a renewal run. 
         * Give activation 24 hours leeway (if we have that time) to
         * accommodate for clients with somewhat weird clocks.
         */
        delay_activation = apr_time_from_sec(MD_SECS_PER_DAY);
        if (delay_activation > (max_delay = d->md->expires - now)) {
            delay_activation = max_delay;
        }
        d->renew_valid_from += delay_activation;
    }

    /* As last step, cleanup any order we created so that challenge data
     * may be removed asap. */
    md_acme_order_purge(d->store, d->p, MD_SG_STAGING, d->md->name, d->env);

out:    
    return rv;
}

static apr_status_t acme_driver_renew(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;

    ad->phase = "ACME staging";
    if (APR_SUCCESS == (rv = acme_renew(d))) {
        ad->phase = "staging done";
    }
        
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: %s, %s", 
                  d->md->name, d->proto->protocol, ad->phase);
    return rv;
}

/**************************************************************************************************/
/* ACME preload */

static apr_status_t acme_preload(md_proto_driver_t *d, md_store_group_t load_group, 
                                 const char *name) 
{
    apr_status_t rv;
    md_pkey_t *privkey, *acct_key;
    md_t *md;
    apr_array_header_t *pubcert;
    struct md_acme_acct_t *acct;

    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, d->p, "%s: preload start", name);
    /* Load data from MD_SG_STAGING and save it into "load_group".
     * This serves several purposes:
     *  1. It's a format check on the input data. 
     *  2. We write back what we read, creating data with our own access permissions
     *  3. We ignore any other accumulated data in STAGING
     *  4. Once "load_group" is complete an ok, we can swap/archive groups with a rename
     *  5. Reading/Writing the data will apply/remove any group specific data encryption.
     */
    if (APR_SUCCESS != (rv = md_load(d->store, MD_SG_STAGING, name, &md, d->p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: loading md json", name);
        return rv;
    }
    if (APR_SUCCESS != (rv = md_pkey_load(d->store, MD_SG_STAGING, name, &privkey, d->p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: loading staging private key", name);
        return rv;
    }
    if (APR_SUCCESS != (rv = md_pubcert_load(d->store, MD_SG_STAGING, name, &pubcert, d->p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: loading pubcert", name);
        return rv;
    }

    /* See if staging holds a new or modified account data */
    rv = md_acme_acct_load(&acct, &acct_key, d->store, MD_SG_STAGING, name, d->p);
    if (APR_STATUS_IS_ENOENT(rv)) {
        acct = NULL;
        acct_key = NULL;
        rv = APR_SUCCESS;
    }
    else if (APR_SUCCESS != rv) {
        return rv; 
    }

    /* Remove any authz information we have here or in MD_SG_CHALLENGES */
    md_acme_order_purge(d->store, d->p, MD_SG_STAGING, name, d->env);

    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, 
                  "%s: staged data load, purging tmp space", name);
    rv = md_store_purge(d->store, d->p, load_group, name);
    if (APR_SUCCESS != rv) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "%s: error purging preload storage", name);
        return rv;
    }
    
    if (acct) {
        md_acme_t *acme;
        const char *id = md->ca_account;

        /* We may have STAGED the same account several times. This happens when
         * several MDs are renewed at once and need a new account. They will all store
         * the new account in their own STAGING area. By checking for accounts with
         * the same url, we save them all into a single one.
         */
        if (!id && acct->url) {
            rv = md_acme_acct_id_for_url(&id, d->store, MD_SG_ACCOUNTS, acct->url, d->p);
            if (APR_STATUS_IS_ENOENT(rv)) {
                id = NULL;
            }
            else if (APR_SUCCESS != rv) {
                md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, 
                              "%s: error looking up existing account by url", name);
                return rv;
            }
        }
        
        if (APR_SUCCESS != (rv = md_acme_create(&acme, d->p, md->ca_url, d->proxy_url))) {
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "%s: error creating acme", name);
            return rv;
        }
        
        if (APR_SUCCESS != (rv = md_acme_acct_save(d->store, d->p, acme, &id, acct, acct_key))) {
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "%s: error saving acct", name);
            return rv;
        }
        md->ca_account = id;
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: saved ACME account %s", 
                      name, id);
    }
    
    if (APR_SUCCESS != (rv = md_save(d->store, d->p, load_group, md, 1))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "%s: saving md json", name);
        return rv;
    }
    if (APR_SUCCESS != (rv = md_pubcert_save(d->store, d->p, load_group, name, pubcert, 1))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "%s: saving cert chain", name);
        return rv;
    }
    if (APR_SUCCESS != (rv = md_pkey_save(d->store, d->p, load_group, name, privkey, 1))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "%s: saving private key", name);
        return rv;
    }
    
    return rv;
}

static apr_status_t acme_driver_preload(md_proto_driver_t *d, md_store_group_t group)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;

    ad->phase = "ACME preload";
    if (APR_SUCCESS == (rv = acme_preload(d, group, d->md->name))) {
        ad->phase = "preload done";
    }
        
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: %s, %s", 
                  d->md->name, d->proto->protocol, ad->phase);
    return rv;
}

static md_proto_t ACME_PROTO = {
    MD_PROTO_ACME, acme_driver_init, acme_driver_renew, acme_driver_preload
};
 
apr_status_t md_acme_protos_add(apr_hash_t *protos, apr_pool_t *p)
{
    (void)p;
    apr_hash_set(protos, MD_PROTO_ACME, sizeof(MD_PROTO_ACME)-1, &ACME_PROTO);
    return APR_SUCCESS;
}
