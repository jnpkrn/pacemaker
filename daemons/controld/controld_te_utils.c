/*
 * Copyright 2004-2018 Andrew Beekhof <andrew@beekhof.net>
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <crm/crm.h>

#include <crm/msg_xml.h>

#include <crm/common/xml.h>
#include <controld_transition.h>
#include <controld_fsa.h>
#include <controld_lrm.h>
#include <controld_messages.h>
#include <controld_throttle.h>
#include <crm/fencing/internal.h>

crm_trigger_t *stonith_reconnect = NULL;
static crm_trigger_t *stonith_history_sync_trigger = NULL;
static mainloop_timer_t *stonith_history_sync_timer = NULL;

/*
 * stonith cleanup list
 *
 * If the DC is shot, proper notifications might not go out.
 * The stonith cleanup list allows the cluster to (re-)send
 * notifications once a new DC is elected.
 */

static GListPtr stonith_cleanup_list = NULL;

/*!
 * \internal
 * \brief Add a node to the stonith cleanup list
 *
 * \param[in] target  Name of node to add
 */
void
add_stonith_cleanup(const char *target) {
    stonith_cleanup_list = g_list_append(stonith_cleanup_list, strdup(target));
}

/*!
 * \internal
 * \brief Remove a node from the stonith cleanup list
 *
 * \param[in] Name of node to remove
 */
void
remove_stonith_cleanup(const char *target)
{
    GListPtr iter = stonith_cleanup_list;

    while (iter != NULL) {
        GListPtr tmp = iter;
        char *iter_name = tmp->data;

        iter = iter->next;
        if (safe_str_eq(target, iter_name)) {
            crm_trace("Removing %s from the cleanup list", iter_name);
            stonith_cleanup_list = g_list_delete_link(stonith_cleanup_list, tmp);
            free(iter_name);
        }
    }
}

/*!
 * \internal
 * \brief Purge all entries from the stonith cleanup list
 */
void
purge_stonith_cleanup()
{
    if (stonith_cleanup_list) {
        GListPtr iter = NULL;

        for (iter = stonith_cleanup_list; iter != NULL; iter = iter->next) {
            char *target = iter->data;

            crm_info("Purging %s from stonith cleanup list", target);
            free(target);
        }
        g_list_free(stonith_cleanup_list);
        stonith_cleanup_list = NULL;
    }
}

/*!
 * \internal
 * \brief Send stonith updates for all entries in cleanup list, then purge it
 */
void
execute_stonith_cleanup()
{
    GListPtr iter;

    for (iter = stonith_cleanup_list; iter != NULL; iter = iter->next) {
        char *target = iter->data;
        crm_node_t *target_node = crm_get_peer(0, target);
        const char *uuid = crm_peer_uuid(target_node);

        crm_notice("Marking %s, target of a previous stonith action, as clean", target);
        send_stonith_update(NULL, target, uuid);
        free(target);
    }
    g_list_free(stonith_cleanup_list);
    stonith_cleanup_list = NULL;
}

/* end stonith cleanup list functions */

static gboolean
fail_incompletable_stonith(crm_graph_t * graph)
{
    GListPtr lpc = NULL;
    const char *task = NULL;
    xmlNode *last_action = NULL;

    if (graph == NULL) {
        return FALSE;
    }

    for (lpc = graph->synapses; lpc != NULL; lpc = lpc->next) {
        GListPtr lpc2 = NULL;
        synapse_t *synapse = (synapse_t *) lpc->data;

        if (synapse->confirmed) {
            continue;
        }

        for (lpc2 = synapse->actions; lpc2 != NULL; lpc2 = lpc2->next) {
            crm_action_t *action = (crm_action_t *) lpc2->data;

            if (action->type != action_type_crm || action->confirmed) {
                continue;
            }

            task = crm_element_value(action->xml, XML_LRM_ATTR_TASK);
            if (task && safe_str_eq(task, CRM_OP_FENCE)) {
                action->failed = TRUE;
                last_action = action->xml;
                update_graph(graph, action);
                crm_notice("Failing action %d (%s): fencer terminated",
                           action->id, ID(action->xml));
            }
        }
    }

    if (last_action != NULL) {
        crm_warn("Fencer failure resulted in unrunnable actions");
        abort_for_stonith_failure(tg_restart, NULL, last_action);
        return TRUE;
    }

    return FALSE;
}

static void
tengine_stonith_connection_destroy(stonith_t * st, stonith_event_t * e)
{
    if (is_set(fsa_input_register, R_ST_REQUIRED)) {
        crm_crit("Fencing daemon connection failed");
        mainloop_set_trigger(stonith_reconnect);

    } else {
        crm_info("Fencing daemon disconnected");
    }

    /* cbchan will be garbage at this point, arrange for it to be reset */
    if(stonith_api) {
        stonith_api->state = stonith_disconnected;
    }

    if (AM_I_DC) {
        fail_incompletable_stonith(transition_graph);
        trigger_graph();
    }
}

char *te_client_id = NULL;

#ifdef HAVE_SYS_REBOOT_H
#  include <unistd.h>
#  include <sys/reboot.h>
#endif

static void
tengine_stonith_notify(stonith_t * st, stonith_event_t * st_event)
{
    if(te_client_id == NULL) {
        te_client_id = crm_strdup_printf("%s.%lu", crm_system_name,
                                         (unsigned long) getpid());
    }

    if (st_event == NULL) {
        crm_err("Notify data not found");
        return;
    }

    crmd_alert_fencing_op(st_event);

    if (st_event->result == pcmk_ok && safe_str_eq("on", st_event->action)) {
        crm_notice("%s was successfully unfenced by %s (at the request of %s)",
                   st_event->target, st_event->executioner ? st_event->executioner : "<anyone>", st_event->origin);
                /* TODO: Hook up st_event->device */
        return;

    } else if (safe_str_eq("on", st_event->action)) {
        crm_err("Unfencing of %s by %s failed: %s (%d)",
                st_event->target, st_event->executioner ? st_event->executioner : "<anyone>",
                pcmk_strerror(st_event->result), st_event->result);
        return;

    } else if (st_event->result == pcmk_ok && crm_str_eq(st_event->target, fsa_our_uname, TRUE)) {
        crm_crit("We were allegedly just fenced by %s for %s!",
                 st_event->executioner ? st_event->executioner : "<anyone>", st_event->origin); /* Dumps blackbox if enabled */

        qb_log_fini(); /* Try to get the above log message to disk - somehow */

        /* Get out ASAP and do not come back up.
         *
         * Triggering a reboot is also not the worst idea either since
         * the rest of the cluster thinks we're safely down
         */

#ifdef RB_HALT_SYSTEM
        pcmk__reboot(RB_HALT_SYSTEM);
#endif

        /*
         * If reboot() fails or is not supported, coming back up will
         * probably lead to a situation where the other nodes set our
         * status to 'lost' because of the fencing callback and will
         * discard subsequent election votes with:
         *
         * Election 87 (current: 5171, owner: 103): Processed vote from east-03 (Peer is not part of our cluster)
         *
         * So just stay dead, something is seriously messed up anyway.
         *
         */
        exit(CRM_EX_FATAL); // None of our wrappers since we already called qb_log_fini()
        return;
    }

    /* Update the count of stonith failures for this target, in case we become
     * DC later. The current DC has already updated its fail count in
     * tengine_stonith_callback().
     */
    if (!AM_I_DC && safe_str_eq(st_event->operation, T_STONITH_NOTIFY_FENCE)) {
        if (st_event->result == pcmk_ok) {
            st_fail_count_reset(st_event->target);
        } else {
            st_fail_count_increment(st_event->target);
        }
    }

    crm_notice("Peer %s was%s terminated (%s) by %s on behalf of %s: %s "
               CRM_XS " initiator=%s ref=%s",
               st_event->target, st_event->result == pcmk_ok ? "" : " not",
               st_event->action,
               st_event->executioner ? st_event->executioner : "<anyone>",
               (st_event->client_origin? st_event->client_origin : "<unknown>"),
               pcmk_strerror(st_event->result),
               st_event->origin, st_event->id);

    if (st_event->result == pcmk_ok) {
        crm_node_t *peer = crm_find_known_peer_full(0, st_event->target, CRM_GET_PEER_ANY);
        const char *uuid = NULL;
        gboolean we_are_executioner = safe_str_eq(st_event->executioner, fsa_our_uname);

        if (peer == NULL) {
            return;
        }

        uuid = crm_peer_uuid(peer);

        crm_trace("target=%s dc=%s", st_event->target, fsa_our_dc);
        if(AM_I_DC) {
            /* The DC always sends updates */
            send_stonith_update(NULL, st_event->target, uuid);

            /* @TODO Ideally, at this point, we'd check whether the fenced node
             * hosted any guest nodes, and call remote_node_down() for them.
             * Unfortunately, the controller doesn't have a simple, reliable way
             * to map hosts to guests. It might be possible to track this in the
             * peer cache via crm_remote_peer_cache_refresh(). For now, we rely
             * on the PE creating fence pseudo-events for the guests.
             */

            if (st_event->client_origin && safe_str_neq(st_event->client_origin, te_client_id)) {

                /* Abort the current transition graph if it wasn't us
                 * that invoked stonith to fence someone
                 */
                crm_info("External fencing operation from %s fenced %s", st_event->client_origin, st_event->target);
                abort_transition(INFINITY, tg_restart, "External Fencing Operation", NULL);
            }

            /* Assume it was our leader if we don't currently have one */
        } else if (((fsa_our_dc == NULL) || safe_str_eq(fsa_our_dc, st_event->target))
            && !is_set(peer->flags, crm_remote_node)) {

            crm_notice("Target %s our leader %s (recorded: %s)",
                       fsa_our_dc ? "was" : "may have been", st_event->target,
                       fsa_our_dc ? fsa_our_dc : "<unset>");

            /* Given the CIB resyncing that occurs around elections,
             * have one node update the CIB now and, if the new DC is different,
             * have them do so too after the election
             */
            if (we_are_executioner) {
                send_stonith_update(NULL, st_event->target, uuid);
            }
            add_stonith_cleanup(st_event->target);
        }

        /* If the target is a remote node, and we host its connection,
         * immediately fail all monitors so it can be recovered quickly.
         * The connection won't necessarily drop when a remote node is fenced,
         * so the failure might not otherwise be detected until the next poke.
         */
        if (is_set(peer->flags, crm_remote_node)) {
            remote_ra_fail(st_event->target);
        }

        crmd_peer_down(peer, TRUE);
     }
}

static gboolean
do_stonith_history_sync(gpointer user_data)
{
    if (stonith_api && (stonith_api->state != stonith_disconnected)) {
        stonith_history_t *history = NULL;

        stonith_api->cmds->history(stonith_api,
                                   st_opt_sync_call | st_opt_broadcast,
                                   NULL, &history, 5);
        stonith_history_free(history);
        return TRUE;
    } else {
        crm_info("Skip triggering stonith history-sync as stonith is disconnected");
        return FALSE;
    }
}

static gboolean
stonith_history_sync_set_trigger(gpointer user_data)
{
    mainloop_set_trigger(stonith_history_sync_trigger);
    return FALSE;
}

void
te_trigger_stonith_history_sync(void)
{
    /* trigger a sync in 5s to give more nodes the
     * chance to show up so that we don't create
     * unnecessary stonith-history-sync traffic
     */

    /* as we are finally checking the stonith-connection
     * in do_stonith_history_sync we should be fine
     * leaving stonith_history_sync_time & stonith_history_sync_trigger
     * around
     */
    if (stonith_history_sync_trigger == NULL) {
        stonith_history_sync_trigger =
            mainloop_add_trigger(G_PRIORITY_LOW,
                                 do_stonith_history_sync, NULL);
    }

    if(stonith_history_sync_timer == NULL) {
        stonith_history_sync_timer =
            mainloop_timer_add("history_sync", 5000,
                               FALSE, stonith_history_sync_set_trigger,
                               NULL);
    }
    crm_info("Fence history will be synchronized cluster-wide within 5 seconds");
    mainloop_timer_start(stonith_history_sync_timer);
}

gboolean
te_connect_stonith(gpointer user_data)
{
    int lpc = 0;
    int rc = pcmk_ok;

    if (stonith_api == NULL) {
        stonith_api = stonith_api_new();
    }

    if (stonith_api->state != stonith_disconnected) {
        crm_trace("Still connected");
        return TRUE;
    }

    for (lpc = 0; lpc < 30; lpc++) {
        crm_debug("Attempting connection to fencing daemon...");

        sleep(1);
        rc = stonith_api->cmds->connect(stonith_api, crm_system_name, NULL);

        if (rc == pcmk_ok) {
            break;
        }

        if (user_data != NULL) {
            if (is_set(fsa_input_register, R_ST_REQUIRED)) {
                crm_err("Sign-in failed: triggered a retry");
                mainloop_set_trigger(stonith_reconnect);
            } else {
                crm_info("Sign-in failed, but no longer required");
            }
            return TRUE;
        }

        crm_err("Sign-in failed: pausing and trying again in 2s...");
        sleep(1);
    }

    CRM_CHECK(rc == pcmk_ok, return TRUE);      /* If not, we failed 30 times... just get out */
    stonith_api->cmds->register_notification(stonith_api, T_STONITH_NOTIFY_DISCONNECT,
                                             tengine_stonith_connection_destroy);

    stonith_api->cmds->register_notification(stonith_api, T_STONITH_NOTIFY_FENCE,
                                             tengine_stonith_notify);

    crm_trace("Connected");
    return TRUE;
}

gboolean
stop_te_timer(crm_action_timer_t * timer)
{
    if (timer == NULL) {
        return FALSE;
    }
    if (timer->source_id != 0) {
        crm_trace("Stopping action timer");
        g_source_remove(timer->source_id);
        timer->source_id = 0;
    } else {
        crm_trace("Action timer was already stopped");
        return FALSE;
    }
    return TRUE;
}

gboolean
te_graph_trigger(gpointer user_data)
{
    enum transition_status graph_rc = -1;

    if (transition_graph == NULL) {
        crm_debug("Nothing to do");
        return TRUE;
    }

    crm_trace("Invoking graph %d in state %s", transition_graph->id, fsa_state2string(fsa_state));

    switch (fsa_state) {
        case S_STARTING:
        case S_PENDING:
        case S_NOT_DC:
        case S_HALT:
        case S_ILLEGAL:
        case S_STOPPING:
        case S_TERMINATE:
            return TRUE;
            break;
        default:
            break;
    }

    if (transition_graph->complete == FALSE) {
        int limit = transition_graph->batch_limit;

        transition_graph->batch_limit = throttle_get_total_job_limit(limit);
        graph_rc = run_graph(transition_graph);
        transition_graph->batch_limit = limit; /* Restore the configured value */

        /* significant overhead... */
        /* print_graph(LOG_TRACE, transition_graph); */

        if (graph_rc == transition_active) {
            crm_trace("Transition not yet complete");
            return TRUE;

        } else if (graph_rc == transition_pending) {
            crm_trace("Transition not yet complete - no actions fired");
            return TRUE;
        }

        if (graph_rc != transition_complete) {
            crm_warn("Transition failed: %s", transition_status(graph_rc));
            print_graph(LOG_NOTICE, transition_graph);
        }
    }

    crm_debug("Transition %d is now complete", transition_graph->id);
    transition_graph->complete = TRUE;
    notify_crmd(transition_graph);

    return TRUE;
}

void
trigger_graph_processing(const char *fn, int line)
{
    crm_trace("%s:%d - Triggered graph processing", fn, line);
    mainloop_set_trigger(transition_trigger);
}

static struct abort_timer_s {
    bool aborted;
    guint id;
    int priority;
    enum transition_action action;
    const char *text;
} abort_timer = { 0, };

static gboolean
abort_timer_popped(gpointer data)
{
    if (AM_I_DC && (abort_timer.aborted == FALSE)) {
        abort_transition(abort_timer.priority, abort_timer.action,
                         abort_timer.text, NULL);
    }
    abort_timer.id = 0;
    return FALSE; // do not immediately reschedule timer
}

/*!
 * \internal
 * \brief Abort transition after delay, if not already aborted in that time
 *
 * \param[in] abort_text  Must be literal string
 */
void
abort_after_delay(int abort_priority, enum transition_action abort_action,
                  const char *abort_text, guint delay_ms)
{
    if (abort_timer.id) {
        // Timer already in progress, stop and reschedule
        g_source_remove(abort_timer.id);
    }
    abort_timer.aborted = FALSE;
    abort_timer.priority = abort_priority;
    abort_timer.action = abort_action;
    abort_timer.text = abort_text;
    abort_timer.id = g_timeout_add(delay_ms, abort_timer_popped, NULL);
}

void
abort_transition_graph(int abort_priority, enum transition_action abort_action,
                       const char *abort_text, xmlNode * reason, const char *fn, int line)
{
    int add[] = { 0, 0, 0 };
    int del[] = { 0, 0, 0 };
    int level = LOG_INFO;
    xmlNode *diff = NULL;
    xmlNode *change = NULL;

    CRM_CHECK(transition_graph != NULL, return);

    switch (fsa_state) {
        case S_STARTING:
        case S_PENDING:
        case S_NOT_DC:
        case S_HALT:
        case S_ILLEGAL:
        case S_STOPPING:
        case S_TERMINATE:
            crm_info("Abort %s suppressed: state=%s (complete=%d)",
                     abort_text, fsa_state2string(fsa_state), transition_graph->complete);
            return;
        default:
            break;
    }

    abort_timer.aborted = TRUE;

    /* Make sure any queued calculations are discarded ASAP */
    free(fsa_pe_ref);
    fsa_pe_ref = NULL;

    if (transition_graph->complete == FALSE) {
        if(update_abort_priority(transition_graph, abort_priority, abort_action, abort_text)) {
            level = LOG_NOTICE;
        }
    }

    if(reason) {
        xmlNode *search = NULL;

        for(search = reason; search; search = search->parent) {
            if (safe_str_eq(XML_TAG_DIFF, TYPE(search))) {
                diff = search;
                break;
            }
        }

        if(diff) {
            xml_patch_versions(diff, add, del);
            for(search = reason; search; search = search->parent) {
                if (safe_str_eq(XML_DIFF_CHANGE, TYPE(search))) {
                    change = search;
                    break;
                }
            }
        }
    }

    if(reason == NULL) {
        do_crm_log(level, "Transition %d aborted: %s "CRM_XS" source=%s:%d complete=%s",
                   transition_graph->id, abort_text, fn, line,
                   (transition_graph->complete? "true" : "false"));

    } else if(change == NULL) {
        char *local_path = xml_get_path(reason);

        do_crm_log(level, "Transition %d aborted by %s.%s: %s "
                   CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                   transition_graph->id, TYPE(reason), ID(reason), abort_text,
                   add[0], add[1], add[2], fn, line, local_path,
                   (transition_graph->complete? "true" : "false"));
        free(local_path);

    } else {
        const char *kind = NULL;
        const char *op = crm_element_value(change, XML_DIFF_OP);
        const char *path = crm_element_value(change, XML_DIFF_PATH);

        if(change == reason) {
            if(strcmp(op, "create") == 0) {
                reason = reason->children;

            } else if(strcmp(op, "modify") == 0) {
                reason = first_named_child(reason, XML_DIFF_RESULT);
                if(reason) {
                    reason = reason->children;
                }
            }
        }

        kind = TYPE(reason);
        if(strcmp(op, "delete") == 0) {
            const char *shortpath = strrchr(path, '/');

            do_crm_log(level, "Transition %d aborted by deletion of %s: %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                       transition_graph->id,
                       (shortpath? (shortpath + 1) : path), abort_text,
                       add[0], add[1], add[2], fn, line, path,
                       (transition_graph->complete? "true" : "false"));

        } else if (safe_str_eq(XML_CIB_TAG_NVPAIR, kind)) { 
            do_crm_log(level, "Transition %d aborted by %s doing %s %s=%s: %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                       transition_graph->id,
                       crm_element_value(reason, XML_ATTR_ID), op,
                       crm_element_value(reason, XML_NVPAIR_ATTR_NAME),
                       crm_element_value(reason, XML_NVPAIR_ATTR_VALUE),
                       abort_text, add[0], add[1], add[2], fn, line, path,
                       (transition_graph->complete? "true" : "false"));

        } else if (safe_str_eq(XML_LRM_TAG_RSC_OP, kind)) {
            const char *magic = crm_element_value(reason, XML_ATTR_TRANSITION_MAGIC);

            do_crm_log(level, "Transition %d aborted by operation %s '%s' on %s: %s "
                       CRM_XS " magic=%s cib=%d.%d.%d source=%s:%d complete=%s",
                       transition_graph->id,
                       crm_element_value(reason, XML_LRM_ATTR_TASK_KEY), op,
                       crm_element_value(reason, XML_LRM_ATTR_TARGET), abort_text,
                       magic, add[0], add[1], add[2], fn, line,
                       (transition_graph->complete? "true" : "false"));

        } else if (safe_str_eq(XML_CIB_TAG_STATE, kind)
                   || safe_str_eq(XML_CIB_TAG_NODE, kind)) {
            const char *uname = crm_peer_uname(ID(reason));

            do_crm_log(level, "Transition %d aborted by %s '%s' on %s: %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d complete=%s",
                       transition_graph->id,
                       kind, op, (uname? uname : ID(reason)), abort_text,
                       add[0], add[1], add[2], fn, line,
                       (transition_graph->complete? "true" : "false"));

        } else {
            const char *id = ID(reason);

            do_crm_log(level, "Transition %d aborted by %s.%s '%s': %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                       transition_graph->id,
                       TYPE(reason), (id? id : ""), (op? op : "change"),
                       abort_text, add[0], add[1], add[2], fn, line, path,
                       (transition_graph->complete? "true" : "false"));
        }
    }

    if (transition_graph->complete) {
        if (transition_timer->period_ms > 0) {
            crm_timer_stop(transition_timer);
            crm_timer_start(transition_timer);
        } else {
            register_fsa_input(C_FSA_INTERNAL, I_PE_CALC, NULL);
        }
        return;
    }

    mainloop_set_trigger(transition_trigger);
}
