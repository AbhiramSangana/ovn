/* Copyright (c) 2022 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include <unistd.h>

/* library headers */
#include "lib/sset.h"
#include "lib/util.h"

/* OVS includes. */
#include "lib/vswitch-idl.h"
#include "include/openvswitch/shash.h"
#include "openvswitch/vlog.h"

/* OVN includes. */
#include "binding.h"
#include "lib/ovn-sb-idl.h"
#include "mirror.h"

VLOG_DEFINE_THIS_MODULE(port_mirror);

struct ovn_mirror {
    char *name;
    const struct sbrec_mirror *sb_mirror;
    const struct ovsrec_mirror *ovs_mirror;
    struct ovs_list mirror_src_lports;
    struct ovs_list mirror_dst_lports;
};

struct mirror_lport {
    struct ovs_list list_node;

    struct local_binding *lbinding;
};

static struct ovn_mirror *ovn_mirror_create(char *mirror_name);
static void ovn_mirror_add(struct shash *ovn_mirrors,
                           struct ovn_mirror *);
static struct ovn_mirror *ovn_mirror_find(struct shash *ovn_mirrors,
                                          const char *mirror_name);
static void ovn_mirror_delete(struct ovn_mirror *);
static void ovn_mirror_add_lport(struct ovn_mirror *, struct local_binding *);
static void sync_ovn_mirror(struct ovn_mirror *, struct ovsdb_idl_txn *,
                            const struct ovsrec_bridge *);

static void create_ovs_mirror(struct ovn_mirror *, struct ovsdb_idl_txn *,
                              const struct ovsrec_bridge *);
static void sync_ovs_mirror_ports(struct ovn_mirror *,
                                  const struct ovsrec_bridge *);
static void delete_ovs_mirror(struct ovn_mirror *,
                              const struct ovsrec_bridge *);
static bool should_delete_ovs_mirror(struct ovn_mirror *);
static void set_mirror_iface_options(struct ovsrec_interface *,
                                     const struct sbrec_mirror *);

static const struct ovsrec_port *get_iface_port(
    const struct ovsrec_interface *, const struct ovsrec_bridge *);


void
mirror_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_mirrors);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_mirror);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_name);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_output_port);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_select_dst_port);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_select_src_port);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_external_ids);
}

void
mirror_init(void)
{
}

void
mirror_destroy(void)
{
}

void
mirror_run(struct ovsdb_idl_txn *ovs_idl_txn,
           const struct ovsrec_mirror_table *ovs_mirror_table,
           const struct sbrec_mirror_table *sb_mirror_table,
           const struct ovsrec_bridge *br_int,
           struct shash *local_bindings)
{
    if (!ovs_idl_txn) {
        return;
    }

    struct shash ovn_mirrors = SHASH_INITIALIZER(&ovn_mirrors);
    struct shash tmp_mirrors = SHASH_INITIALIZER(&tmp_mirrors);

    /* Iterate through sb mirrors and build the 'ovn_mirrors'. */
    const struct sbrec_mirror *sb_mirror;
    SBREC_MIRROR_TABLE_FOR_EACH (sb_mirror, sb_mirror_table) {
        struct ovn_mirror *m = ovn_mirror_create(sb_mirror->name);
        m->sb_mirror = sb_mirror;
        ovn_mirror_add(&ovn_mirrors, m);
    }

    /* Iterate through ovs mirrors and add to the 'ovn_mirrors'. */
    const struct ovsrec_mirror *ovs_mirror;
    OVSREC_MIRROR_TABLE_FOR_EACH (ovs_mirror, ovs_mirror_table) {
        bool ovn_owned_mirror = smap_get_bool(&ovs_mirror->external_ids,
                                              "ovn-owned", false);
        if (!ovn_owned_mirror) {
            continue;
        }

        struct ovn_mirror *m = ovn_mirror_find(&ovn_mirrors, ovs_mirror->name);
        if (!m) {
            m = ovn_mirror_create(ovs_mirror->name);
            ovn_mirror_add(&ovn_mirrors, m);
        }
        m->ovs_mirror = ovs_mirror;
    }

    if (shash_is_empty(&ovn_mirrors)) {
        shash_destroy(&ovn_mirrors);
        return;
    }

    /* Iterate through the local bindings and if the local binding's 'pb' has
     * mirrors associated, add it to the ovn_mirror. */
    struct shash_node *node;
    SHASH_FOR_EACH (node, local_bindings) {
        struct local_binding *lbinding = node->data;
        const struct sbrec_port_binding *pb =
            local_binding_get_primary_pb(local_bindings, lbinding->name);
        if (!pb || !pb->n_mirror_rules) {
            continue;
        }

        for (size_t i = 0; i < pb->n_mirror_rules; i++) {
            struct ovn_mirror *m = ovn_mirror_find(&ovn_mirrors,
                                                   pb->mirror_rules[i]->name);
            ovs_assert(m);
            ovn_mirror_add_lport(m, lbinding);
        }
    }

    /* Iterate through the built 'ovn_mirrors' and
     * sync with the local ovsdb i.e.
     * create/update or delete the ovsrec mirror(s). */
     SHASH_FOR_EACH (node, &ovn_mirrors) {
        struct ovn_mirror *m = node->data;
        sync_ovn_mirror(m, ovs_idl_txn, br_int);
     }

    SHASH_FOR_EACH_SAFE (node, &ovn_mirrors) {
        ovn_mirror_delete(node->data);
        shash_delete(&ovn_mirrors, node);
    }

    shash_destroy(&ovn_mirrors);
}

/* Static functions. */
static struct ovn_mirror *
ovn_mirror_create(char *mirror_name)
{
    struct ovn_mirror *m = xzalloc(sizeof *m);
    m->name = xstrdup(mirror_name);
    ovs_list_init(&m->mirror_src_lports);
    ovs_list_init(&m->mirror_dst_lports);
    return m;
}

static void
ovn_mirror_add(struct shash *ovn_mirrors, struct ovn_mirror *m)
{
    shash_add(ovn_mirrors, m->name, m);
}

static struct ovn_mirror *
ovn_mirror_find(struct shash *ovn_mirrors, const char *mirror_name)
{
    return shash_find_data(ovn_mirrors, mirror_name);
}

static void
ovn_mirror_delete(struct ovn_mirror *m)
{
    free(m->name);
    struct mirror_lport *m_lport;
    LIST_FOR_EACH_POP (m_lport, list_node, &m->mirror_src_lports) {
        free(m_lport);
    }

    LIST_FOR_EACH_POP (m_lport, list_node, &m->mirror_dst_lports) {
        free(m_lport);
    }

    free(m);
}

static void
ovn_mirror_add_lport(struct ovn_mirror *m, struct local_binding *lbinding)
{
    struct mirror_lport *m_lport = xzalloc(sizeof *m_lport);
    m_lport->lbinding = lbinding;
    if (!strcmp(m->sb_mirror->filter, "from-lport")) {
        ovs_list_push_back(&m->mirror_src_lports, &m_lport->list_node);
    } else {
        ovs_list_push_back(&m->mirror_dst_lports, &m_lport->list_node);
    }
}

static void
set_mirror_iface_options(struct ovsrec_interface *iface,
                         const struct sbrec_mirror *sb_mirror)
{
    struct smap options = SMAP_INITIALIZER(&options);
    char *key;

    key = xasprintf("%ld", (long int) sb_mirror->index);
    smap_add(&options, "remote_ip", sb_mirror->sink);
    smap_add(&options, "key", key);
    if (!strcmp(sb_mirror->type, "erspan")) {
        /* Set the ERSPAN index */
        smap_add(&options, "erspan_idx", key);
        smap_add(&options, "erspan_ver", "1");
    }
    ovsrec_interface_set_options(iface, &options);

    free(key);
    smap_destroy(&options);
}

static void
check_and_update_interface_table(const struct sbrec_mirror *sb_mirror,
                                 const struct ovsrec_mirror *ovs_mirror)
{
    char *type;
    struct ovsrec_interface *iface =
                          ovs_mirror->output_port->interfaces[0];
    struct smap *opts = &iface->options;
    const char *erspan_ver = smap_get(opts, "erspan_ver");
    if (erspan_ver) {
        type = "erspan";
    } else {
        type = "gre";
    }
    if (strcmp(type, sb_mirror->type)) {
        ovsrec_interface_set_type(iface, sb_mirror->type);
    }
    set_mirror_iface_options(iface, sb_mirror);
}

static void
sync_ovn_mirror(struct ovn_mirror *m, struct ovsdb_idl_txn *ovs_idl_txn,
                const struct ovsrec_bridge *br_int)
{
    if (should_delete_ovs_mirror(m)) {
        /* Delete the ovsrec mirror. */
        delete_ovs_mirror(m, br_int);
        return;
    }

    if (ovs_list_is_empty(&m->mirror_src_lports) &&
            ovs_list_is_empty(&m->mirror_dst_lports)) {
        /* Nothing to do. */
        return;
    }

    if (m->sb_mirror && !m->ovs_mirror) {
        create_ovs_mirror(m, ovs_idl_txn, br_int);
    } else {
        check_and_update_interface_table(m->sb_mirror, m->ovs_mirror);
    }

    sync_ovs_mirror_ports(m, br_int);
}

static bool
should_delete_ovs_mirror(struct ovn_mirror *m)
{
    if (!m->ovs_mirror) {
        return false;
    }

    if (m->ovs_mirror && !m->sb_mirror) {
        return true;
    }

    return (ovs_list_is_empty(&m->mirror_src_lports) &&
            ovs_list_is_empty(&m->mirror_dst_lports));
}

static const struct ovsrec_port *
get_iface_port(const struct ovsrec_interface *iface,
               const struct ovsrec_bridge *br_int)
{
    for (size_t i = 0; i < br_int->n_ports; i++) {
        const struct ovsrec_port *p = br_int->ports[i];
        for (size_t j = 0; j < p->n_interfaces; j++) {
            if (!strcmp(iface->name, p->interfaces[j]->name)) {
                return p;
            }
        }
    }
    return NULL;
}

static void
create_ovs_mirror(struct ovn_mirror *m, struct ovsdb_idl_txn *ovs_idl_txn,
                  const struct ovsrec_bridge *br_int)
{
    struct ovsrec_interface *iface = ovsrec_interface_insert(ovs_idl_txn);
    char *port_name = xasprintf("ovn-%s", m->name);

    ovsrec_interface_set_name(iface, port_name);
    ovsrec_interface_set_type(iface, m->sb_mirror->type);
    set_mirror_iface_options(iface, m->sb_mirror);

    struct ovsrec_port *port = ovsrec_port_insert(ovs_idl_txn);
    ovsrec_port_set_name(port, port_name);
    ovsrec_port_set_interfaces(port, &iface, 1);
    ovsrec_bridge_update_ports_addvalue(br_int, port);

    free(port_name);

    m->ovs_mirror = ovsrec_mirror_insert(ovs_idl_txn);
    ovsrec_mirror_set_name(m->ovs_mirror, m->name);
    ovsrec_mirror_set_output_port(m->ovs_mirror, port);

    const struct smap external_ids =
        SMAP_CONST1(&external_ids, "ovn-owned", "true");
    ovsrec_mirror_set_external_ids(m->ovs_mirror, &external_ids);

    ovsrec_bridge_update_mirrors_addvalue(br_int, m->ovs_mirror);
}

static void
sync_ovs_mirror_ports(struct ovn_mirror *m, const struct ovsrec_bridge *br_int)
{
    struct mirror_lport *m_lport;

    if (ovs_list_is_empty(&m->mirror_src_lports)) {
        ovsrec_mirror_set_select_src_port(m->ovs_mirror, NULL, 0);
    } else {
        size_t n_lports = ovs_list_size(&m->mirror_src_lports);
        struct ovsrec_port **ovs_ports = xmalloc(sizeof *ovs_ports * n_lports);

        size_t i = 0;
        LIST_FOR_EACH (m_lport, list_node, &m->mirror_src_lports) {
            const struct ovsrec_port *p =
                get_iface_port(m_lport->lbinding->iface, br_int);
            ovs_assert(p);
            ovs_ports[i++] = (struct ovsrec_port *) p;
        }

        ovsrec_mirror_set_select_src_port(m->ovs_mirror, ovs_ports, n_lports);
        free(ovs_ports);
    }

    if (ovs_list_is_empty(&m->mirror_dst_lports)) {
        ovsrec_mirror_set_select_dst_port(m->ovs_mirror, NULL, 0);
    } else {
        size_t n_lports = ovs_list_size(&m->mirror_dst_lports);
        struct ovsrec_port **ovs_ports = xmalloc(sizeof *ovs_ports * n_lports);

        size_t i = 0;
        LIST_FOR_EACH (m_lport, list_node, &m->mirror_dst_lports) {
            const struct ovsrec_port *p =
                get_iface_port(m_lport->lbinding->iface, br_int);
            ovs_assert(p);
            ovs_ports[i++] = (struct ovsrec_port *) p;
        }

        ovsrec_mirror_set_select_dst_port(m->ovs_mirror, ovs_ports, n_lports);
        free(ovs_ports);
    }
}

static void
delete_ovs_mirror(struct ovn_mirror *m, const struct ovsrec_bridge *br_int)
{
    ovsrec_bridge_update_ports_delvalue(br_int, m->ovs_mirror->output_port);
    ovsrec_bridge_update_mirrors_delvalue(br_int, m->ovs_mirror);
    ovsrec_port_delete(m->ovs_mirror->output_port);
    ovsrec_mirror_delete(m->ovs_mirror);
}
