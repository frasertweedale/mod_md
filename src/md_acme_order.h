/* Copyright 2019 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef md_acme_order_h
#define md_acme_order_h

struct md_json_t;

typedef struct md_acme_order_t md_acme_order_t;

struct md_acme_order_t {
    apr_pool_t *p;
    const char *url;
    struct md_json_t *json;
    struct apr_array_header_t *authz_urls;
    struct apr_array_header_t *challenge_dirs;
};

#define MD_FN_ORDER             "order.json"

/**************************************************************************************************/

md_acme_order_t *md_acme_order_create(apr_pool_t *p);

apr_status_t md_acme_order_add(md_acme_order_t *order, const char *authz_url);
apr_status_t md_acme_order_remove(md_acme_order_t *order, const char *authz_url);

apr_status_t md_acme_order_add_challenge_dir(md_acme_order_t *order, const char *dir);


struct md_json_t *md_acme_order_to_json(md_acme_order_t *set, apr_pool_t *p);
md_acme_order_t *md_acme_order_from_json(struct md_json_t *json, apr_pool_t *p);

apr_status_t md_acme_order_load(struct md_store_t *store, md_store_group_t group, 
                                    const char *md_name, md_acme_order_t **pauthz_set, 
                                    apr_pool_t *p);
apr_status_t md_acme_order_save(struct md_store_t *store, apr_pool_t *p, 
                                    md_store_group_t group, const char *md_name, 
                                    md_acme_order_t *authz_set, int create);

apr_status_t md_acme_order_purge(struct md_store_t *store, apr_pool_t *p, 
                                 md_store_group_t group, const char *md_name);


apr_status_t md_acme_order_start_challenges(md_acme_order_t *order, md_acme_t *acme, 
                                            apr_array_header_t *challenge_types,
                                            md_store_t *store, const md_t *md, apr_pool_t *p);

apr_status_t md_acme_order_monitor_authzs(md_acme_order_t *order, md_acme_t *acme, 
                                          const md_t *md, apr_interval_time_t timeout, 
                                          apr_pool_t *p);

#endif /* md_acme_order_h */