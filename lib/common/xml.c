/*
 * Copyright 2004-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <bzlib.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlIO.h>  /* xmlAllocOutputBuffer */

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/iso8601_internal.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>  /* CRM_XML_LOG_BASE */
#include "crmcommon_private.h"

#define XML_BUFFER_SIZE	4096
#define XML_PARSER_DEBUG 0

/* @TODO XML_PARSE_RECOVER allows some XML errors to be silently worked around
 * by libxml2, which is potentially ambiguous and dangerous. We should drop it
 * when we can break backward compatibility with configurations that might be
 * relying on it (i.e. pacemaker 3.0.0).
 *
 * It might be a good idea to have a transitional period where we first try
 * parsing without XML_PARSE_RECOVER, and if that fails, try parsing again with
 * it, logging a warning if it succeeds.
 */
#define PCMK__XML_PARSE_OPTS    (XML_PARSE_NOBLANKS | XML_PARSE_RECOVER)

typedef struct {
    int found;
    const char *string;
} filter_t;

typedef struct xml_deleted_obj_s {
        char *path;
        int position;
} xml_deleted_obj_t;

/* *INDENT-OFF* */

static filter_t filter[] = {
    { 0, XML_ATTR_ORIGIN },
    { 0, XML_CIB_ATTR_WRITTEN },
    { 0, XML_ATTR_UPDATE_ORIG },
    { 0, XML_ATTR_UPDATE_CLIENT },
    { 0, XML_ATTR_UPDATE_USER },
};
/* *INDENT-ON* */

static xmlNode *subtract_xml_comment(xmlNode * parent, xmlNode * left, xmlNode * right, gboolean * changed);
static xmlNode *find_xml_comment(xmlNode * root, xmlNode * search_comment, gboolean exact);
static int add_xml_comment(xmlNode * parent, xmlNode * target, xmlNode * update);

#define CHUNK_SIZE 1024

bool
pcmk__tracking_xml_changes(xmlNode *xml, bool lazy)
{
    xml_doc_private_t *docpriv;

    if (xml == NULL || xml->doc == NULL || xml->doc->_private == NULL) {
        return FALSE;
    }

    docpriv = xml->doc->_private;
    if (is_not_set(docpriv->flags, xpf_tracking)
            || (lazy && is_not_set(docpriv->flags, xpf_lazy))) {
        return FALSE;
    }
    return TRUE;
}

#define buffer_print(buffer, max, offset, fmt, args...) do {            \
        int rc = (max);                                                 \
        if(buffer) {                                                    \
            rc = snprintf((buffer) + (offset), (max) - (offset), fmt, ##args); \
        }                                                               \
        if(buffer && rc < 0) {                                          \
            crm_perror(LOG_ERR, "snprintf failed at offset %d", offset); \
            (buffer)[(offset)] = 0;                                     \
            break;                                                      \
        } else if(rc >= ((max) - (offset))) {                           \
            char *tmp = NULL;                                           \
            (max) = QB_MAX(CHUNK_SIZE, (max) * 2);                      \
            tmp = realloc_safe((buffer), (max));                        \
            CRM_ASSERT(tmp);                                            \
            (buffer) = tmp;                                             \
        } else {                                                        \
            offset += rc;                                               \
            break;                                                      \
        }                                                               \
    } while(1);

static void
insert_prefix(int options, char **buffer, int *offset, int *max, int depth)
{
    if (options & xml_log_option_formatted) {
        size_t spaces = 2 * depth;

        if ((*buffer) == NULL || spaces >= ((*max) - (*offset))) {
            (*max) = QB_MAX(CHUNK_SIZE, (*max) * 2);
            (*buffer) = realloc_safe((*buffer), (*max));
        }
        memset((*buffer) + (*offset), ' ', spaces);
        (*offset) += spaces;
    }
}

static void
set_parent_flag(xmlNode *xml, long flag) 
{

    for(; xml; xml = xml->parent) {
        xml_node_private_t *nodepriv = xml->_private;

        if (nodepriv == NULL) {
            /* During calls to xmlDocCopyNode(), _private will be unset for parent nodes */
        } else {
            nodepriv->flags |= flag;
            /* crm_trace("Setting flag %x due to %s[@id=%s]", flag, xml->name, ID(xml)); */
        }
    }
}

void
pcmk__set_xml_flag(xmlNode *xml, enum xml_private_flags flag)
{

    if(xml && xml->doc && xml->doc->_private){
        /* During calls to xmlDocCopyNode(), xml->doc may be unset */
        xml_doc_private_t *docpriv = xml->doc->_private;

        docpriv->flags |= flag;
        /* crm_trace("Setting flag %x due to %s[@id=%s]", flag, xml->name, ID(xml)); */
    }
}

static void
__xml_node_dirty(xmlNode *xml) 
{
    pcmk__set_xml_flag(xml, xpf_dirty);
    set_parent_flag(xml, xpf_dirty);
}

static void
__xml_node_clean(xmlNode *xml) 
{
    xmlNode *cIter = NULL;
    xml_node_private_t *nodepriv = xml->_private;

    if (nodepriv) {
        nodepriv->flags = 0;
    }

    for (cIter = __xml_first_child(xml); cIter != NULL; cIter = __xml_next(cIter)) {
        __xml_node_clean(cIter);
    }
}

static void
crm_node_created(xmlNode *xml) 
{
    xmlNode *cIter = NULL;
    xml_node_private_t *nodepriv = xml->_private;

    if(nodepriv && pcmk__tracking_xml_changes(xml, FALSE)) {
        if(is_not_set(nodepriv->flags, xpf_created)) {
            nodepriv->flags |= xpf_created;
            __xml_node_dirty(xml);
        }

        for (cIter = __xml_first_child(xml); cIter != NULL; cIter = __xml_next(cIter)) {
           crm_node_created(cIter);
        }
    }
}

void
pcmk__mark_xml_attr_dirty(xmlAttr *a) 
{
    xmlNode *parent = a->parent;
    xml_node_private_t *nodepriv = a->_private;

    nodepriv->flags |= (xpf_dirty|xpf_modified);
    nodepriv->flags &= ~xpf_deleted;
    /* crm_trace("Setting flag %x due to %s[@id=%s, @%s=%s]", */
    /*           xpf_dirty, parent?parent->name:NULL, ID(parent), a->name, a->children->content); */

    __xml_node_dirty(parent);
}

int get_tag_name(const char *input, size_t offset, size_t max);
int get_attr_name(const char *input, size_t offset, size_t max);
int get_attr_value(const char *input, size_t offset, size_t max);
gboolean can_prune_leaf(xmlNode * xml_node);

static int add_xml_object(xmlNode * parent, xmlNode * target, xmlNode * update, gboolean as_diff);

#define XML_DOC_PRIVATE_MAGIC   0x81726354UL
#define XML_NODE_PRIVATE_MAGIC  0x54637281UL

static void
__xml_deleted_obj_free(void *data)
{
    if(data) {
        xml_deleted_obj_t *deleted_obj = data;

        free(deleted_obj->path);
        free(deleted_obj);
    }
}

xml_doc_private_t *
__xml_doc_private_create(xml_doc_private_t **docpriv)
{
    *docpriv = calloc(1, sizeof(xml_doc_private_t));
    CRM_CHECK(*docpriv != NULL, return *docpriv);
    (*docpriv)->check = XML_DOC_PRIVATE_MAGIC;
    /* Flags will be reset if necessary when tracking is enabled */
    (*docpriv)->flags |= (xpf_dirty|xpf_created);
    return *docpriv;
}

static void
__xml_doc_private_clean(xml_doc_private_t *docpriv)
{
    if (docpriv != NULL) {
        CRM_ASSERT(docpriv->check == XML_DOC_PRIVATE_MAGIC);

        free(docpriv->user);
        docpriv->user = NULL;

        if (docpriv->acls != NULL) {
            pcmk__free_acls(docpriv->acls);
            docpriv->acls = NULL;
        }

        if (docpriv->deleted_objs != NULL) {
            g_list_free_full(docpriv->deleted_objs, __xml_deleted_obj_free);
            docpriv->deleted_objs = NULL;
        }
    }
}

static void
pcmkDeregisterNode(xmlNodePtr node)
{
    /* need to explicitly avoid our custom _private field cleanup when
       called from internal XSLT cleanup (xsltApplyStylesheetInternal
       -> xsltFreeTransformContext -> xsltFreeRVTs -> xmlFreeDoc)
       onto result tree fragments, represented as standalone documents
       with otherwise infeasible space-prefixed name (xsltInternals.h:
       XSLT_MARK_RES_TREE_FRAG) and carrying its own payload at _private
       field -- later assert on the XML_*_PRIVATE_MAGIC would explode */
    if (node->name == NULL || node->name[0] != ' ') {
        if (node->_private) {
            if (node->type == XML_DOCUMENT_NODE) {
                __xml_doc_private_clean(node->_private);
            } else {
                CRM_ASSERT(((xml_node_private_t *) node->_private)->check
                               == XML_NODE_PRIVATE_MAGIC);
                /* nothing dynamically allocated nested */
            }
            free(node->_private);
            node->_private = NULL;
        }
    }
}

static void
pcmkRegisterNode(xmlNodePtr node)
{
    switch (node->type) {
        case XML_DOCUMENT_NODE: {
            /* we don't know the root tag yet, but we cannot speculatively
               expect it's going to be a base config and roll back in
               XML_ELEMENT_NODE clause later on either, since node won't
               have doc item initialized yet; thus, we opt for lazy, ad-hoc
               initialization of the _private member from pcmk__unpack_acl */
            break;
        }
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
        case XML_COMMENT_NODE: {
            xml_node_private_t *nodepriv = NULL;
            nodepriv = calloc(1, sizeof(xml_node_private_t));
            nodepriv->check = XML_NODE_PRIVATE_MAGIC;
            /* Flags will be reset if necessary when tracking is enabled */
            nodepriv->flags |= (xpf_dirty|xpf_created);
            node->_private = nodepriv;
            if (nodepriv && pcmk__tracking_xml_changes(node, FALSE)) {
                /* XML_ELEMENT_NODE doesn't get picked up here, node->doc is
                 * not hooked up at the point we are called
                 */
                pcmk__set_xml_flag(node, xpf_dirty);
                __xml_node_dirty(node);
            }
            break;
        }
        case XML_TEXT_NODE:
        case XML_DTD_NODE:
        case XML_CDATA_SECTION_NODE:
            break;
        default:
            /* Ignore */
            crm_trace("Ignoring %p %d", node, node->type);
            CRM_LOG_ASSERT(node->type == XML_ELEMENT_NODE);
            break;
    }
}

void
xml_track_changes(xmlNode * xml, const char *user, xmlNode *acl_source, bool enforce_acls) 
{
    xml_accept_changes(xml);
    crm_trace("Tracking changes%s to %p", enforce_acls?" with ACLs":"", xml);
    pcmk__set_xml_flag(xml, xpf_tracking);
    if(enforce_acls) {
        if(acl_source == NULL) {
            acl_source = xml;
        }
        pcmk__set_xml_flag(xml, xpf_acl_enabled);
        pcmk__unpack_acl(acl_source, xml, user);
        pcmk__apply_acl(xml);
    }
}

bool xml_tracking_changes(xmlNode * xml)
{
    if (xml == NULL || xml->doc == NULL || xml->doc->_private == NULL) {
        return FALSE;

    } else if (is_set(((xml_doc_private_t *) xml->doc->_private)->flags,
                      xpf_tracking)) {
        return TRUE;
    }
    return FALSE;
}

bool xml_document_dirty(xmlNode *xml) 
{
    if(xml != NULL && xml->doc && xml->doc->_private) {
        xml_doc_private_t *docpriv = xml->doc->_private;

        return is_set(docpriv->flags, xpf_dirty);
    }
    return FALSE;
}

/*
<diff format="2.0">
  <version>
    <source admin_epoch="1" epoch="2" num_updates="3"/>
    <target admin_epoch="1" epoch="3" num_updates="0"/>
  </version>
  <change operation="add" xpath="/cib/configuration/nodes">
    <node id="node2" uname="node2" description="foo"/>
  </change>
  <change operation="add" xpath="/cib/configuration/nodes/node[node2]">
    <instance_attributes id="nodes-node"><!-- NOTE: can be a full tree -->
      <nvpair id="nodes-node2-ram" name="ram" value="1024M"/>
    </instance_attributes>
  </change>
  <change operation="update" xpath="/cib/configuration/nodes[@id='node2']">
    <change-list>
      <change-attr operation="set" name="type" value="member"/>
      <change-attr operation="unset" name="description"/>
    </change-list>
    <change-result>
      <node id="node2" uname="node2" type="member"/><!-- NOTE: not recursive -->
    </change-result>
  </change>
  <change operation="delete" xpath="/cib/configuration/nodes/node[@id='node3'] /">
  <change operation="update" xpath="/cib/configuration/resources/group[@id='g1']">
    <change-list>
      <change-attr operation="set" name="description" value="some grabage here"/>
    </change-list>
    <change-result>
      <group id="g1" description="some grabage here"/><!-- NOTE: not recursive -->
    </change-result>
  </change>
  <change operation="update" xpath="/cib/status/node_state[@id='node2]/lrm[@id='node2']/lrm_resources/lrm_resource[@id='Fence']">
    <change-list>
      <change-attr operation="set" name="oper" value="member"/>
      <change-attr operation="set" name="operation_key" value="Fence_start_0"/>
      <change-attr operation="set" name="operation" value="start"/>
      <change-attr operation="set" name="transition-key" value="2:-1:0:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"/>
      <change-attr operation="set" name="transition-magic" value="0:0;2:-1:0:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"/>
      <change-attr operation="set" name="call-id" value="2"/>
      <change-attr operation="set" name="rc-code" value="0"/>
    </change-list>
    <change-result>
      <lrm_rsc_op id="Fence_last_0" operation_key="Fence_start_0" operation="start" crm-debug-origin="crm_simulate"  transition-key="2:-1:0:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" transition-magic="0:0;2:-1:0:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" call-id="2" rc-code="0" op-status="0" interval="0" exec-time="0" queue-time="0" op-digest="f2317cad3d54cec5d7d7aa7d0bf35cf8"/>
    </change-result>
  </change>
</diff>
 */
static int __xml_offset(xmlNode *xml) 
{
    int position = 0;
    xmlNode *cIter = NULL;

    for (cIter = xml; cIter->prev; cIter = cIter->prev) {
        xml_node_private_t *nodepriv = ((xmlNode*)cIter->prev)->_private;

        if (is_not_set(nodepriv->flags, xpf_skip)) {
            position++;
        }
    }

    return position;
}

static int __xml_offset_no_deletions(xmlNode *xml) 
{
    int position = 0;
    xmlNode *cIter = NULL;

    for (cIter = xml; cIter->prev; cIter = cIter->prev) {
        xml_node_private_t *nodepriv = ((xmlNode*)cIter->prev)->_private;

        if (is_not_set(nodepriv->flags, xpf_deleted)) {
            position++;
        }
    }

    return position;
}

static void
__xml_build_changes(xmlNode * xml, xmlNode *patchset)
{
    xmlNode *cIter = NULL;
    xmlAttr *pIter = NULL;
    xmlNode *change = NULL;
    xml_node_private_t *nodepriv = xml->_private;

    if (patchset && is_set(nodepriv->flags, xpf_created)) {
        int offset = 0;
        char buffer[XML_BUFFER_SIZE];

        if (pcmk__element_xpath(NULL, xml->parent, buffer, offset,
                                sizeof(buffer)) > 0) {
            int position = __xml_offset_no_deletions(xml);

            change = create_xml_node(patchset, XML_DIFF_CHANGE);

            crm_xml_add(change, XML_DIFF_OP, "create");
            crm_xml_add(change, XML_DIFF_PATH, buffer);
            crm_xml_add_int(change, XML_DIFF_POSITION, position);
            add_node_copy(change, xml);
        }

        return;
    }

    for (pIter = pcmk__first_xml_attr(xml); pIter != NULL; pIter = pIter->next) {
        xmlNode *attr = NULL;

        nodepriv = pIter->_private;
        if (is_not_set(nodepriv->flags, xpf_deleted)
                && is_not_set(nodepriv->flags, xpf_dirty)) {
            continue;
        }

        if(change == NULL) {
            int offset = 0;
            char buffer[XML_BUFFER_SIZE];

            if (pcmk__element_xpath(NULL, xml, buffer, offset,
                                    sizeof(buffer)) > 0) {
                change = create_xml_node(patchset, XML_DIFF_CHANGE);

                crm_xml_add(change, XML_DIFF_OP, "modify");
                crm_xml_add(change, XML_DIFF_PATH, buffer);

                change = create_xml_node(change, XML_DIFF_LIST);
            }
        }

        attr = create_xml_node(change, XML_DIFF_ATTR);

        crm_xml_add(attr, XML_NVPAIR_ATTR_NAME, (const char *)pIter->name);
        if (nodepriv->flags & xpf_deleted) {
            crm_xml_add(attr, XML_DIFF_OP, "unset");

        } else {
            const char *value = crm_element_value(xml, (const char *)pIter->name);

            crm_xml_add(attr, XML_DIFF_OP, "set");
            crm_xml_add(attr, XML_NVPAIR_ATTR_VALUE, value);
        }
    }

    if(change) {
        xmlNode *result = NULL;

        change = create_xml_node(change->parent, XML_DIFF_RESULT);
        result = create_xml_node(change, (const char *)xml->name);

        for (pIter = pcmk__first_xml_attr(xml); pIter != NULL; pIter = pIter->next) {
            const char *value = crm_element_value(xml, (const char *)pIter->name);

            nodepriv = pIter->_private;
            if (is_not_set(nodepriv->flags, xpf_deleted)) {
                crm_xml_add(result, (const char *)pIter->name, value);
            }
        }
    }

    for (cIter = __xml_first_child(xml); cIter != NULL; cIter = __xml_next(cIter)) {
        __xml_build_changes(cIter, patchset);
    }

    nodepriv = xml->_private;
    if (patchset && is_set(nodepriv->flags, xpf_moved)) {
        int offset = 0;
        char buffer[XML_BUFFER_SIZE];

        crm_trace("%s.%s moved to position %d", xml->name, ID(xml), __xml_offset(xml));
        if (pcmk__element_xpath(NULL, xml, buffer, offset,
                                sizeof(buffer)) > 0) {
            change = create_xml_node(patchset, XML_DIFF_CHANGE);

            crm_xml_add(change, XML_DIFF_OP, "move");
            crm_xml_add(change, XML_DIFF_PATH, buffer);
            crm_xml_add_int(change, XML_DIFF_POSITION, __xml_offset_no_deletions(xml));
        }
    }
}

static void
__xml_accept_changes(xmlNode * xml)
{
    xmlNode *cIter = NULL;
    xmlAttr *pIter = NULL;
    xml_node_private_t *nodepriv = xml->_private;

    nodepriv->flags = xpf_none;
    pIter = pcmk__first_xml_attr(xml);

    while (pIter != NULL) {
        const xmlChar *name = pIter->name;

        nodepriv = pIter->_private;
        pIter = pIter->next;

        if (nodepriv->flags & xpf_deleted) {
            xml_remove_prop(xml, (const char *)name);

        } else {
            nodepriv->flags = xpf_none;
        }
    }

    for (cIter = __xml_first_child(xml); cIter != NULL; cIter = __xml_next(cIter)) {
        __xml_accept_changes(cIter);
    }
}

static bool
is_config_change(xmlNode *xml)
{
    GListPtr gIter = NULL;
    xmlNode *config = first_named_child(xml, XML_CIB_TAG_CONFIGURATION);
    xml_node_private_t *nodepriv = NULL;
    xml_doc_private_t *docpriv;

    if (config) {
        nodepriv = config->_private;
    }
    if (nodepriv && is_set(nodepriv->flags, xpf_dirty)) {
        return TRUE;
    }

    if (xml->doc && xml->doc->_private) {
        docpriv = xml->doc->_private;
        for (gIter = docpriv->deleted_objs; gIter; gIter = gIter->next) {
            xml_deleted_obj_t *deleted_obj = gIter->data;

            if(strstr(deleted_obj->path, "/"XML_TAG_CIB"/"XML_CIB_TAG_CONFIGURATION) != NULL) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static void
xml_repair_v1_diff(xmlNode * last, xmlNode * next, xmlNode * local_diff, gboolean changed)
{
    int lpc = 0;
    xmlNode *cib = NULL;
    xmlNode *diff_child = NULL;

    const char *tag = NULL;

    const char *vfields[] = {
        XML_ATTR_GENERATION_ADMIN,
        XML_ATTR_GENERATION,
        XML_ATTR_NUMUPDATES,
    };

    if (local_diff == NULL) {
        crm_trace("Nothing to do");
        return;
    }

    tag = "diff-removed";
    diff_child = find_xml_node(local_diff, tag, FALSE);
    if (diff_child == NULL) {
        diff_child = create_xml_node(local_diff, tag);
    }

    tag = XML_TAG_CIB;
    cib = find_xml_node(diff_child, tag, FALSE);
    if (cib == NULL) {
        cib = create_xml_node(diff_child, tag);
    }

    for(lpc = 0; last && lpc < DIMOF(vfields); lpc++){
        const char *value = crm_element_value(last, vfields[lpc]);

        crm_xml_add(diff_child, vfields[lpc], value);
        if(changed || lpc == 2) {
            crm_xml_add(cib, vfields[lpc], value);
        }
    }

    tag = "diff-added";
    diff_child = find_xml_node(local_diff, tag, FALSE);
    if (diff_child == NULL) {
        diff_child = create_xml_node(local_diff, tag);
    }

    tag = XML_TAG_CIB;
    cib = find_xml_node(diff_child, tag, FALSE);
    if (cib == NULL) {
        cib = create_xml_node(diff_child, tag);
    }

    for(lpc = 0; next && lpc < DIMOF(vfields); lpc++){
        const char *value = crm_element_value(next, vfields[lpc]);

        crm_xml_add(diff_child, vfields[lpc], value);
    }

    if (next) {
        xmlAttrPtr xIter = NULL;

        for (xIter = next->properties; xIter; xIter = xIter->next) {
            const char *p_name = (const char *)xIter->name;
            const char *p_value = crm_element_value(next, p_name);

            xmlSetProp(cib, (pcmkXmlStr) p_name, (pcmkXmlStr) p_value);
        }
    }

    crm_log_xml_explicit(local_diff, "Repaired-diff");
}

static xmlNode *
xml_create_patchset_v1(xmlNode *source, xmlNode *target, bool config, bool suppress)
{
    xmlNode *patchset = diff_xml_object(source, target, suppress);

    if(patchset) {
        CRM_LOG_ASSERT(xml_document_dirty(target));
        xml_repair_v1_diff(source, target, patchset, config);
        crm_xml_add(patchset, "format", "1");
    }
    return patchset;
}

static xmlNode *
xml_create_patchset_v2(xmlNode *source, xmlNode *target)
{
    int lpc = 0;
    GListPtr gIter = NULL;
    xml_doc_private_t *docpriv;

    xmlNode *v = NULL;
    xmlNode *version = NULL;
    xmlNode *patchset = NULL;
    const char *vfields[] = {
        XML_ATTR_GENERATION_ADMIN,
        XML_ATTR_GENERATION,
        XML_ATTR_NUMUPDATES,
    };

    CRM_ASSERT(target);
    if(xml_document_dirty(target) == FALSE) {
        return NULL;
    }

    CRM_ASSERT(target->doc);
    docpriv = target->doc->_private;

    patchset = create_xml_node(NULL, XML_TAG_DIFF);
    crm_xml_add_int(patchset, "format", 2);

    version = create_xml_node(patchset, XML_DIFF_VERSION);

    v = create_xml_node(version, XML_DIFF_VSOURCE);
    for(lpc = 0; lpc < DIMOF(vfields); lpc++){
        const char *value = crm_element_value(source, vfields[lpc]);

        if(value == NULL) {
            value = "1";
        }
        crm_xml_add(v, vfields[lpc], value);
    }

    v = create_xml_node(version, XML_DIFF_VTARGET);
    for(lpc = 0; lpc < DIMOF(vfields); lpc++){
        const char *value = crm_element_value(target, vfields[lpc]);

        if(value == NULL) {
            value = "1";
        }
        crm_xml_add(v, vfields[lpc], value);
    }

    for(gIter = docpriv->deleted_objs; gIter; gIter = gIter->next) {
        xml_deleted_obj_t *deleted_obj = gIter->data;
        xmlNode *change = create_xml_node(patchset, XML_DIFF_CHANGE);

        crm_xml_add(change, XML_DIFF_OP, "delete");
        crm_xml_add(change, XML_DIFF_PATH, deleted_obj->path);
        if (deleted_obj->position >= 0) {
            crm_xml_add_int(change, XML_DIFF_POSITION, deleted_obj->position);
        }
    }

    __xml_build_changes(target, patchset);
    return patchset;
}

xmlNode *
xml_create_patchset(int format, xmlNode *source, xmlNode *target, bool *config_changed, bool manage_version)
{
    int counter = 0;
    bool config = FALSE;
    xmlNode *patch = NULL;
    const char *version = crm_element_value(source, XML_ATTR_CRM_VERSION);

    xml_acl_disable(target);
    if(xml_document_dirty(target) == FALSE) {
        crm_trace("No change %d", format);
        return NULL; /* No change */
    }

    config = is_config_change(target);
    if(config_changed) {
        *config_changed = config;
    }

    if(manage_version && config) {
        crm_trace("Config changed %d", format);
        crm_xml_add(target, XML_ATTR_NUMUPDATES, "0");

        crm_element_value_int(target, XML_ATTR_GENERATION, &counter);
        crm_xml_add_int(target, XML_ATTR_GENERATION, counter+1);

    } else if(manage_version) {
        crm_element_value_int(target, XML_ATTR_NUMUPDATES, &counter);
        crm_trace("Status changed %d - %d %s", format, counter, crm_element_value(source, XML_ATTR_NUMUPDATES));
        crm_xml_add_int(target, XML_ATTR_NUMUPDATES, counter+1);
    }

    if(format == 0) {
        if (compare_version("3.0.8", version) < 0) {
            format = 2;

        } else {
            format = 1;
        }
        crm_trace("Using patch format %d for version: %s", format, version);
    }

    switch(format) {
        case 1:
            patch = xml_create_patchset_v1(source, target, config, FALSE);
            break;
        case 2:
            patch = xml_create_patchset_v2(source, target);
            break;
        default:
            crm_err("Unknown patch format: %d", format);
            return NULL;
    }

    return patch;
}

void
patchset_process_digest(xmlNode *patch, xmlNode *source, xmlNode *target, bool with_digest)
{
    int format = 1;
    const char *version = NULL;
    char *digest = NULL;

    if (patch == NULL || source == NULL || target == NULL) {
        return;
    }

    /* NOTE: We should always call xml_accept_changes() before calculating digest. */
    /* Otherwise, with an on-tracking dirty target, we could get a wrong digest. */
    CRM_LOG_ASSERT(xml_document_dirty(target) == FALSE);

    crm_element_value_int(patch, "format", &format);
    if (format > 1 && with_digest == FALSE) {
        return;
    }

    version = crm_element_value(source, XML_ATTR_CRM_VERSION);
    digest = calculate_xml_versioned_digest(target, FALSE, TRUE, version);

    crm_xml_add(patch, XML_ATTR_DIGEST, digest);
    free(digest);

    return;
}

static void
__xml_log_element(int log_level, const char *file, const char *function, int line,
                  const char *prefix, xmlNode * data, int depth, int options);

void
xml_log_patchset(uint8_t log_level, const char *function, xmlNode * patchset)
{
    int format = 1;
    xmlNode *child = NULL;
    xmlNode *added = NULL;
    xmlNode *removed = NULL;
    gboolean is_first = TRUE;

    int add[] = { 0, 0, 0 };
    int del[] = { 0, 0, 0 };

    const char *fmt = NULL;
    const char *digest = NULL;
    int options = xml_log_option_formatted;

    static struct qb_log_callsite *patchset_cs = NULL;

    if (patchset_cs == NULL) {
        patchset_cs = qb_log_callsite_get(function, __FILE__, "xml-patchset", log_level, __LINE__, 0);
    }

    if (patchset == NULL) {
        crm_trace("Empty patch");
        return;

    } else if (log_level == 0) {
        /* Log to stdout */
    } else if (crm_is_callsite_active(patchset_cs, log_level, 0) == FALSE) {
        return;
    }

    xml_patch_versions(patchset, add, del);
    fmt = crm_element_value(patchset, "format");
    digest = crm_element_value(patchset, XML_ATTR_DIGEST);

    if (add[2] != del[2] || add[1] != del[1] || add[0] != del[0]) {
        do_crm_log_alias(log_level, __FILE__, function, __LINE__,
                         "Diff: --- %d.%d.%d %s", del[0], del[1], del[2], fmt);
        do_crm_log_alias(log_level, __FILE__, function, __LINE__,
                         "Diff: +++ %d.%d.%d %s", add[0], add[1], add[2], digest);

    } else if (patchset != NULL && (add[0] || add[1] || add[2])) {
        do_crm_log_alias(log_level, __FILE__, function, __LINE__, 
                         "%s: Local-only Change: %d.%d.%d", function ? function : "",
                         add[0], add[1], add[2]);
    }

    crm_element_value_int(patchset, "format", &format);
    if(format == 2) {
        xmlNode *change = NULL;

        for (change = __xml_first_child(patchset); change != NULL; change = __xml_next(change)) {
            const char *op = crm_element_value(change, XML_DIFF_OP);
            const char *xpath = crm_element_value(change, XML_DIFF_PATH);

            if(op == NULL) {
            } else if(strcmp(op, "create") == 0) {
                int lpc = 0, max = 0;
                char *prefix = crm_strdup_printf("++ %s: ", xpath);

                max = strlen(prefix);
                __xml_log_element(log_level, __FILE__, function, __LINE__, prefix, change->children,
                                  0, xml_log_option_formatted|xml_log_option_open);

                for(lpc = 2; lpc < max; lpc++) {
                    prefix[lpc] = ' ';
                }

                __xml_log_element(log_level, __FILE__, function, __LINE__, prefix, change->children,
                                  0, xml_log_option_formatted|xml_log_option_close|xml_log_option_children);
                free(prefix);

            } else if(strcmp(op, "move") == 0) {
                do_crm_log_alias(log_level, __FILE__, function, __LINE__, "+~ %s moved to offset %s", xpath, crm_element_value(change, XML_DIFF_POSITION));

            } else if(strcmp(op, "modify") == 0) {
                xmlNode *clist = first_named_child(change, XML_DIFF_LIST);
                char buffer_set[XML_BUFFER_SIZE];
                char buffer_unset[XML_BUFFER_SIZE];
                int o_set = 0;
                int o_unset = 0;

                buffer_set[0] = 0;
                buffer_unset[0] = 0;
                for (child = __xml_first_child(clist); child != NULL; child = __xml_next(child)) {
                    const char *name = crm_element_value(child, "name");

                    op = crm_element_value(child, XML_DIFF_OP);
                    if(op == NULL) {
                    } else if(strcmp(op, "set") == 0) {
                        const char *value = crm_element_value(child, "value");

                        if(o_set > 0) {
                            o_set += snprintf(buffer_set + o_set, XML_BUFFER_SIZE - o_set, ", ");
                        }
                        o_set += snprintf(buffer_set + o_set, XML_BUFFER_SIZE - o_set, "@%s=%s", name, value);

                    } else if(strcmp(op, "unset") == 0) {
                        if(o_unset > 0) {
                            o_unset += snprintf(buffer_unset + o_unset, XML_BUFFER_SIZE - o_unset, ", ");
                        }
                        o_unset += snprintf(buffer_unset + o_unset, XML_BUFFER_SIZE - o_unset, "@%s", name);
                    }
                }
                if(o_set) {
                    do_crm_log_alias(log_level, __FILE__, function, __LINE__, "+  %s:  %s", xpath, buffer_set);
                }
                if(o_unset) {
                    do_crm_log_alias(log_level, __FILE__, function, __LINE__, "-- %s:  %s", xpath, buffer_unset);
                }

            } else if(strcmp(op, "delete") == 0) {
                int position = -1;

                crm_element_value_int(change, XML_DIFF_POSITION, &position);
                if (position >= 0) {
                    do_crm_log_alias(log_level, __FILE__, function, __LINE__, "-- %s (%d)", xpath, position);

                } else {
                    do_crm_log_alias(log_level, __FILE__, function, __LINE__, "-- %s", xpath);
                }
            }
        }
        return;
    }

    if (log_level < LOG_DEBUG || function == NULL) {
        options |= xml_log_option_diff_short;
    }

    removed = find_xml_node(patchset, "diff-removed", FALSE);
    for (child = __xml_first_child(removed); child != NULL; child = __xml_next(child)) {
        log_data_element(log_level, __FILE__, function, __LINE__, "- ", child, 0,
                         options | xml_log_option_diff_minus);
        if (is_first) {
            is_first = FALSE;
        } else {
            do_crm_log_alias(log_level, __FILE__, function, __LINE__, " --- ");
        }
    }

    is_first = TRUE;
    added = find_xml_node(patchset, "diff-added", FALSE);
    for (child = __xml_first_child(added); child != NULL; child = __xml_next(child)) {
        log_data_element(log_level, __FILE__, function, __LINE__, "+ ", child, 0,
                         options | xml_log_option_diff_plus);
        if (is_first) {
            is_first = FALSE;
        } else {
            do_crm_log_alias(log_level, __FILE__, function, __LINE__, " +++ ");
        }
    }
}

void
xml_log_changes(uint8_t log_level, const char *function, xmlNode * xml)
{
    GListPtr gIter = NULL;
    xml_doc_private_t *docpriv;

    CRM_ASSERT(xml);
    CRM_ASSERT(xml->doc);

    docpriv = xml->doc->_private;
    if (is_not_set(docpriv->flags, xpf_dirty)) {
        return;
    }

    for (gIter = docpriv->deleted_objs; gIter; gIter = gIter->next) {
        xml_deleted_obj_t *deleted_obj = gIter->data;

        if (deleted_obj->position >= 0) {
            do_crm_log_alias(log_level, __FILE__, function, __LINE__, "-- %s (%d)",
                             deleted_obj->path, deleted_obj->position);

        } else {
            do_crm_log_alias(log_level, __FILE__, function, __LINE__, "-- %s",
                             deleted_obj->path);
        }
    }

    log_data_element(log_level, __FILE__, function, __LINE__, "+ ", xml, 0,
                     xml_log_option_formatted|xml_log_option_dirty_add);
}

void
xml_accept_changes(xmlNode * xml)
{
    xmlNode *top = NULL;
    xml_doc_private_t *docpriv;

    if(xml == NULL) {
        return;
    }

    crm_trace("Accepting changes to %p", xml);
    docpriv = xml->doc->_private;
    top = xmlDocGetRootElement(xml->doc);

    if (docpriv != NULL) {
        __xml_doc_private_clean(docpriv);
    } else {
        docpriv = __xml_doc_private_create(
                      (xml_doc_private_t **) &xml->doc->_private);
    }

    if (is_not_set(docpriv->flags, xpf_dirty)) {
        docpriv->flags = xpf_none;
        return;
    }

    docpriv->flags = xpf_none;
    __xml_accept_changes(top);
}

static xmlNode *
find_element(xmlNode *haystack, xmlNode *needle, gboolean exact)
{
    CRM_CHECK(needle != NULL, return NULL);
    return (needle->type == XML_COMMENT_NODE)?
           find_xml_comment(haystack, needle, exact)
           : find_entity(haystack, crm_element_name(needle), ID(needle));
}

/* Simplified version for applying v1-style XML patches */
static void
__subtract_xml_object(xmlNode * target, xmlNode * patch)
{
    xmlNode *patch_child = NULL;
    xmlNode *cIter = NULL;
    xmlAttrPtr xIter = NULL;

    char *id = NULL;
    const char *name = NULL;
    const char *value = NULL;

    if (target == NULL || patch == NULL) {
        return;
    }

    if (target->type == XML_COMMENT_NODE) {
        gboolean dummy;

        subtract_xml_comment(target->parent, target, patch, &dummy);
    }

    name = crm_element_name(target);
    CRM_CHECK(name != NULL, return);
    CRM_CHECK(safe_str_eq(crm_element_name(target), crm_element_name(patch)), return);
    CRM_CHECK(safe_str_eq(ID(target), ID(patch)), return);

    /* check for XML_DIFF_MARKER in a child */
    id = crm_element_value_copy(target, XML_ATTR_ID);
    value = crm_element_value(patch, XML_DIFF_MARKER);
    if (value != NULL && strcmp(value, "removed:top") == 0) {
        crm_trace("We are the root of the deletion: %s.id=%s", name, id);
        free_xml(target);
        free(id);
        return;
    }

    for (xIter = pcmk__first_xml_attr(patch); xIter != NULL; xIter = xIter->next) {
        const char *p_name = (const char *)xIter->name;

        /* Removing and then restoring the id field would change the ordering of properties */
        if (safe_str_neq(p_name, XML_ATTR_ID)) {
            xml_remove_prop(target, p_name);
        }
    }

    /* changes to child objects */
    cIter = __xml_first_child(target);
    while (cIter) {
        xmlNode *target_child = cIter;

        cIter = __xml_next(cIter);
        patch_child = find_element(patch, target_child, FALSE);
        __subtract_xml_object(target_child, patch_child);
    }
    free(id);
}

static void
__add_xml_object(xmlNode * parent, xmlNode * target, xmlNode * patch)
{
    xmlNode *patch_child = NULL;
    xmlNode *target_child = NULL;
    xmlAttrPtr xIter = NULL;

    const char *id = NULL;
    const char *name = NULL;
    const char *value = NULL;

    if (patch == NULL) {
        return;
    } else if (parent == NULL && target == NULL) {
        return;
    }

    /* check for XML_DIFF_MARKER in a child */
    value = crm_element_value(patch, XML_DIFF_MARKER);
    if (target == NULL
        && value != NULL
        && strcmp(value, "added:top") == 0) {
        id = ID(patch);
        name = crm_element_name(patch);
        crm_trace("We are the root of the addition: %s.id=%s", name, id);
        add_node_copy(parent, patch);
        return;

    } else if(target == NULL) {
        id = ID(patch);
        name = crm_element_name(patch);
        crm_err("Could not locate: %s.id=%s", name, id);
        return;
    }

    if (target->type == XML_COMMENT_NODE) {
        add_xml_comment(parent, target, patch);
    }

    name = crm_element_name(target);
    CRM_CHECK(name != NULL, return);
    CRM_CHECK(safe_str_eq(crm_element_name(target), crm_element_name(patch)), return);
    CRM_CHECK(safe_str_eq(ID(target), ID(patch)), return);

    for (xIter = pcmk__first_xml_attr(patch); xIter != NULL; xIter = xIter->next) {
        const char *p_name = (const char *)xIter->name;
        const char *p_value = crm_element_value(patch, p_name);

        xml_remove_prop(target, p_name); /* Preserve the patch order */
        crm_xml_add(target, p_name, p_value);
    }

    /* changes to child objects */
    for (patch_child = __xml_first_child(patch); patch_child != NULL;
         patch_child = __xml_next(patch_child)) {

        target_child = find_element(target, patch_child, FALSE);
        __add_xml_object(target, target_child, patch_child);
    }
}

/*!
 * \internal
 * \brief Find additions or removals in a patch set
 *
 * \param[in]     patchset   XML of patch
 * \param[in]     format     Patch version
 * \param[in]     added      TRUE if looking for additions, FALSE if removals
 * \param[in,out] patch_node Will be set to node if found
 *
 * \return TRUE if format is valid, FALSE if invalid
 */
static bool
find_patch_xml_node(xmlNode *patchset, int format, bool added,
                    xmlNode **patch_node)
{
    xmlNode *cib_node;
    const char *label;

    switch(format) {
        case 1:
            label = added? "diff-added" : "diff-removed";
            *patch_node = find_xml_node(patchset, label, FALSE);
            cib_node = find_xml_node(*patch_node, "cib", FALSE);
            if (cib_node != NULL) {
                *patch_node = cib_node;
            }
            break;
        case 2:
            label = added? "target" : "source";
            *patch_node = find_xml_node(patchset, "version", FALSE);
            *patch_node = find_xml_node(*patch_node, label, FALSE);
            break;
        default:
            crm_warn("Unknown patch format: %d", format);
            *patch_node = NULL;
            return FALSE;
    }
    return TRUE;
}

bool xml_patch_versions(xmlNode *patchset, int add[3], int del[3])
{
    int lpc = 0;
    int format = 1;
    xmlNode *tmp = NULL;

    const char *vfields[] = {
        XML_ATTR_GENERATION_ADMIN,
        XML_ATTR_GENERATION,
        XML_ATTR_NUMUPDATES,
    };


    crm_element_value_int(patchset, "format", &format);

    /* Process removals */
    if (!find_patch_xml_node(patchset, format, FALSE, &tmp)) {
        return -EINVAL;
    }
    if (tmp) {
        for(lpc = 0; lpc < DIMOF(vfields); lpc++) {
            crm_element_value_int(tmp, vfields[lpc], &(del[lpc]));
            crm_trace("Got %d for del[%s]", del[lpc], vfields[lpc]);
        }
    }

    /* Process additions */
    if (!find_patch_xml_node(patchset, format, TRUE, &tmp)) {
        return -EINVAL;
    }
    if (tmp) {
        for(lpc = 0; lpc < DIMOF(vfields); lpc++) {
            crm_element_value_int(tmp, vfields[lpc], &(add[lpc]));
            crm_trace("Got %d for add[%s]", add[lpc], vfields[lpc]);
        }
    }

    return pcmk_ok;
}

static int
xml_patch_version_check(xmlNode *xml, xmlNode *patchset, int format) 
{
    int lpc = 0;
    bool changed = FALSE;

    int this[] = { 0, 0, 0 };
    int add[] = { 0, 0, 0 };
    int del[] = { 0, 0, 0 };

    const char *vfields[] = {
        XML_ATTR_GENERATION_ADMIN,
        XML_ATTR_GENERATION,
        XML_ATTR_NUMUPDATES,
    };

    for(lpc = 0; lpc < DIMOF(vfields); lpc++) {
        crm_element_value_int(xml, vfields[lpc], &(this[lpc]));
        crm_trace("Got %d for this[%s]", this[lpc], vfields[lpc]);
        if (this[lpc] < 0) {
            this[lpc] = 0;
        }
    }

    /* Set some defaults in case nothing is present */
    add[0] = this[0];
    add[1] = this[1];
    add[2] = this[2] + 1;
    for(lpc = 0; lpc < DIMOF(vfields); lpc++) {
        del[lpc] = this[lpc];
    }

    xml_patch_versions(patchset, add, del);

    for(lpc = 0; lpc < DIMOF(vfields); lpc++) {
        if(this[lpc] < del[lpc]) {
            crm_debug("Current %s is too low (%d.%d.%d < %d.%d.%d --> %d.%d.%d)", vfields[lpc],
                      this[0], this[1], this[2], del[0], del[1], del[2], add[0], add[1], add[2]);
            return -pcmk_err_diff_resync;

        } else if(this[lpc] > del[lpc]) {
            crm_info("Current %s is too high (%d.%d.%d > %d.%d.%d --> %d.%d.%d) %p", vfields[lpc],
                     this[0], this[1], this[2], del[0], del[1], del[2], add[0], add[1], add[2], patchset);
            crm_log_xml_info(patchset, "OldPatch");
            return -pcmk_err_old_data;
        }
    }

    for(lpc = 0; lpc < DIMOF(vfields); lpc++) {
        if(add[lpc] > del[lpc]) {
            changed = TRUE;
        }
    }

    if(changed == FALSE) {
        crm_notice("Versions did not change in patch %d.%d.%d", add[0], add[1], add[2]);
        return -pcmk_err_old_data;
    }

    crm_debug("Can apply patch %d.%d.%d to %d.%d.%d",
             add[0], add[1], add[2], this[0], this[1], this[2]);
    return pcmk_ok;
}

static int
xml_apply_patchset_v1(xmlNode *xml, xmlNode *patchset)
{
    int rc = pcmk_ok;
    int root_nodes_seen = 0;

    xmlNode *child_diff = NULL;
    xmlNode *added = find_xml_node(patchset, "diff-added", FALSE);
    xmlNode *removed = find_xml_node(patchset, "diff-removed", FALSE);
    xmlNode *old = copy_xml(xml);

    crm_trace("Subtraction Phase");
    for (child_diff = __xml_first_child(removed); child_diff != NULL;
         child_diff = __xml_next(child_diff)) {
        CRM_CHECK(root_nodes_seen == 0, rc = FALSE);
        if (root_nodes_seen == 0) {
            __subtract_xml_object(xml, child_diff);
        }
        root_nodes_seen++;
    }

    if (root_nodes_seen > 1) {
        crm_err("(-) Diffs cannot contain more than one change set... saw %d", root_nodes_seen);
        rc = -ENOTUNIQ;
    }

    root_nodes_seen = 0;
    crm_trace("Addition Phase");
    if (rc == pcmk_ok) {
        xmlNode *child_diff = NULL;

        for (child_diff = __xml_first_child(added); child_diff != NULL;
             child_diff = __xml_next(child_diff)) {
            CRM_CHECK(root_nodes_seen == 0, rc = FALSE);
            if (root_nodes_seen == 0) {
                __add_xml_object(NULL, xml, child_diff);
            }
            root_nodes_seen++;
        }
    }

    if (root_nodes_seen > 1) {
        crm_err("(+) Diffs cannot contain more than one change set... saw %d", root_nodes_seen);
        rc = -ENOTUNIQ;
    }

    purge_diff_markers(xml);       /* Purge prior to checking the digest */

    free_xml(old);
    return rc;
}

static xmlNode *
__first_xml_child_match(xmlNode *parent, const char *name, const char *id, int position)
{
    xmlNode *cIter = NULL;

    for (cIter = __xml_first_child(parent); cIter != NULL; cIter = __xml_next(cIter)) {
        if(strcmp((const char *)cIter->name, name) != 0) {
            continue;
        } else if(id) {
            const char *cid = ID(cIter);
            if(cid == NULL || strcmp(cid, id) != 0) {
                continue;
            }
        }

        /* The "position" makes sense only for XML comments for now */
        if (cIter->type == XML_COMMENT_NODE
            && position >= 0
            && __xml_offset(cIter) != position) {
            continue;
        }

        return cIter;
    }
    return NULL;
}

/*!
 * \internal
 * \brief Simplified, more efficient alternative to get_xpath_object()
 *
 * \param[in] top              Root of XML to search
 * \param[in] key              Search xpath
 * \param[in] target_position  If deleting, where to delete
 *
 * \return XML child matching xpath if found, NULL otherwise
 *
 * \note This only works on simplified xpaths found in v2 patchset diffs,
 *       i.e. the only allowed search predicate is [@id='XXX'].
 */
static xmlNode *
__xml_find_path(xmlNode *top, const char *key, int target_position)
{
    xmlNode *target = (xmlNode*) top->doc;
    const char *current = key;
    char *section;
    char *remainder;
    char *id;
    char *tag;
    char *path = NULL;
    int rc;
    size_t key_len;

    CRM_CHECK(key != NULL, return NULL);
    key_len = strlen(key);

    /* These are scanned from key after a slash, so they can't be bigger
     * than key_len - 1 characters plus a null terminator.
     */

    remainder = calloc(key_len, sizeof(char));
    CRM_ASSERT(remainder != NULL);

    section = calloc(key_len, sizeof(char));
    CRM_ASSERT(section != NULL);

    id = calloc(key_len, sizeof(char));
    CRM_ASSERT(id != NULL);

    tag = calloc(key_len, sizeof(char));
    CRM_ASSERT(tag != NULL);

    do {
        // Look for /NEXT_COMPONENT/REMAINING_COMPONENTS
        rc = sscanf(current, "/%[^/]%s", section, remainder);
        if (rc > 0) {
            // Separate FIRST_COMPONENT into TAG[@id='ID']
            int f = sscanf(section, "%[^[][@id='%[^']", tag, id);
            int current_position = -1;

            /* The target position is for the final component tag, so only use
             * it if there is nothing left to search after this component.
             */
            if ((rc == 1) && (target_position >= 0)) {
                current_position = target_position;
            }

            switch (f) {
                case 1:
                    target = __first_xml_child_match(target, tag, NULL, current_position);
                    break;
                case 2:
                    target = __first_xml_child_match(target, tag, id, current_position);
                    break;
                default:
                    // This should not be possible
                    target = NULL;
                    break;
            }
            current = remainder;
        }

    // Continue if something remains to search, and we've matched so far
    } while ((rc == 2) && target);

    if (target) {
        crm_trace("Found %s for %s",
                  (path = (char *) xmlGetNodePath(target)), key);
        free(path);
    } else {
        crm_debug("No match for %s", key);
    }

    free(remainder);
    free(section);
    free(tag);
    free(id);
    return target;
}

typedef struct xml_change_obj_s {
    xmlNode *change;
    xmlNode *match;
} xml_change_obj_t;

static gint
sort_change_obj_by_position(gconstpointer a, gconstpointer b)
{
    const xml_change_obj_t *change_obj_a = a;
    const xml_change_obj_t *change_obj_b = b;
    int position_a = -1;
    int position_b = -1;

    crm_element_value_int(change_obj_a->change, XML_DIFF_POSITION, &position_a);
    crm_element_value_int(change_obj_b->change, XML_DIFF_POSITION, &position_b);

    if (position_a < position_b) {
        return -1;

    } else if (position_a > position_b) {
        return 1;
    }

    return 0;
}

static int
xml_apply_patchset_v2(xmlNode *xml, xmlNode *patchset)
{
    int rc = pcmk_ok;
    xmlNode *change = NULL;
    GListPtr change_objs = NULL;
    GListPtr gIter = NULL;

    for (change = __xml_first_child(patchset); change != NULL; change = __xml_next(change)) {
        xmlNode *match = NULL;
        const char *op = crm_element_value(change, XML_DIFF_OP);
        const char *xpath = crm_element_value(change, XML_DIFF_PATH);
        int position = -1;

        if(op == NULL) {
            continue;
        }

        crm_trace("Processing %s %s", change->name, op);

        // "delete" changes for XML comments are generated with "position"
        if(strcmp(op, "delete") == 0) {
            crm_element_value_int(change, XML_DIFF_POSITION, &position);
        }
        match = __xml_find_path(xml, xpath, position);
        crm_trace("Performing %s on %s with %p", op, xpath, match);

        if(match == NULL && strcmp(op, "delete") == 0) {
            crm_debug("No %s match for %s in %p", op, xpath, xml->doc);
            continue;

        } else if(match == NULL) {
            crm_err("No %s match for %s in %p", op, xpath, xml->doc);
            rc = -pcmk_err_diff_failed;
            continue;

        } else if (strcmp(op, "create") == 0 || strcmp(op, "move") == 0) {
            // Delay the adding of a "create" object
            xml_change_obj_t *change_obj = calloc(1, sizeof(xml_change_obj_t));

            CRM_ASSERT(change_obj != NULL);

            change_obj->change = change;
            change_obj->match = match;

            change_objs = g_list_append(change_objs, change_obj);

            if (strcmp(op, "move") == 0) {
                // Temporarily put the "move" object after the last sibling
                if (match->parent != NULL && match->parent->last != NULL) {
                    xmlAddNextSibling(match->parent->last, match);
                }
            }

        } else if(strcmp(op, "delete") == 0) {
            free_xml(match);

        } else if(strcmp(op, "modify") == 0) {
            xmlAttr *pIter = pcmk__first_xml_attr(match);
            xmlNode *attrs = __xml_first_child(first_named_child(change, XML_DIFF_RESULT));

            if(attrs == NULL) {
                rc = -ENOMSG;
                continue;
            }
            while(pIter != NULL) {
                const char *name = (const char *)pIter->name;

                pIter = pIter->next;
                xml_remove_prop(match, name);
            }

            for (pIter = pcmk__first_xml_attr(attrs); pIter != NULL; pIter = pIter->next) {
                const char *name = (const char *)pIter->name;
                const char *value = crm_element_value(attrs, name);

                crm_xml_add(match, name, value);
            }

        } else {
            crm_err("Unknown operation: %s", op);
            rc = -pcmk_err_diff_failed;
        }
    }

    // Changes should be generated in the right order. Double checking.
    change_objs = g_list_sort(change_objs, sort_change_obj_by_position);

    for (gIter = change_objs; gIter; gIter = gIter->next) {
        xml_change_obj_t *change_obj = gIter->data;
        xmlNode *match = change_obj->match;
        const char *op = NULL;
        const char *xpath = NULL;

        change = change_obj->change;

        op = crm_element_value(change, XML_DIFF_OP);
        xpath = crm_element_value(change, XML_DIFF_PATH);

        crm_trace("Continue performing %s on %s with %p", op, xpath, match);

        if(strcmp(op, "create") == 0) {
            int position = 0;
            xmlNode *child = NULL;
            xmlNode *match_child = NULL;

            match_child = match->children;
            crm_element_value_int(change, XML_DIFF_POSITION, &position);

            while(match_child && position != __xml_offset(match_child)) {
                match_child = match_child->next;
            }

            child = xmlDocCopyNode(change->children, match->doc, 1);
            if(match_child) {
                crm_trace("Adding %s at position %d", child->name, position);
                xmlAddPrevSibling(match_child, child);

            } else if(match->last) { /* Add to the end */
                crm_trace("Adding %s at position %d (end)", child->name, position);
                xmlAddNextSibling(match->last, child);

            } else {
                crm_trace("Adding %s at position %d (first)", child->name, position);
                CRM_LOG_ASSERT(position == 0);
                xmlAddChild(match, child);
            }
            crm_node_created(child);

        } else if(strcmp(op, "move") == 0) {
            int position = 0;

            crm_element_value_int(change, XML_DIFF_POSITION, &position);
            if(position != __xml_offset(match)) {
                xmlNode *match_child = NULL;
                int p = position;

                if(p > __xml_offset(match)) {
                    p++; /* Skip ourselves */
                }

                CRM_ASSERT(match->parent != NULL);
                match_child = match->parent->children;

                while(match_child && p != __xml_offset(match_child)) {
                    match_child = match_child->next;
                }

                crm_trace("Moving %s to position %d (was %d, prev %p, %s %p)",
                         match->name, position, __xml_offset(match), match->prev,
                         match_child?"next":"last", match_child?match_child:match->parent->last);

                if(match_child) {
                    xmlAddPrevSibling(match_child, match);

                } else {
                    CRM_ASSERT(match->parent->last != NULL);
                    xmlAddNextSibling(match->parent->last, match);
                }

            } else {
                crm_trace("%s is already in position %d", match->name, position);
            }

            if(position != __xml_offset(match)) {
                crm_err("Moved %s.%s to position %d instead of %d (%p)",
                        match->name, ID(match), __xml_offset(match), position, match->prev);
                rc = -pcmk_err_diff_failed;
            }
        }
    }

    g_list_free_full(change_objs, free);
    return rc;
}

int
xml_apply_patchset(xmlNode *xml, xmlNode *patchset, bool check_version) 
{
    int format = 1;
    int rc = pcmk_ok;
    xmlNode *old = NULL;
    const char *digest = crm_element_value(patchset, XML_ATTR_DIGEST);

    if(patchset == NULL) {
        return rc;
    }

    xml_log_patchset(LOG_TRACE, __FUNCTION__, patchset);

    crm_element_value_int(patchset, "format", &format);
    if(check_version) {
        rc = xml_patch_version_check(xml, patchset, format);
        if(rc != pcmk_ok) {
            return rc;
        }
    }

    if(digest) {
        /* Make it available for logging if the result doesn't have the expected digest */
        old = copy_xml(xml);
    }

    if(rc == pcmk_ok) {
        switch(format) {
            case 1:
                rc = xml_apply_patchset_v1(xml, patchset);
                break;
            case 2:
                rc = xml_apply_patchset_v2(xml, patchset);
                break;
            default:
                crm_err("Unknown patch format: %d", format);
                rc = -EINVAL;
        }
    }

    if(rc == pcmk_ok && digest) {
        static struct qb_log_callsite *digest_cs = NULL;

        char *new_digest = NULL;
        char *version = crm_element_value_copy(xml, XML_ATTR_CRM_VERSION);

        if (digest_cs == NULL) {
            digest_cs =
                qb_log_callsite_get(__func__, __FILE__, "diff-digest", LOG_TRACE, __LINE__,
                                    crm_trace_nonlog);
        }

        new_digest = calculate_xml_versioned_digest(xml, FALSE, TRUE, version);
        if (safe_str_neq(new_digest, digest)) {
            crm_info("v%d digest mis-match: expected %s, calculated %s", format, digest, new_digest);
            rc = -pcmk_err_diff_failed;

            if (digest_cs && digest_cs->targets) {
                save_xml_to_file(old,     "PatchDigest:input", NULL);
                save_xml_to_file(xml,     "PatchDigest:result", NULL);
                save_xml_to_file(patchset,"PatchDigest:diff", NULL);

            } else {
                crm_trace("%p %.6x", digest_cs, digest_cs ? digest_cs->targets : 0);
            }

        } else {
            crm_trace("v%d digest matched: expected %s, calculated %s", format, digest, new_digest);
        }
        free(new_digest);
        free(version);
    }
    free_xml(old);
    return rc;
}

xmlNode *
find_xml_node(xmlNode * root, const char *search_path, gboolean must_find)
{
    xmlNode *a_child = NULL;
    const char *name = "NULL";

    if (root != NULL) {
        name = crm_element_name(root);
    }

    if (search_path == NULL) {
        crm_warn("Will never find <NULL>");
        return NULL;
    }

    for (a_child = __xml_first_child(root); a_child != NULL; a_child = __xml_next(a_child)) {
        if (strcmp((const char *)a_child->name, search_path) == 0) {
/* 		crm_trace("returning node (%s).", crm_element_name(a_child)); */
            return a_child;
        }
    }

    if (must_find) {
        crm_warn("Could not find %s in %s.", search_path, name);
    } else if (root != NULL) {
        crm_trace("Could not find %s in %s.", search_path, name);
    } else {
        crm_trace("Could not find %s in <NULL>.", search_path);
    }

    return NULL;
}

/* As the name suggests, the perfect match is required for both node
   name and fully specified attribute, otherwise, when attribute not
   specified, the outcome is the first node matching on the name. */
static xmlNode *
find_entity_by_attr_or_just_name(xmlNode *parent, const char *node_name,
                                 const char *attr_n, const char *attr_v)
{
    xmlNode *child;

    /* ensure attr_v specified when attr_n is */
    CRM_CHECK(attr_n == NULL || attr_v != NULL, return NULL);

    for (child = __xml_first_child(parent); child != NULL; child = __xml_next(child)) {
        /* XXX uncertain if the first check is strictly necessary here */
        if (node_name == NULL || !strcmp((const char *) child->name, node_name)) {
            if (attr_n == NULL
                    || crm_str_eq(crm_element_value(child, attr_n), attr_v, TRUE)) {
                return child;
            }
        }
    }

    crm_trace("node <%s%s%s%s%s> not found in %s", crm_str(node_name),
              attr_n ? " " : "",
              attr_n ? attr_n : "",
              attr_n ? "=" : "",
              attr_n ? attr_v : "",
              crm_element_name(parent));

    return NULL;
}

xmlNode *
find_entity(xmlNode *parent, const char *node_name, const char *id)
{
    return find_entity_by_attr_or_just_name(parent, node_name,
                                            (id == NULL) ? id : XML_ATTR_ID, id);
}

void
copy_in_properties(xmlNode * target, xmlNode * src)
{
    if (src == NULL) {
        crm_warn("No node to copy properties from");

    } else if (target == NULL) {
        crm_err("No node to copy properties into");

    } else {
        xmlAttrPtr pIter = NULL;

        for (pIter = pcmk__first_xml_attr(src); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = pcmk__xml_attr_value(pIter);

            expand_plus_plus(target, p_name, p_value);
        }
    }

    return;
}

void
fix_plus_plus_recursive(xmlNode * target)
{
    /* TODO: Remove recursion and use xpath searches for value++ */
    xmlNode *child = NULL;
    xmlAttrPtr pIter = NULL;

    for (pIter = pcmk__first_xml_attr(target); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;
        const char *p_value = pcmk__xml_attr_value(pIter);

        expand_plus_plus(target, p_name, p_value);
    }
    for (child = __xml_first_child(target); child != NULL; child = __xml_next(child)) {
        fix_plus_plus_recursive(child);
    }
}

void
expand_plus_plus(xmlNode * target, const char *name, const char *value)
{
    int offset = 1;
    int name_len = 0;
    int int_value = 0;
    int value_len = 0;

    const char *old_value = NULL;

    if (value == NULL || name == NULL) {
        return;
    }

    old_value = crm_element_value(target, name);

    if (old_value == NULL) {
        /* if no previous value, set unexpanded */
        goto set_unexpanded;

    } else if (strstr(value, name) != value) {
        goto set_unexpanded;
    }

    name_len = strlen(name);
    value_len = strlen(value);
    if (value_len < (name_len + 2)
        || value[name_len] != '+' || (value[name_len + 1] != '+' && value[name_len + 1] != '=')) {
        goto set_unexpanded;
    }

    /* if we are expanding ourselves,
     * then no previous value was set and leave int_value as 0
     */
    if (old_value != value) {
        int_value = char2score(old_value);
    }

    if (value[name_len + 1] != '+') {
        const char *offset_s = value + (name_len + 2);

        offset = char2score(offset_s);
    }
    int_value += offset;

    if (int_value > INFINITY) {
        int_value = (int)INFINITY;
    }

    crm_xml_add_int(target, name, int_value);
    return;

  set_unexpanded:
    if (old_value == value) {
        /* the old value is already set, nothing to do */
        return;
    }
    crm_xml_add(target, name, value);
    return;
}

xmlDoc *
getDocPtr(xmlNode * node)
{
    xmlDoc *doc = NULL;

    CRM_CHECK(node != NULL, return NULL);

    doc = node->doc;
    if (doc == NULL) {
        doc = xmlNewDoc((pcmkXmlStr) "1.0");
        xmlDocSetRootElement(doc, node);
        xmlSetTreeDoc(node, doc);
    }
    return doc;
}

xmlNode *
add_node_copy(xmlNode * parent, xmlNode * src_node)
{
    xmlNode *child = NULL;
    xmlDoc *doc = getDocPtr(parent);

    CRM_CHECK(src_node != NULL, return NULL);

    child = xmlDocCopyNode(src_node, doc, 1);
    xmlAddChild(parent, child);
    crm_node_created(child);
    return child;
}

int
add_node_nocopy(xmlNode * parent, const char *name, xmlNode * child)
{
    add_node_copy(parent, child);
    free_xml(child);
    return 1;
}

xmlNode *
create_xml_node(xmlNode * parent, const char *name)
{
    xmlDoc *doc = NULL;
    xmlNode *node = NULL;

    if (name == NULL || name[0] == 0) {
        CRM_CHECK(name != NULL && name[0] == 0, return NULL);
        return NULL;
    }

    if (parent == NULL) {
        doc = xmlNewDoc((pcmkXmlStr) "1.0");
        node = xmlNewDocRawNode(doc, NULL, (pcmkXmlStr) name, NULL);
        xmlDocSetRootElement(doc, node);

    } else {
        doc = getDocPtr(parent);
        node = xmlNewDocRawNode(doc, NULL, (pcmkXmlStr) name, NULL);
        xmlAddChild(parent, node);
    }
    crm_node_created(node);
    return node;
}

xmlNode *
pcmk_create_xml_text_node(xmlNode * parent, const char *name, const char *content)
{
    xmlNode *node = create_xml_node(parent, name);

    if (node != NULL) {
        xmlNodeSetContent(node, (pcmkXmlStr) content);
    }

    return node;
}

xmlNode *
pcmk_create_html_node(xmlNode * parent, const char *element_name, const char *id,
                      const char *class_name, const char *text)
{
    xmlNode *node = pcmk_create_xml_text_node(parent, element_name, text);

    if (class_name != NULL) {
        xmlSetProp(node, (pcmkXmlStr) "class", (pcmkXmlStr) class_name);
    }

    if (id != NULL) {
        xmlSetProp(node, (pcmkXmlStr) "id", (pcmkXmlStr) id);
    }

    return node;
}

int
pcmk__element_xpath(const char *prefix, xmlNode *xml, char *buffer,
                    int offset, size_t buffer_size)
{
    const char *id = ID(xml);

    if(offset == 0 && prefix == NULL && xml->parent) {
        offset = pcmk__element_xpath(NULL, xml->parent, buffer, offset,
                                     buffer_size);
    }

    if(id) {
        offset += snprintf(buffer + offset, buffer_size - offset,
                           "/%s[@id='%s']", (const char *) xml->name, id);
    } else if(xml->name) {
        offset += snprintf(buffer + offset, buffer_size - offset,
                           "/%s", (const char *) xml->name);
    }

    return offset;
}

char *
xml_get_path(xmlNode *xml)
{
    int offset = 0;
    char buffer[XML_BUFFER_SIZE];

    if (pcmk__element_xpath(NULL, xml, buffer, offset, sizeof(buffer)) > 0) {
        return strdup(buffer);
    }
    return NULL;
}

/*!
 * Free an XML element and all of its children, removing it from its parent
 *
 * \param[in] xml  XML element to free
 */
void
pcmk_free_xml_subtree(xmlNode *xml)
{
    xmlUnlinkNode(xml); // Detaches from parent and siblings
    xmlFreeNode(xml);   // Frees
}

static void
free_xml_with_position(xmlNode * child, int position)
{
    if (child != NULL) {
        xmlNode *top = NULL;
        xmlDoc *doc = child->doc;
        xml_node_private_t *nodepriv = child->_private;

        if (doc != NULL) {
            top = xmlDocGetRootElement(doc);
        }

        if (doc != NULL && top == child) {
            /* Free everything */
            xmlFreeDoc(doc);

        } else if (pcmk__check_acl(child, NULL, xpf_acl_write) == FALSE) {
            int offset = 0;
            char buffer[XML_BUFFER_SIZE];

            pcmk__element_xpath(NULL, child, buffer, offset, sizeof(buffer));
            crm_trace("Cannot remove %s %x", buffer, nodepriv->flags);
            return;

        } else {
            if (doc && pcmk__tracking_xml_changes(child, FALSE)
                && is_not_set(nodepriv->flags, xpf_created)) {
                int offset = 0;
                char buffer[XML_BUFFER_SIZE];

                if (pcmk__element_xpath(NULL, child, buffer, offset,
                                        sizeof(buffer)) > 0) {
                    xml_deleted_obj_t *deleted_obj = calloc(1, sizeof(xml_deleted_obj_t));
                    xml_doc_private_t *docpriv;

                    crm_trace("Deleting %s %p from %p", buffer, child, doc);

                    deleted_obj->path = strdup(buffer);

                    deleted_obj->position = -1;
                    /* Record the "position" only for XML comments for now */
                    if (child->type == XML_COMMENT_NODE) {
                        if (position >= 0) {
                            deleted_obj->position = position;

                        } else {
                            deleted_obj->position = __xml_offset(child);
                        }
                    }

                    docpriv = doc->_private;
                    docpriv->deleted_objs = g_list_append(docpriv->deleted_objs,
                                                          deleted_obj);
                    pcmk__set_xml_flag(child, xpf_dirty);
                }
            }
            pcmk_free_xml_subtree(child);
        }
    }
}


void
free_xml(xmlNode * child)
{
    free_xml_with_position(child, -1);
}

xmlNode *
copy_xml(xmlNode * src)
{
    xmlDoc *doc = xmlNewDoc((pcmkXmlStr) "1.0");
    xmlNode *copy = xmlDocCopyNode(src, doc, 1);

    xmlDocSetRootElement(doc, copy);
    xmlSetTreeDoc(copy, doc);
    return copy;
}

static void
crm_xml_err(void *ctx, const char *fmt, ...)
G_GNUC_PRINTF(2, 3);

static void
crm_xml_err(void *ctx, const char *fmt, ...)
{
    va_list ap;
    static struct qb_log_callsite *xml_error_cs = NULL;

    if (xml_error_cs == NULL) {
        xml_error_cs = qb_log_callsite_get(
            __func__, __FILE__, "xml library error", LOG_TRACE, __LINE__, crm_trace_nonlog);
    }

    va_start(ap, fmt);
    if (xml_error_cs && xml_error_cs->targets) {
        CRM_XML_LOG_BASE(LOG_ERR, TRUE,
                         crm_abort(__FILE__, __PRETTY_FUNCTION__, __LINE__, "xml library error",
                                   TRUE, TRUE),
                         "XML Error: ", fmt, ap);
    } else {
        CRM_XML_LOG_BASE(LOG_ERR, TRUE, 0, "XML Error: ", fmt, ap);
    }
    va_end(ap);
}

xmlNode *
string2xml(const char *input)
{
    xmlNode *xml = NULL;
    xmlDocPtr output = NULL;
    xmlParserCtxtPtr ctxt = NULL;
    xmlErrorPtr last_error = NULL;

    if (input == NULL) {
        crm_err("Can't parse NULL input");
        return NULL;
    }

    /* create a parser context */
    ctxt = xmlNewParserCtxt();
    CRM_CHECK(ctxt != NULL, return NULL);

    xmlCtxtResetLastError(ctxt);
    xmlSetGenericErrorFunc(ctxt, crm_xml_err);
    output = xmlCtxtReadDoc(ctxt, (pcmkXmlStr) input, NULL, NULL,
                            PCMK__XML_PARSE_OPTS);
    if (output) {
        xml = xmlDocGetRootElement(output);
    }
    last_error = xmlCtxtGetLastError(ctxt);
    if (last_error && last_error->code != XML_ERR_OK) {
        /* crm_abort(__FILE__,__FUNCTION__,__LINE__, "last_error->code != XML_ERR_OK", TRUE, TRUE); */
        /*
         * http://xmlsoft.org/html/libxml-xmlerror.html#xmlErrorLevel
         * http://xmlsoft.org/html/libxml-xmlerror.html#xmlParserErrors
         */
        crm_warn("Parsing failed (domain=%d, level=%d, code=%d): %s",
                 last_error->domain, last_error->level, last_error->code, last_error->message);

        if (last_error->code == XML_ERR_DOCUMENT_EMPTY) {
            CRM_LOG_ASSERT("Cannot parse an empty string");

        } else if (last_error->code != XML_ERR_DOCUMENT_END) {
            crm_err("Couldn't%s parse %d chars: %s", xml ? " fully" : "", (int)strlen(input),
                    input);
            if (xml != NULL) {
                crm_log_xml_err(xml, "Partial");
            }

        } else {
            int len = strlen(input);
            int lpc = 0;

            while(lpc < len) {
                crm_warn("Parse error[+%.3d]: %.80s", lpc, input+lpc);
                lpc += 80;
            }

            CRM_LOG_ASSERT("String parsing error");
        }
    }

    xmlFreeParserCtxt(ctxt);
    return xml;
}

xmlNode *
stdin2xml(void)
{
    size_t data_length = 0;
    size_t read_chars = 0;

    char *xml_buffer = NULL;
    xmlNode *xml_obj = NULL;

    do {
        xml_buffer = realloc_safe(xml_buffer, data_length + XML_BUFFER_SIZE);
        read_chars = fread(xml_buffer + data_length, 1, XML_BUFFER_SIZE, stdin);
        data_length += read_chars;
    } while (read_chars == XML_BUFFER_SIZE);

    if (data_length == 0) {
        crm_warn("No XML supplied on stdin");
        free(xml_buffer);
        return NULL;
    }

    xml_buffer[data_length] = '\0';
    xml_obj = string2xml(xml_buffer);
    free(xml_buffer);

    crm_log_xml_trace(xml_obj, "Created fragment");
    return xml_obj;
}

static char *
decompress_file(const char *filename)
{
    char *buffer = NULL;

#if HAVE_BZLIB_H
    int rc = 0;
    size_t length = 0, read_len = 0;

    BZFILE *bz_file = NULL;
    FILE *input = fopen(filename, "r");

    if (input == NULL) {
        crm_perror(LOG_ERR, "Could not open %s for reading", filename);
        return NULL;
    }

    bz_file = BZ2_bzReadOpen(&rc, input, 0, 0, NULL, 0);
    if (rc != BZ_OK) {
        crm_err("Could not prepare to read compressed %s: %s "
                CRM_XS " bzerror=%d", filename, bz2_strerror(rc), rc);
        BZ2_bzReadClose(&rc, bz_file);
        return NULL;
    }

    rc = BZ_OK;
    while (rc == BZ_OK) {
        buffer = realloc_safe(buffer, XML_BUFFER_SIZE + length + 1);
        read_len = BZ2_bzRead(&rc, bz_file, buffer + length, XML_BUFFER_SIZE);

        crm_trace("Read %ld bytes from file: %d", (long)read_len, rc);

        if (rc == BZ_OK || rc == BZ_STREAM_END) {
            length += read_len;
        }
    }

    buffer[length] = '\0';

    if (rc != BZ_STREAM_END) {
        crm_err("Could not read compressed %s: %s "
                CRM_XS " bzerror=%d", filename, bz2_strerror(rc), rc);
        free(buffer);
        buffer = NULL;
    }

    BZ2_bzReadClose(&rc, bz_file);
    fclose(input);

#else
    crm_err("Could not read compressed %s: not built with bzlib support",
            filename);
#endif
    return buffer;
}

void
strip_text_nodes(xmlNode * xml)
{
    xmlNode *iter = xml->children;

    while (iter) {
        xmlNode *next = iter->next;

        switch (iter->type) {
            case XML_TEXT_NODE:
                /* Remove it */
                pcmk_free_xml_subtree(iter);
                break;

            case XML_ELEMENT_NODE:
                /* Search it */
                strip_text_nodes(iter);
                break;

            default:
                /* Leave it */
                break;
        }

        iter = next;
    }
}

xmlNode *
filename2xml(const char *filename)
{
    xmlNode *xml = NULL;
    xmlDocPtr output = NULL;
    gboolean uncompressed = TRUE;
    xmlParserCtxtPtr ctxt = NULL;
    xmlErrorPtr last_error = NULL;

    /* create a parser context */
    ctxt = xmlNewParserCtxt();
    CRM_CHECK(ctxt != NULL, return NULL);

    xmlCtxtResetLastError(ctxt);
    xmlSetGenericErrorFunc(ctxt, crm_xml_err);

    if (filename) {
        uncompressed = !crm_ends_with_ext(filename, ".bz2");
    }

    if (filename == NULL) {
        /* STDIN_FILENO == fileno(stdin) */
        output = xmlCtxtReadFd(ctxt, STDIN_FILENO, "unknown.xml", NULL,
                               PCMK__XML_PARSE_OPTS);

    } else if (uncompressed) {
        output = xmlCtxtReadFile(ctxt, filename, NULL, PCMK__XML_PARSE_OPTS);

    } else {
        char *input = decompress_file(filename);

        output = xmlCtxtReadDoc(ctxt, (pcmkXmlStr) input, NULL, NULL,
                                PCMK__XML_PARSE_OPTS);
        free(input);
    }

    if (output && (xml = xmlDocGetRootElement(output))) {
        strip_text_nodes(xml);
    }

    last_error = xmlCtxtGetLastError(ctxt);
    if (last_error && last_error->code != XML_ERR_OK) {
        /* crm_abort(__FILE__,__FUNCTION__,__LINE__, "last_error->code != XML_ERR_OK", TRUE, TRUE); */
        /*
         * http://xmlsoft.org/html/libxml-xmlerror.html#xmlErrorLevel
         * http://xmlsoft.org/html/libxml-xmlerror.html#xmlParserErrors
         */
        crm_err("Parsing failed (domain=%d, level=%d, code=%d): %s",
                last_error->domain, last_error->level, last_error->code, last_error->message);

        if (last_error && last_error->code != XML_ERR_OK) {
            crm_err("Couldn't%s parse %s", xml ? " fully" : "", filename);
            if (xml != NULL) {
                crm_log_xml_err(xml, "Partial");
            }
        }
    }

    xmlFreeParserCtxt(ctxt);
    return xml;
}

/*!
 * \internal
 * \brief Add a "last written" attribute to an XML node, set to current time
 *
 * \param[in] xml_node XML node to get attribute
 *
 * \return Value that was set, or NULL on error
 */
const char *
crm_xml_add_last_written(xmlNode *xml_node)
{
    const char *now_str = crm_now_string(NULL);
    return crm_xml_add(xml_node, XML_CIB_ATTR_WRITTEN,
                       now_str ? now_str : "Could not determine current time");
}

/*!
 * \brief Sanitize a string so it is usable as an XML ID
 *
 * \param[in,out] id  String to sanitize
 */
void
crm_xml_sanitize_id(char *id)
{
    char *c;

    for (c = id; *c; ++c) {
        /* @TODO Sanitize more comprehensively */
        switch (*c) {
            case ':':
            case '#':
                *c = '.';
        }
    }
}

/*!
 * \brief Set the ID of an XML element using a format
 *
 * \param[in,out] xml  XML element
 * \param[in]     fmt  printf-style format
 * \param[in]     ...  any arguments required by format
 */
void
crm_xml_set_id(xmlNode *xml, const char *format, ...)
{
    va_list ap;
    int len = 0;
    char *id = NULL;

    /* equivalent to crm_strdup_printf() */
    va_start(ap, format);
    len = vasprintf(&id, format, ap);
    va_end(ap);
    CRM_ASSERT(len > 0);

    crm_xml_sanitize_id(id);
    crm_xml_add(xml, XML_ATTR_ID, id);
    free(id);
}

/*!
 * \internal
 * \brief Write XML to a file stream
 *
 * \param[in] xml_node  XML to write
 * \param[in] filename  Name of file being written (for logging only)
 * \param[in] stream    Open file stream corresponding to filename
 * \param[in] compress  Whether to compress XML before writing
 *
 * \return Number of bytes written on success, -errno otherwise
 */
static int
write_xml_stream(xmlNode * xml_node, const char *filename, FILE * stream, gboolean compress)
{
    int res = 0;
    char *buffer = NULL;
    unsigned int out = 0;

    crm_log_xml_trace(xml_node, "writing");

    buffer = dump_xml_formatted(xml_node);
    CRM_CHECK(buffer && strlen(buffer),
              crm_log_xml_warn(xml_node, "formatting failed");
              res = -pcmk_err_generic;
              goto bail);

    if (compress) {
#if HAVE_BZLIB_H
        int rc = BZ_OK;
        unsigned int in = 0;
        BZFILE *bz_file = NULL;

        bz_file = BZ2_bzWriteOpen(&rc, stream, 5, 0, 30);
        if (rc != BZ_OK) {
            crm_warn("Not compressing %s: could not prepare file stream: %s "
                     CRM_XS " bzerror=%d", filename, bz2_strerror(rc), rc);
        } else {
            BZ2_bzWrite(&rc, bz_file, buffer, strlen(buffer));
            if (rc != BZ_OK) {
                crm_warn("Not compressing %s: could not compress data: %s "
                         CRM_XS " bzerror=%d errno=%d",
                         filename, bz2_strerror(rc), rc, errno);
            }
        }

        if (rc == BZ_OK) {
            BZ2_bzWriteClose(&rc, bz_file, 0, &in, &out);
            if (rc != BZ_OK) {
                crm_warn("Not compressing %s: could not write compressed data: %s "
                         CRM_XS " bzerror=%d errno=%d",
                         filename, bz2_strerror(rc), rc, errno);
                out = 0; // retry without compression
            } else {
                res = (int) out;
                crm_trace("Compressed XML for %s from %u bytes to %u",
                          filename, in, out);
            }
        }
#else
        crm_warn("Not compressing %s: not built with bzlib support", filename);
#endif
    }

    if (out == 0) {
        res = fprintf(stream, "%s", buffer);
        if (res < 0) {
            res = -errno;
            crm_perror(LOG_ERR, "writing %s", filename);
            goto bail;
        }
    }

  bail:

    if (fflush(stream) != 0) {
        res = -errno;
        crm_perror(LOG_ERR, "flushing %s", filename);
    }

    /* Don't report error if the file does not support synchronization */
    if (fsync(fileno(stream)) < 0 && errno != EROFS  && errno != EINVAL) {
        res = -errno;
        crm_perror(LOG_ERR, "synchronizing %s", filename);
    }

    fclose(stream);

    crm_trace("Saved %d bytes%s to %s as XML",
              res, ((out > 0)? " (compressed)" : ""), filename);
    free(buffer);

    return res;
}

/*!
 * \brief Write XML to a file descriptor
 *
 * \param[in] xml_node  XML to write
 * \param[in] filename  Name of file being written (for logging only)
 * \param[in] fd        Open file descriptor corresponding to filename
 * \param[in] compress  Whether to compress XML before writing
 *
 * \return Number of bytes written on success, -errno otherwise
 */
int
write_xml_fd(xmlNode * xml_node, const char *filename, int fd, gboolean compress)
{
    FILE *stream = NULL;

    CRM_CHECK(xml_node && (fd > 0), return -EINVAL);
    stream = fdopen(fd, "w");
    if (stream == NULL) {
        return -errno;
    }
    return write_xml_stream(xml_node, filename, stream, compress);
}

/*!
 * \brief Write XML to a file
 *
 * \param[in] xml_node  XML to write
 * \param[in] filename  Name of file to write
 * \param[in] compress  Whether to compress XML before writing
 *
 * \return Number of bytes written on success, -errno otherwise
 */
int
write_xml_file(xmlNode * xml_node, const char *filename, gboolean compress)
{
    FILE *stream = NULL;

    CRM_CHECK(xml_node && filename, return -EINVAL);
    stream = fopen(filename, "w");
    if (stream == NULL) {
        return -errno;
    }
    return write_xml_stream(xml_node, filename, stream, compress);
}

xmlNode *
get_message_xml(xmlNode * msg, const char *field)
{
    xmlNode *tmp = first_named_child(msg, field);

    return __xml_first_child(tmp);
}

gboolean
add_message_xml(xmlNode * msg, const char *field, xmlNode * xml)
{
    xmlNode *holder = create_xml_node(msg, field);

    add_node_copy(holder, xml);
    return TRUE;
}

static char *
crm_xml_escape_shuffle(char *text, int start, int *length, const char *replace)
{
    int lpc;
    int offset = strlen(replace) - 1;   /* We have space for 1 char already */

    *length += offset;
    text = realloc_safe(text, *length);

    for (lpc = (*length) - 1; lpc > (start + offset); lpc--) {
        text[lpc] = text[lpc - offset];
    }

    memcpy(text + start, replace, offset + 1);
    return text;
}

char *
crm_xml_escape(const char *text)
{
    int index;
    int changes = 0;
    int length = 1 + strlen(text);
    char *copy = strdup(text);

    /*
     * When xmlCtxtReadDoc() parses &lt; and friends in a
     * value, it converts them to their human readable
     * form.
     *
     * If one uses xmlNodeDump() to convert it back to a
     * string, all is well, because special characters are
     * converted back to their escape sequences.
     *
     * However xmlNodeDump() is randomly dog slow, even with the same
     * input. So we need to replicate the escaping in our custom
     * version so that the result can be re-parsed by xmlCtxtReadDoc()
     * when necessary.
     */

    for (index = 0; index < length; index++) {
        switch (copy[index]) {
            case 0:
                break;
            case '<':
                copy = crm_xml_escape_shuffle(copy, index, &length, "&lt;");
                changes++;
                break;
            case '>':
                copy = crm_xml_escape_shuffle(copy, index, &length, "&gt;");
                changes++;
                break;
            case '"':
                copy = crm_xml_escape_shuffle(copy, index, &length, "&quot;");
                changes++;
                break;
            case '\'':
                copy = crm_xml_escape_shuffle(copy, index, &length, "&apos;");
                changes++;
                break;
            case '&':
                copy = crm_xml_escape_shuffle(copy, index, &length, "&amp;");
                changes++;
                break;
            case '\t':
                /* Might as well just expand to a few spaces... */
                copy = crm_xml_escape_shuffle(copy, index, &length, "    ");
                changes++;
                break;
            case '\n':
                /* crm_trace("Convert: \\%.3o", copy[index]); */
                copy = crm_xml_escape_shuffle(copy, index, &length, "\\n");
                changes++;
                break;
            case '\r':
                copy = crm_xml_escape_shuffle(copy, index, &length, "\\r");
                changes++;
                break;
                /* For debugging...
            case '\\':
                crm_trace("Passthrough: \\%c", copy[index+1]);
                break;
                */
            default:
                /* Check for and replace non-printing characters with their octal equivalent */
                if(copy[index] < ' ' || copy[index] > '~') {
                    char *replace = crm_strdup_printf("\\%.3o", copy[index]);

                    /* crm_trace("Convert to octal: \\%.3o", copy[index]); */
                    copy = crm_xml_escape_shuffle(copy, index, &length, replace);
                    free(replace);
                    changes++;
                }
        }
    }

    if (changes) {
        crm_trace("Dumped '%s'", copy);
    }
    return copy;
}

static inline void
dump_xml_attr(xmlAttrPtr attr, int options, char **buffer, int *offset, int *max)
{
    char *p_value = NULL;
    const char *p_name = NULL;
    xml_node_private_t *nodepriv = NULL;

    CRM_ASSERT(buffer != NULL);
    if (attr == NULL || attr->children == NULL) {
        return;
    }

    nodepriv = attr->_private;
    if (nodepriv && is_set(nodepriv->flags, xpf_deleted)) {
        return;
    }

    p_name = (const char *)attr->name;
    p_value = crm_xml_escape((const char *)attr->children->content);
    buffer_print(*buffer, *max, *offset, " %s=\"%s\"", p_name, p_value);
    free(p_value);
}

static void
__xml_log_element(int log_level, const char *file, const char *function, int line,
                  const char *prefix, xmlNode * data, int depth, int options)
{
    int max = 0;
    int offset = 0;
    const char *name = NULL;
    const char *hidden = NULL;

    xmlNode *child = NULL;
    xmlAttrPtr pIter = NULL;

    if(data == NULL) {
        return;
    }

    name = crm_element_name(data);

    if(is_set(options, xml_log_option_open)) {
        char *buffer = NULL;

        insert_prefix(options, &buffer, &offset, &max, depth);

        if (data->type == XML_COMMENT_NODE) {
            buffer_print(buffer, max, offset, "<!--%s-->", data->content);

        } else {
            buffer_print(buffer, max, offset, "<%s", name);

            hidden = crm_element_value(data, "hidden");
            for (pIter = pcmk__first_xml_attr(data); pIter != NULL; pIter = pIter->next) {
                xml_node_private_t *nodepriv = pIter->_private;
                const char *p_name = (const char *)pIter->name;
                const char *p_value = pcmk__xml_attr_value(pIter);
                char *p_copy = NULL;

                if(is_set(nodepriv->flags, xpf_deleted)) {
                    continue;
                } else if ((is_set(options, xml_log_option_diff_plus)
                     || is_set(options, xml_log_option_diff_minus))
                    && strcmp(XML_DIFF_MARKER, p_name) == 0) {
                    continue;

                } else if (hidden != NULL && p_name[0] != 0 && strstr(hidden, p_name) != NULL) {
                    p_copy = strdup("*****");

                } else {
                    p_copy = crm_xml_escape(p_value);
                }

                buffer_print(buffer, max, offset, " %s=\"%s\"", p_name, p_copy);
                free(p_copy);
            }

            if(xml_has_children(data) == FALSE) {
                buffer_print(buffer, max, offset, "/>");

            } else if(is_set(options, xml_log_option_children)) {
                buffer_print(buffer, max, offset, ">");

            } else {
                buffer_print(buffer, max, offset, "/>");
            }
        }

        do_crm_log_alias(log_level, file, function, line, "%s %s", prefix, buffer);
        free(buffer);
    }

    if(data->type == XML_COMMENT_NODE) {
        return;

    } else if(xml_has_children(data) == FALSE) {
        return;

    } else if(is_set(options, xml_log_option_children)) {
        offset = 0;
        max = 0;

        for (child = __xml_first_child(data); child != NULL; child = __xml_next(child)) {
            __xml_log_element(log_level, file, function, line, prefix, child, depth + 1, options|xml_log_option_open|xml_log_option_close);
        }
    }

    if(is_set(options, xml_log_option_close)) {
        char *buffer = NULL;

        insert_prefix(options, &buffer, &offset, &max, depth);
        buffer_print(buffer, max, offset, "</%s>", name);

        do_crm_log_alias(log_level, file, function, line, "%s %s", prefix, buffer);
        free(buffer);
    }
}

static void
__xml_log_change_element(int log_level, const char *file, const char *function, int line,
                         const char *prefix, xmlNode * data, int depth, int options)
{
    xml_node_private_t *nodepriv;
    char *prefix_m = NULL;
    xmlNode *child = NULL;
    xmlAttrPtr pIter = NULL;

    if(data == NULL) {
        return;
    }

    nodepriv = data->_private;

    prefix_m = strdup(prefix);
    prefix_m[1] = '+';

    if (is_set(nodepriv->flags, xpf_dirty)
            && is_set(nodepriv->flags, xpf_created)) {
        /* Continue and log full subtree */
        __xml_log_element(log_level, file, function, line,
                          prefix_m, data, depth, options|xml_log_option_open|xml_log_option_close|xml_log_option_children);

    } else if (is_set(nodepriv->flags, xpf_dirty)) {
        char *spaces = calloc(80, 1);
        int s_count = 0, s_max = 80;
        char *prefix_del = NULL;
        char *prefix_moved = NULL;
        const char *flags = prefix;

        insert_prefix(options, &spaces, &s_count, &s_max, depth);
        prefix_del = strdup(prefix);
        prefix_del[0] = '-';
        prefix_del[1] = '-';
        prefix_moved = strdup(prefix);
        prefix_moved[1] = '~';

        if (is_set(nodepriv->flags, xpf_moved)) {
            flags = prefix_moved;
        } else {
            flags = prefix;
        }

        __xml_log_element(log_level, file, function, line,
                          flags, data, depth, options|xml_log_option_open);

        for (pIter = pcmk__first_xml_attr(data); pIter != NULL; pIter = pIter->next) {
            const char *aname = (const char*)pIter->name;

            nodepriv = pIter->_private;
            if (is_set(nodepriv->flags, xpf_deleted)) {
                const char *value = crm_element_value(data, aname);
                flags = prefix_del;
                do_crm_log_alias(log_level, file, function, line,
                                 "%s %s @%s=%s", flags, spaces, aname, value);

            } else if (is_set(nodepriv->flags, xpf_dirty)) {
                const char *value = crm_element_value(data, aname);

                if (is_set(nodepriv->flags, xpf_created)) {
                    flags = prefix_m;

                } else if (is_set(nodepriv->flags, xpf_modified)) {
                    flags = prefix;

                } else if (is_set(nodepriv->flags, xpf_moved)) {
                    flags = prefix_moved;

                } else {
                    flags = prefix;
                }
                do_crm_log_alias(log_level, file, function, line,
                                 "%s %s @%s=%s", flags, spaces, aname, value);
            }
        }
        free(prefix_moved);
        free(prefix_del);
        free(spaces);

        for (child = __xml_first_child(data); child != NULL; child = __xml_next(child)) {
            __xml_log_change_element(log_level, file, function, line, prefix, child, depth + 1, options);
        }

        __xml_log_element(log_level, file, function, line,
                          prefix, data, depth, options|xml_log_option_close);

    } else {
        for (child = __xml_first_child(data); child != NULL; child = __xml_next(child)) {
            __xml_log_change_element(log_level, file, function, line, prefix, child, depth + 1, options);
        }
    }

    free(prefix_m);

}

void
log_data_element(int log_level, const char *file, const char *function, int line,
                 const char *prefix, xmlNode * data, int depth, int options)
{
    xmlNode *a_child = NULL;

    char *prefix_m = NULL;

    if (prefix == NULL) {
        prefix = "";
    }

    /* Since we use the same file and line, to avoid confusing libqb, we need to use the same format strings */
    if (data == NULL) {
        do_crm_log_alias(log_level, file, function, line, "%s: %s", prefix,
                         "No data to dump as XML");
        return;
    }

    if(is_set(options, xml_log_option_dirty_add) || is_set(options, xml_log_option_dirty_add)) {
        __xml_log_change_element(log_level, file, function, line, prefix, data, depth, options);
        return;
    }

    if (is_set(options, xml_log_option_formatted)) {
        if (is_set(options, xml_log_option_diff_plus)
            && (data->children == NULL || crm_element_value(data, XML_DIFF_MARKER))) {
            options |= xml_log_option_diff_all;
            prefix_m = strdup(prefix);
            prefix_m[1] = '+';
            prefix = prefix_m;

        } else if (is_set(options, xml_log_option_diff_minus)
                   && (data->children == NULL || crm_element_value(data, XML_DIFF_MARKER))) {
            options |= xml_log_option_diff_all;
            prefix_m = strdup(prefix);
            prefix_m[1] = '-';
            prefix = prefix_m;
        }
    }

    if (is_set(options, xml_log_option_diff_short)
               && is_not_set(options, xml_log_option_diff_all)) {
        /* Still searching for the actual change */
        for (a_child = __xml_first_child(data); a_child != NULL; a_child = __xml_next(a_child)) {
            log_data_element(log_level, file, function, line, prefix, a_child, depth + 1, options);
        }
    } else {
        __xml_log_element(log_level, file, function, line, prefix, data, depth,
                          options|xml_log_option_open|xml_log_option_close|xml_log_option_children);
    }
    free(prefix_m);
}

static void
dump_filtered_xml(xmlNode * data, int options, char **buffer, int *offset, int *max)
{
    int lpc;
    xmlAttrPtr xIter = NULL;
    static int filter_len = DIMOF(filter);

    for (lpc = 0; options && lpc < filter_len; lpc++) {
        filter[lpc].found = FALSE;
    }

    for (xIter = pcmk__first_xml_attr(data); xIter != NULL; xIter = xIter->next) {
        bool skip = FALSE;
        const char *p_name = (const char *)xIter->name;

        for (lpc = 0; skip == FALSE && lpc < filter_len; lpc++) {
            if (filter[lpc].found == FALSE && strcmp(p_name, filter[lpc].string) == 0) {
                filter[lpc].found = TRUE;
                skip = TRUE;
                break;
            }
        }

        if (skip == FALSE) {
            dump_xml_attr(xIter, options, buffer, offset, max);
        }
    }
}

static void
dump_xml_element(xmlNode * data, int options, char **buffer, int *offset, int *max, int depth)
{
    const char *name = NULL;

    CRM_ASSERT(max != NULL);
    CRM_ASSERT(offset != NULL);
    CRM_ASSERT(buffer != NULL);

    if (data == NULL) {
        crm_trace("Nothing to dump");
        return;
    }

    if (*buffer == NULL) {
        *offset = 0;
        *max = 0;
    }

    name = crm_element_name(data);
    CRM_ASSERT(name != NULL);

    insert_prefix(options, buffer, offset, max, depth);
    buffer_print(*buffer, *max, *offset, "<%s", name);

    if (options & xml_log_option_filtered) {
        dump_filtered_xml(data, options, buffer, offset, max);

    } else {
        xmlAttrPtr xIter = NULL;

        for (xIter = pcmk__first_xml_attr(data); xIter != NULL; xIter = xIter->next) {
            dump_xml_attr(xIter, options, buffer, offset, max);
        }
    }

    if (data->children == NULL) {
        buffer_print(*buffer, *max, *offset, "/>");

    } else {
        buffer_print(*buffer, *max, *offset, ">");
    }

    if (options & xml_log_option_formatted) {
        buffer_print(*buffer, *max, *offset, "\n");
    }

    if (data->children) {
        xmlNode *xChild = NULL;
        for(xChild = data->children; xChild != NULL; xChild = xChild->next) {
            crm_xml_dump(xChild, options, buffer, offset, max, depth + 1);
        }

        insert_prefix(options, buffer, offset, max, depth);
        buffer_print(*buffer, *max, *offset, "</%s>", name);

        if (options & xml_log_option_formatted) {
            buffer_print(*buffer, *max, *offset, "\n");
        }
    }
}

static void
dump_xml_text(xmlNode * data, int options, char **buffer, int *offset, int *max, int depth)
{
    CRM_ASSERT(max != NULL);
    CRM_ASSERT(offset != NULL);
    CRM_ASSERT(buffer != NULL);

    if (data == NULL) {
        crm_trace("Nothing to dump");
        return;
    }

    if (*buffer == NULL) {
        *offset = 0;
        *max = 0;
    }

    insert_prefix(options, buffer, offset, max, depth);

    buffer_print(*buffer, *max, *offset, "%s", data->content);

    if (options & xml_log_option_formatted) {
        buffer_print(*buffer, *max, *offset, "\n");
    }
}

static void
dump_xml_cdata(xmlNode * data, int options, char **buffer, int *offset, int *max, int depth)
{
    CRM_ASSERT(max != NULL);
    CRM_ASSERT(offset != NULL);
    CRM_ASSERT(buffer != NULL);

    if (data == NULL) {
        crm_trace("Nothing to dump");
        return;
    }

    if (*buffer == NULL) {
        *offset = 0;
        *max = 0;
    }

    insert_prefix(options, buffer, offset, max, depth);

    buffer_print(*buffer, *max, *offset, "<![CDATA[");
    buffer_print(*buffer, *max, *offset, "%s", data->content);
    buffer_print(*buffer, *max, *offset, "]]>");

    if (options & xml_log_option_formatted) {
        buffer_print(*buffer, *max, *offset, "\n");
    }
}


static void
dump_xml_comment(xmlNode * data, int options, char **buffer, int *offset, int *max, int depth)
{
    CRM_ASSERT(max != NULL);
    CRM_ASSERT(offset != NULL);
    CRM_ASSERT(buffer != NULL);

    if (data == NULL) {
        crm_trace("Nothing to dump");
        return;
    }

    if (*buffer == NULL) {
        *offset = 0;
        *max = 0;
    }

    insert_prefix(options, buffer, offset, max, depth);

    buffer_print(*buffer, *max, *offset, "<!--");
    buffer_print(*buffer, *max, *offset, "%s", data->content);
    buffer_print(*buffer, *max, *offset, "-->");

    if (options & xml_log_option_formatted) {
        buffer_print(*buffer, *max, *offset, "\n");
    }
}

#define PCMK__XMLDUMP_STATS 0

void
crm_xml_dump(xmlNode * data, int options, char **buffer, int *offset, int *max, int depth)
{
    if(data == NULL) {
        *offset = 0;
        *max = 0;
        return;
    }

    if (is_not_set(options, xml_log_option_filtered)
            && is_set(options, xml_log_option_full_fledged)) {
        /* libxml's serialization reuse is a good idea, sadly we cannot
           apply it for the filtered cases (preceding filtering pass
           would preclude further reuse of such in-situ modified XML
           in generic context and is likely not a win performance-wise),
           and there's also a historically unstable throughput argument
           (likely stemming from memory allocation overhead, eventhough
           that shall be minimized with defaults preset in crm_xml_init) */
#if (PCMK__XMLDUMP_STATS - 0)
        time_t next, new = time(NULL);
#endif
        xmlDoc *doc;
        xmlOutputBuffer *xml_buffer;

        doc = getDocPtr(data);
        /* doc will only be NULL if data is */
        CRM_CHECK(doc != NULL, return);

        xml_buffer = xmlAllocOutputBuffer(NULL);
        CRM_ASSERT(xml_buffer != NULL);

        /* XXX we could setup custom allocation scheme for the particular
               buffer, but it's subsumed with crm_xml_init that needs to
               be invoked prior to entering this function as such, since
               its other branch vitally depends on it -- what can be done
               about this all is to have a facade parsing functions that
               would 100% mark entering libxml code for us, since we don't
               do anything as crazy as swapping out the binary form of the
               parsed tree (but those would need to be strictly used as
               opposed to libxml's raw functions) */

        xmlNodeDumpOutput(xml_buffer, doc, data, 0,
                          (options & xml_log_option_formatted), NULL);
        xmlOutputBufferWrite(xml_buffer, sizeof("\n") - 1, "\n");  /* final NL */
        if (xml_buffer->buffer != NULL) {
            buffer_print(*buffer, *max, *offset, "%s",
                         (char *) xmlBufContent(xml_buffer->buffer));
        }

#if (PCMK__XMLDUMP_STATS - 0)
        next = time(NULL);
        if ((now + 1) < next) {
            crm_log_xml_trace(data, "Long time");
            crm_err("xmlNodeDump() -> %dbytes took %ds", *max, next - now);
        }
#endif

        xmlOutputBufferClose(xml_buffer);
        return;
    }

    switch(data->type) {
        case XML_ELEMENT_NODE:
            /* Handle below */
            dump_xml_element(data, options, buffer, offset, max, depth);
            break;
        case XML_TEXT_NODE:
            /* if option xml_log_option_text is enabled, then dump XML_TEXT_NODE */
            if (options & xml_log_option_text) {
                dump_xml_text(data, options, buffer, offset, max, depth);
            }
            return;
        case XML_COMMENT_NODE:
            dump_xml_comment(data, options, buffer, offset, max, depth);
            break;
        case XML_CDATA_SECTION_NODE:
            dump_xml_cdata(data, options, buffer, offset, max, depth);
            break;
        default:
            crm_warn("Unhandled type: %d", data->type);
            return;

            /*
            XML_ATTRIBUTE_NODE = 2
            XML_ENTITY_REF_NODE = 5
            XML_ENTITY_NODE = 6
            XML_PI_NODE = 7
            XML_DOCUMENT_NODE = 9
            XML_DOCUMENT_TYPE_NODE = 10
            XML_DOCUMENT_FRAG_NODE = 11
            XML_NOTATION_NODE = 12
            XML_HTML_DOCUMENT_NODE = 13
            XML_DTD_NODE = 14
            XML_ELEMENT_DECL = 15
            XML_ATTRIBUTE_DECL = 16
            XML_ENTITY_DECL = 17
            XML_NAMESPACE_DECL = 18
            XML_XINCLUDE_START = 19
            XML_XINCLUDE_END = 20
            XML_DOCB_DOCUMENT_NODE = 21
            */
    }

}

void
crm_buffer_add_char(char **buffer, int *offset, int *max, char c)
{
    buffer_print(*buffer, *max, *offset, "%c", c);
}

char *
dump_xml_formatted_with_text(xmlNode * an_xml_node)
{
    char *buffer = NULL;
    int offset = 0, max = 0;

    crm_xml_dump(an_xml_node,
                 xml_log_option_formatted|xml_log_option_full_fledged,
                 &buffer, &offset, &max, 0);
    return buffer;
}

char *
dump_xml_formatted(xmlNode * an_xml_node)
{
    char *buffer = NULL;
    int offset = 0, max = 0;

    crm_xml_dump(an_xml_node, xml_log_option_formatted, &buffer, &offset, &max, 0);
    return buffer;
}

char *
dump_xml_unformatted(xmlNode * an_xml_node)
{
    char *buffer = NULL;
    int offset = 0, max = 0;

    crm_xml_dump(an_xml_node, 0, &buffer, &offset, &max, 0);
    return buffer;
}

gboolean
xml_has_children(const xmlNode * xml_root)
{
    if (xml_root != NULL && xml_root->children != NULL) {
        return TRUE;
    }
    return FALSE;
}

void
xml_remove_prop(xmlNode * obj, const char *name)
{
    if (pcmk__check_acl(obj, NULL, xpf_acl_write) == FALSE) {
        crm_trace("Cannot remove %s from %s", name, obj->name);

    } else if (pcmk__tracking_xml_changes(obj, FALSE)) {
        /* Leave in place (marked for removal) until after the diff is calculated */
        xmlAttr *attr = xmlHasProp(obj, (pcmkXmlStr) name);
        xml_node_private_t *nodepriv = attr->_private;

        set_parent_flag(obj, xpf_dirty);
        nodepriv->flags |= xpf_deleted;
        /* crm_trace("Setting flag %x due to %s[@id=%s].%s", xpf_dirty, obj->name, ID(obj), name); */

    } else {
        xmlUnsetProp(obj, (pcmkXmlStr) name);
    }
}

void
purge_diff_markers(xmlNode * a_node)
{
    xmlNode *child = NULL;

    CRM_CHECK(a_node != NULL, return);

    xml_remove_prop(a_node, XML_DIFF_MARKER);
    for (child = __xml_first_child(a_node); child != NULL; child = __xml_next(child)) {
        purge_diff_markers(child);
    }
}

void
save_xml_to_file(xmlNode * xml, const char *desc, const char *filename)
{
    char *f = NULL;

    if (filename == NULL) {
        char *uuid = crm_generate_uuid();

        f = crm_strdup_printf("%s/%s", crm_get_tmpdir(), uuid);
        filename = f;
        free(uuid);
    }

    crm_info("Saving %s to %s", desc, filename);
    write_xml_file(xml, filename, FALSE);
    free(f);
}

gboolean
apply_xml_diff(xmlNode *old_xml, xmlNode * diff, xmlNode **new_xml)
{
    gboolean result = TRUE;
    int root_nodes_seen = 0;
    static struct qb_log_callsite *digest_cs = NULL;
    const char *digest = crm_element_value(diff, XML_ATTR_DIGEST);
    const char *version = crm_element_value(diff, XML_ATTR_CRM_VERSION);

    xmlNode *child_diff = NULL;
    xmlNode *added = find_xml_node(diff, "diff-added", FALSE);
    xmlNode *removed = find_xml_node(diff, "diff-removed", FALSE);

    CRM_CHECK(new_xml != NULL, return FALSE);
    if (digest_cs == NULL) {
        digest_cs =
            qb_log_callsite_get(__func__, __FILE__, "diff-digest", LOG_TRACE, __LINE__,
                                crm_trace_nonlog);
    }

    crm_trace("Subtraction Phase");
    for (child_diff = __xml_first_child(removed); child_diff != NULL;
         child_diff = __xml_next(child_diff)) {
        CRM_CHECK(root_nodes_seen == 0, result = FALSE);
        if (root_nodes_seen == 0) {
            *new_xml = subtract_xml_object(NULL, old_xml, child_diff, FALSE, NULL, NULL);
        }
        root_nodes_seen++;
    }

    if (root_nodes_seen == 0) {
        *new_xml = copy_xml(old_xml);

    } else if (root_nodes_seen > 1) {
        crm_err("(-) Diffs cannot contain more than one change set..." " saw %d", root_nodes_seen);
        result = FALSE;
    }

    root_nodes_seen = 0;
    crm_trace("Addition Phase");
    if (result) {
        xmlNode *child_diff = NULL;

        for (child_diff = __xml_first_child(added); child_diff != NULL;
             child_diff = __xml_next(child_diff)) {
            CRM_CHECK(root_nodes_seen == 0, result = FALSE);
            if (root_nodes_seen == 0) {
                add_xml_object(NULL, *new_xml, child_diff, TRUE);
            }
            root_nodes_seen++;
        }
    }

    if (root_nodes_seen > 1) {
        crm_err("(+) Diffs cannot contain more than one change set..." " saw %d", root_nodes_seen);
        result = FALSE;

    } else if (result && digest) {
        char *new_digest = NULL;

        purge_diff_markers(*new_xml);       /* Purge now so the diff is ok */
        new_digest = calculate_xml_versioned_digest(*new_xml, FALSE, TRUE, version);
        if (safe_str_neq(new_digest, digest)) {
            crm_info("Digest mis-match: expected %s, calculated %s", digest, new_digest);
            result = FALSE;

            crm_trace("%p %.6x", digest_cs, digest_cs ? digest_cs->targets : 0);
            if (digest_cs && digest_cs->targets) {
                save_xml_to_file(old_xml, "diff:original", NULL);
                save_xml_to_file(diff, "diff:input", NULL);
                save_xml_to_file(*new_xml, "diff:new", NULL);
            }

        } else {
            crm_trace("Digest matched: expected %s, calculated %s", digest, new_digest);
        }
        free(new_digest);

    } else if (result) {
        purge_diff_markers(*new_xml);       /* Purge now so the diff is ok */
    }

    return result;
}

/*!
 * \internal
 * \brief Set a flag on all attributes of an XML element
 *
 * \param[in,out] xml   XML node to set flags on
 * \param[in]     flag  XML private flag to set
 */
static void
set_attrs_flag(xmlNode *xml, enum xml_private_flags flag)
{
    for (xmlAttr *attr = pcmk__first_xml_attr(xml); attr; attr = attr->next) {
        ((xml_node_private_t *) (attr->_private))->flags |= flag;
    }
}

/*!
 * \internal
 * \brief Add an XML attribute to a node, marked as deleted
 *
 * When calculating XML changes, we need to know when an attribute has been
 * deleted. Add the attribute back to the new XML, so that we can check the
 * removal against ACLs, and mark it as deleted for later removal after
 * differences have been calculated.
 */
static void
mark_attr_deleted(xmlNode *new_xml, const char *element, const char *attr_name,
                  const char *old_value)
{
    xml_doc_private_t *docpriv = new_xml->doc->_private;
    xmlAttr *attr = NULL;
    xml_node_private_t *nodepriv;

    // Prevent the dirty flag being set recursively upwards
    clear_bit(docpriv->flags, xpf_tracking);

    // Restore the old value (and the tracking flag)
    attr = xmlSetProp(new_xml, (pcmkXmlStr) attr_name, (pcmkXmlStr) old_value);
    set_bit(docpriv->flags, xpf_tracking);

    // Reset flags (so the attribute doesn't appear as newly created)
    nodepriv = attr->_private;
    nodepriv->flags = 0;

    // Check ACLs and mark restored value for later removal
    xml_remove_prop(new_xml, attr_name);

    crm_trace("XML attribute %s=%s was removed from %s",
              attr_name, old_value, element);
}

/*
 * \internal
 * \brief Check ACLs for a changed XML attribute
 */
static void
mark_attr_changed(xmlNode *new_xml, const char *element, const char *attr_name,
                  const char *old_value)
{
    char *vcopy = crm_element_value_copy(new_xml, attr_name);

    crm_trace("XML attribute %s was changed from '%s' to '%s' in %s",
              attr_name, old_value, vcopy, element);

    // Restore the original value
    xmlSetProp(new_xml, (pcmkXmlStr) attr_name, (pcmkXmlStr) old_value);

    // Change it back to the new value, to check ACLs
    crm_xml_add(new_xml, attr_name, vcopy);
    free(vcopy);
}

/*!
 * \internal
 * \brief Mark an XML attribute as having changed position
 */
static void
mark_attr_moved(xmlNode *new_xml, const char *element, xmlAttr *old_attr,
                xmlAttr *new_attr, int p_old, int p_new)
{
    xml_node_private_t *nodepriv = new_attr->_private;

    crm_trace("XML attribute %s moved from position %d to %d in %s",
              old_attr->name, p_old, p_new, element);

    // Mark document, element, and all element's parents as changed
    __xml_node_dirty(new_xml);

    // Mark attribute as changed
    nodepriv->flags |= xpf_dirty|xpf_moved;

    nodepriv = (p_old > p_new)? old_attr->_private : new_attr->_private;
    nodepriv->flags |= xpf_skip;
}

/*!
 * \internal
 * \brief Calculate differences in all previously existing XML attributes
 */
static void
xml_diff_old_attrs(xmlNode *old_xml, xmlNode *new_xml)
{
    xmlAttr *attr_iter = pcmk__first_xml_attr(old_xml);

    while (attr_iter != NULL) {
        xmlAttr *old_attr = attr_iter;
        xmlAttr *new_attr = xmlHasProp(new_xml, attr_iter->name);
        const char *name = (const char *) attr_iter->name;
        const char *old_value = crm_element_value(old_xml, name);

        attr_iter = attr_iter->next;
        if (new_attr == NULL) {
            mark_attr_deleted(new_xml, (const char *) old_xml->name, name,
                              old_value);

        } else {
            xml_node_private_t *nodepriv = new_attr->_private;
            int new_pos = __xml_offset((xmlNode*) new_attr);
            int old_pos = __xml_offset((xmlNode*) old_attr);
            const char *new_value = crm_element_value(new_xml, name);

            // This attribute isn't new
            nodepriv->flags &= ~xpf_created;

            if (strcmp(new_value, old_value) != 0) {
                mark_attr_changed(new_xml, (const char *) old_xml->name, name,
                                  old_value);

            } else if ((old_pos != new_pos)
                       && !pcmk__tracking_xml_changes(new_xml, TRUE)) {
                mark_attr_moved(new_xml, (const char *) old_xml->name,
                                old_attr, new_attr, old_pos, new_pos);
            }
        }
    }
}

/*!
 * \internal
 * \brief Check all attributes in new XML for creation
 */
static void
mark_created_attrs(xmlNode *new_xml)
{
    xmlAttr *attr_iter = pcmk__first_xml_attr(new_xml);

    while (attr_iter != NULL) {
        xmlAttr *new_attr = attr_iter;
        xml_node_private_t *nodepriv = attr_iter->_private;

        attr_iter = attr_iter->next;
        if (is_set(nodepriv->flags, xpf_created)) {
            const char *attr_name = (const char *) new_attr->name;

            crm_trace("Created new attribute %s=%s in %s",
                      attr_name, crm_element_value(new_xml, attr_name),
                      new_xml->name);

            /* Check ACLs (we can't use the remove-then-create trick because it
             * would modify the attribute position).
             */
            if (pcmk__check_acl(new_xml, attr_name, xpf_acl_write)) {
                pcmk__mark_xml_attr_dirty(new_attr);
            } else {
                // Creation was not allowed, so remove the attribute
                xmlUnsetProp(new_xml, new_attr->name);
            }
        }
    }
}

/*!
 * \internal
 * \brief Calculate differences in attributes between two XML nodes
 */
static void
xml_diff_attrs(xmlNode *old_xml, xmlNode *new_xml)
{
    set_attrs_flag(new_xml, xpf_created); // cleared later if not really new
    xml_diff_old_attrs(old_xml, new_xml);
    mark_created_attrs(new_xml);
}

/*!
 * \internal
 * \brief Add an XML child element to a node, marked as deleted
 *
 * When calculating XML changes, we need to know when a child element has been
 * deleted. Add the child back to the new XML, so that we can check the removal
 * against ACLs, and mark it as deleted for later removal after differences have
 * been calculated.
 */
static void
mark_child_deleted(xmlNode *old_child, xmlNode *new_parent)
{
    // Re-create the child element so we can check ACLs
    xmlNode *candidate = add_node_copy(new_parent, old_child);

    // Clear flags on new child and its children
    __xml_node_clean(candidate);

    // Check whether ACLs allow the deletion
    pcmk__apply_acl(xmlDocGetRootElement(candidate->doc));

    // Remove the child again (which will track it in document's deleted_objs)
    free_xml_with_position(candidate, __xml_offset(old_child));

    if (find_element(new_parent, old_child, TRUE) == NULL) {
        ((xml_doc_private_t *) (old_child->_private))->flags |= xpf_skip;
    }
}

static void
mark_child_moved(xmlNode *old_child, xmlNode *new_parent, xmlNode *new_child,
                 int p_old, int p_new)
{
    xml_node_private_t *nodepriv = new_child->_private;

    crm_trace("Child element %s with id='%s' moved from position %d to %d under %s",
              new_child->name, (ID(new_child)? ID(new_child) : "<no id>"),
              p_old, p_new, new_parent->name);
    __xml_node_dirty(new_parent);
    nodepriv->flags |= xpf_moved;

    if (p_old > p_new) {
        nodepriv = old_child->_private;
    } else {
        nodepriv = new_child->_private;
    }
    nodepriv->flags |= xpf_skip;
}

static void
__xml_diff_object(xmlNode *old_xml, xmlNode *new_xml, bool check_top)
{
    xmlNode *cIter = NULL;
    xml_node_private_t *nodepriv = NULL;

    CRM_CHECK(new_xml != NULL, return);
    if (old_xml == NULL) {
        crm_node_created(new_xml);
        pcmk__apply_creation_acl(new_xml, check_top);
        return;
    }

    nodepriv = new_xml->_private;
    CRM_CHECK(nodepriv != NULL, return);

    if (nodepriv->flags & xpf_processed) {
        /* Avoid re-comparing nodes */
        return;
    }
    nodepriv->flags |= xpf_processed;

    xml_diff_attrs(old_xml, new_xml);

    // Check for differences in the original children
    for (cIter = __xml_first_child(old_xml); cIter != NULL; ) {
        xmlNode *old_child = cIter;
        xmlNode *new_child = find_element(new_xml, cIter, TRUE);

        cIter = __xml_next(cIter);
        if(new_child) {
            __xml_diff_object(old_child, new_child, TRUE);

        } else {
            mark_child_deleted(old_child, new_xml);
        }
    }

    // Check for moved or created children
    for (cIter = __xml_first_child(new_xml); cIter != NULL; ) {
        xmlNode *new_child = cIter;
        xmlNode *old_child = find_element(old_xml, cIter, TRUE);

        cIter = __xml_next(cIter);
        if(old_child == NULL) {
            // This is a newly created child
            nodepriv = new_child->_private;
            nodepriv->flags |= xpf_skip;
            __xml_diff_object(old_child, new_child, TRUE);

        } else {
            /* Check for movement, we already checked for differences */
            int p_new = __xml_offset(new_child);
            int p_old = __xml_offset(old_child);

            if(p_old != p_new) {
                mark_child_moved(old_child, new_xml, new_child, p_old, p_new);
            }
        }
    }
}

void
xml_calculate_significant_changes(xmlNode *old_xml, xmlNode *new_xml)
{
    pcmk__set_xml_flag(new_xml, xpf_lazy);
    xml_calculate_changes(old_xml, new_xml);
}

void
xml_calculate_changes(xmlNode *old_xml, xmlNode *new_xml)
{
    CRM_CHECK(safe_str_eq(crm_element_name(old_xml),
                          crm_element_name(new_xml)),
              return);
    CRM_CHECK(safe_str_eq(ID(old_xml), ID(new_xml)), return);

    if(xml_tracking_changes(new_xml) == FALSE) {
        xml_track_changes(new_xml, NULL, NULL, FALSE);
    }

    __xml_diff_object(old_xml, new_xml, FALSE);
}

xmlNode *
diff_xml_object(xmlNode * old, xmlNode * new, gboolean suppress)
{
    xmlNode *tmp1 = NULL;
    xmlNode *diff = create_xml_node(NULL, "diff");
    xmlNode *removed = create_xml_node(diff, "diff-removed");
    xmlNode *added = create_xml_node(diff, "diff-added");

    crm_xml_add(diff, XML_ATTR_CRM_VERSION, CRM_FEATURE_SET);

    tmp1 = subtract_xml_object(removed, old, new, FALSE, NULL, "removed:top");
    if (suppress && tmp1 != NULL && can_prune_leaf(tmp1)) {
        free_xml(tmp1);
    }

    tmp1 = subtract_xml_object(added, new, old, TRUE, NULL, "added:top");
    if (suppress && tmp1 != NULL && can_prune_leaf(tmp1)) {
        free_xml(tmp1);
    }

    if (added->children == NULL && removed->children == NULL) {
        free_xml(diff);
        diff = NULL;
    }

    return diff;
}

gboolean
can_prune_leaf(xmlNode * xml_node)
{
    xmlNode *cIter = NULL;
    xmlAttrPtr pIter = NULL;
    gboolean can_prune = TRUE;
    const char *name = crm_element_name(xml_node);

    if (safe_str_eq(name, XML_TAG_RESOURCE_REF)
        || safe_str_eq(name, XML_CIB_TAG_OBJ_REF)
        || safe_str_eq(name, XML_ACL_TAG_ROLE_REF)
        || safe_str_eq(name, XML_ACL_TAG_ROLE_REFv1)) {
        return FALSE;
    }

    for (pIter = pcmk__first_xml_attr(xml_node); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;

        if (strcmp(p_name, XML_ATTR_ID) == 0) {
            continue;
        }
        can_prune = FALSE;
    }

    cIter = __xml_first_child(xml_node);
    while (cIter) {
        xmlNode *child = cIter;

        cIter = __xml_next(cIter);
        if (can_prune_leaf(child)) {
            free_xml(child);
        } else {
            can_prune = FALSE;
        }
    }
    return can_prune;
}

static xmlNode *
find_xml_comment(xmlNode * root, xmlNode * search_comment, gboolean exact)
{
    xmlNode *a_child = NULL;
    int search_offset = __xml_offset(search_comment);

    CRM_CHECK(search_comment->type == XML_COMMENT_NODE, return NULL);

    for (a_child = __xml_first_child(root); a_child != NULL; a_child = __xml_next(a_child)) {
        if (exact) {
            int offset = __xml_offset(a_child);
            xml_node_private_t *nodepriv = a_child->_private;

            if (offset < search_offset) {
                continue;

            } else if (offset > search_offset) {
                return NULL;
            }

            if (is_set(nodepriv->flags, xpf_skip)) {
                continue;
            }
        }

        if (a_child->type == XML_COMMENT_NODE
            && safe_str_eq((const char *)a_child->content, (const char *)search_comment->content)) {
            return a_child;

        } else if (exact) {
            return NULL;
        }
    }

    return NULL;
}

static xmlNode *
subtract_xml_comment(xmlNode * parent, xmlNode * left, xmlNode * right,
                     gboolean * changed)
{
    CRM_CHECK(left != NULL, return NULL);
    CRM_CHECK(left->type == XML_COMMENT_NODE, return NULL);

    if (right == NULL
        || safe_str_neq((const char *)left->content, (const char *)right->content)) {
        xmlNode *deleted = NULL;

        deleted = add_node_copy(parent, left);
        *changed = TRUE;

        return deleted;
    }

    return NULL;
}

xmlNode *
subtract_xml_object(xmlNode * parent, xmlNode * left, xmlNode * right,
                    gboolean full, gboolean * changed, const char *marker)
{
    gboolean dummy = FALSE;
    gboolean skip = FALSE;
    xmlNode *diff = NULL;
    xmlNode *right_child = NULL;
    xmlNode *left_child = NULL;
    xmlAttrPtr xIter = NULL;

    const char *id = NULL;
    const char *name = NULL;
    const char *value = NULL;
    const char *right_val = NULL;

    int lpc = 0;
    static int filter_len = DIMOF(filter);

    if (changed == NULL) {
        changed = &dummy;
    }

    if (left == NULL) {
        return NULL;
    }

    if (left->type == XML_COMMENT_NODE) {
        return subtract_xml_comment(parent, left, right, changed);
    }

    id = ID(left);
    if (right == NULL) {
        xmlNode *deleted = NULL;

        crm_trace("Processing <%s id=%s> (complete copy)", crm_element_name(left), id);
        deleted = add_node_copy(parent, left);
        crm_xml_add(deleted, XML_DIFF_MARKER, marker);

        *changed = TRUE;
        return deleted;
    }

    name = crm_element_name(left);
    CRM_CHECK(name != NULL, return NULL);
    CRM_CHECK(safe_str_eq(crm_element_name(left), crm_element_name(right)), return NULL);

    /* check for XML_DIFF_MARKER in a child */
    value = crm_element_value(right, XML_DIFF_MARKER);
    if (value != NULL && strcmp(value, "removed:top") == 0) {
        crm_trace("We are the root of the deletion: %s.id=%s", name, id);
        *changed = TRUE;
        return NULL;
    }

    /* Avoiding creating the full heirarchy would save even more work here */
    diff = create_xml_node(parent, name);

    /* Reset filter */
    for (lpc = 0; lpc < filter_len; lpc++) {
        filter[lpc].found = FALSE;
    }

    /* changes to child objects */
    for (left_child = __xml_first_child(left); left_child != NULL;
         left_child = __xml_next(left_child)) {
        gboolean child_changed = FALSE;

        right_child = find_element(right, left_child, FALSE);
        subtract_xml_object(diff, left_child, right_child, full, &child_changed, marker);
        if (child_changed) {
            *changed = TRUE;
        }
    }

    if (*changed == FALSE) {
        /* Nothing to do */

    } else if (full) {
        xmlAttrPtr pIter = NULL;

        for (pIter = pcmk__first_xml_attr(left); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = pcmk__xml_attr_value(pIter);

            xmlSetProp(diff, (pcmkXmlStr) p_name, (pcmkXmlStr) p_value);
        }

        /* We already have everything we need... */
        goto done;
    }

    /* changes to name/value pairs */
    for (xIter = pcmk__first_xml_attr(left); xIter != NULL; xIter = xIter->next) {
        const char *prop_name = (const char *)xIter->name;
        xmlAttrPtr right_attr = NULL;
        xml_node_private_t *nodepriv = NULL;

        if (strcmp(prop_name, XML_ATTR_ID) == 0) {
            /* id already obtained when present ~ this case, so just reuse */
            xmlSetProp(diff, (pcmkXmlStr) XML_ATTR_ID, (pcmkXmlStr) id);
            continue;
        }

        skip = FALSE;
        for (lpc = 0; skip == FALSE && lpc < filter_len; lpc++) {
            if (filter[lpc].found == FALSE && strcmp(prop_name, filter[lpc].string) == 0) {
                filter[lpc].found = TRUE;
                skip = TRUE;
                break;
            }
        }

        if (skip) {
            continue;
        }

        right_attr = xmlHasProp(right, (pcmkXmlStr) prop_name);
        if (right_attr) {
            nodepriv = right_attr->_private;
        }

        right_val = crm_element_value(right, prop_name);
        if (right_val == NULL
                || (nodepriv != NULL && is_set(nodepriv->flags, xpf_deleted))) {
            /* new */
            *changed = TRUE;
            if (full) {
                xmlAttrPtr pIter = NULL;

                for (pIter = pcmk__first_xml_attr(left); pIter != NULL; pIter = pIter->next) {
                    const char *p_name = (const char *)pIter->name;
                    const char *p_value = pcmk__xml_attr_value(pIter);

                    xmlSetProp(diff, (pcmkXmlStr) p_name, (pcmkXmlStr) p_value);
                }
                break;

            } else {
                const char *left_value = crm_element_value(left, prop_name);

                xmlSetProp(diff, (pcmkXmlStr) prop_name, (pcmkXmlStr) value);
                crm_xml_add(diff, prop_name, left_value);
            }

        } else {
            /* Only now do we need the left value */
            const char *left_value = crm_element_value(left, prop_name);

            if (strcmp(left_value, right_val) == 0) {
                /* unchanged */

            } else {
                *changed = TRUE;
                if (full) {
                    xmlAttrPtr pIter = NULL;

                    crm_trace("Changes detected to %s in <%s id=%s>", prop_name,
                              crm_element_name(left), id);
                    for (pIter = pcmk__first_xml_attr(left); pIter != NULL; pIter = pIter->next) {
                        const char *p_name = (const char *)pIter->name;
                        const char *p_value = pcmk__xml_attr_value(pIter);

                        xmlSetProp(diff, (pcmkXmlStr) p_name, (pcmkXmlStr) p_value);
                    }
                    break;

                } else {
                    crm_trace("Changes detected to %s (%s -> %s) in <%s id=%s>",
                              prop_name, left_value, right_val, crm_element_name(left), id);
                    crm_xml_add(diff, prop_name, left_value);
                }
            }
        }
    }

    if (*changed == FALSE) {
        free_xml(diff);
        return NULL;

    } else if (full == FALSE && id) {
        crm_xml_add(diff, XML_ATTR_ID, id);
    }
  done:
    return diff;
}

static int
add_xml_comment(xmlNode * parent, xmlNode * target, xmlNode * update)
{
    CRM_CHECK(update != NULL, return 0);
    CRM_CHECK(update->type == XML_COMMENT_NODE, return 0);

    if (target == NULL) {
        target = find_xml_comment(parent, update, FALSE);
    }

    if (target == NULL) {
        add_node_copy(parent, update);

    /* We won't reach here currently */
    } else if (safe_str_neq((const char *)target->content, (const char *)update->content)) {
        xmlFree(target->content);
        target->content = xmlStrdup(update->content);
    }

    return 0;
}

static int
add_xml_object(xmlNode * parent, xmlNode * target, xmlNode * update, gboolean as_diff)
{
    xmlNode *a_child = NULL;
    const char *object_name = NULL,
               *object_href = NULL,
               *object_href_val = NULL;

#if XML_PARSE_DEBUG
    crm_log_xml_trace("update:", update);
    crm_log_xml_trace("target:", target);
#endif

    CRM_CHECK(update != NULL, return 0);

    if (update->type == XML_COMMENT_NODE) {
        return add_xml_comment(parent, target, update);
    }

    object_name = crm_element_name(update);
    object_href_val = ID(update);
    if (object_href_val != NULL) {
        object_href = XML_ATTR_ID;
    } else {
        object_href_val = crm_element_value(update, XML_ATTR_IDREF);
        object_href = (object_href_val == NULL) ? NULL : XML_ATTR_IDREF;
    }

    CRM_CHECK(object_name != NULL, return 0);
    CRM_CHECK(target != NULL || parent != NULL, return 0);

    if (target == NULL) {
        target = find_entity_by_attr_or_just_name(parent, object_name,
                                                  object_href, object_href_val);
    }

    if (target == NULL) {
        target = create_xml_node(parent, object_name);
        CRM_CHECK(target != NULL, return 0);
#if XML_PARSER_DEBUG
        crm_trace("Added  <%s%s%s%s%s/>", crm_str(object_name),
                  object_href ? " " : "",
                  object_href ? object_href : "",
                  object_href ? "=" : "",
                  object_href ? object_href_val : "");

    } else {
        crm_trace("Found node <%s%s%s%s%s/> to update", crm_str(object_name),
                  object_href ? " " : "",
                  object_href ? object_href : "",
                  object_href ? "=" : "",
                  object_href ? object_href_val : "");
#endif
    }

    CRM_CHECK(safe_str_eq(crm_element_name(target), crm_element_name(update)), return 0);

    if (as_diff == FALSE) {
        /* So that expand_plus_plus() gets called */
        copy_in_properties(target, update);

    } else {
        /* No need for expand_plus_plus(), just raw speed */
        xmlAttrPtr pIter = NULL;

        for (pIter = pcmk__first_xml_attr(update); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = pcmk__xml_attr_value(pIter);

            /* Remove it first so the ordering of the update is preserved */
            xmlUnsetProp(target, (pcmkXmlStr) p_name);
            xmlSetProp(target, (pcmkXmlStr) p_name, (pcmkXmlStr) p_value);
        }
    }

    for (a_child = __xml_first_child(update); a_child != NULL; a_child = __xml_next(a_child)) {
#if XML_PARSER_DEBUG
        crm_trace("Updating child <%s%s%s%s%s/>", crm_str(object_name),
                  object_href ? " " : "",
                  object_href ? object_href : "",
                  object_href ? "=" : "",
                  object_href ? object_href_val : "");
#endif
        add_xml_object(target, NULL, a_child, as_diff);
    }

#if XML_PARSER_DEBUG
    crm_trace("Finished with <%s%s%s%s%s/>", crm_str(object_name),
              object_href ? " " : "",
              object_href ? object_href : "",
              object_href ? "=" : "",
              object_href ? object_href_val : "");
#endif
    return 0;
}

gboolean
update_xml_child(xmlNode * child, xmlNode * to_update)
{
    gboolean can_update = TRUE;
    xmlNode *child_of_child = NULL;

    CRM_CHECK(child != NULL, return FALSE);
    CRM_CHECK(to_update != NULL, return FALSE);

    if (safe_str_neq(crm_element_name(to_update), crm_element_name(child))) {
        can_update = FALSE;

    } else if (safe_str_neq(ID(to_update), ID(child))) {
        can_update = FALSE;

    } else if (can_update) {
#if XML_PARSER_DEBUG
        crm_log_xml_trace(child, "Update match found...");
#endif
        add_xml_object(NULL, child, to_update, FALSE);
    }

    for (child_of_child = __xml_first_child(child); child_of_child != NULL;
         child_of_child = __xml_next(child_of_child)) {
        /* only update the first one */
        if (can_update) {
            break;
        }
        can_update = update_xml_child(child_of_child, to_update);
    }

    return can_update;
}

int
find_xml_children(xmlNode ** children, xmlNode * root,
                  const char *tag, const char *field, const char *value, gboolean search_matches)
{
    int match_found = 0;

    CRM_CHECK(root != NULL, return FALSE);
    CRM_CHECK(children != NULL, return FALSE);

    if (tag != NULL && safe_str_neq(tag, crm_element_name(root))) {

    } else if (value != NULL && safe_str_neq(value, crm_element_value(root, field))) {

    } else {
        if (*children == NULL) {
            *children = create_xml_node(NULL, __FUNCTION__);
        }
        add_node_copy(*children, root);
        match_found = 1;
    }

    if (search_matches || match_found == 0) {
        xmlNode *child = NULL;

        for (child = __xml_first_child(root); child != NULL; child = __xml_next(child)) {
            match_found += find_xml_children(children, child, tag, field, value, search_matches);
        }
    }

    return match_found;
}

gboolean
replace_xml_child(xmlNode * parent, xmlNode * child, xmlNode * update, gboolean delete_only)
{
    gboolean can_delete = FALSE;
    xmlNode *child_of_child = NULL;

    const char *up_id = NULL;
    const char *child_id = NULL;
    const char *right_val = NULL;

    CRM_CHECK(child != NULL, return FALSE);
    CRM_CHECK(update != NULL, return FALSE);

    up_id = ID(update);
    child_id = ID(child);

    if (up_id == NULL || (child_id && strcmp(child_id, up_id) == 0)) {
        can_delete = TRUE;
    }
    if (safe_str_neq(crm_element_name(update), crm_element_name(child))) {
        can_delete = FALSE;
    }
    if (can_delete && delete_only) {
        xmlAttrPtr pIter = NULL;

        for (pIter = pcmk__first_xml_attr(update); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = pcmk__xml_attr_value(pIter);

            right_val = crm_element_value(child, p_name);
            if (safe_str_neq(p_value, right_val)) {
                can_delete = FALSE;
            }
        }
    }

    if (can_delete && parent != NULL) {
        crm_log_xml_trace(child, "Delete match found...");
        if (delete_only || update == NULL) {
            free_xml(child);

        } else {
            xmlNode *tmp = copy_xml(update);
            xmlDoc *doc = tmp->doc;
            xmlNode *old = NULL;

            xml_accept_changes(tmp);
            old = xmlReplaceNode(child, tmp);

            if(xml_tracking_changes(tmp)) {
                /* Replaced sections may have included relevant ACLs */
                pcmk__apply_acl(tmp);
            }

            xml_calculate_changes(old, tmp);
            xmlDocSetRootElement(doc, old);
            free_xml(old);
        }
        child = NULL;
        return TRUE;

    } else if (can_delete) {
        crm_log_xml_debug(child, "Cannot delete the search root");
        can_delete = FALSE;
    }

    child_of_child = __xml_first_child(child);
    while (child_of_child) {
        xmlNode *next = __xml_next(child_of_child);

        can_delete = replace_xml_child(child, child_of_child, update, delete_only);

        /* only delete the first one */
        if (can_delete) {
            child_of_child = NULL;
        } else {
            child_of_child = next;
        }
    }

    return can_delete;
}

xmlNode *
sorted_xml(xmlNode *input, xmlNode *parent, gboolean recursive)
{
    xmlNode *child = NULL;
    GSList *nvpairs = NULL;
    xmlNode *result = NULL;
    const char *name = NULL;

    CRM_CHECK(input != NULL, return NULL);

    name = crm_element_name(input);
    CRM_CHECK(name != NULL, return NULL);

    result = create_xml_node(parent, name);
    nvpairs = pcmk_xml_attrs2nvpairs(input);
    nvpairs = pcmk_sort_nvpairs(nvpairs);
    pcmk_nvpairs2xml_attrs(nvpairs, result);
    pcmk_free_nvpairs(nvpairs);

    for (child = __xml_first_child(input); child != NULL;
         child = __xml_next(child)) {

        if (recursive) {
            sorted_xml(child, result, recursive);
        } else {
            add_node_copy(result, child);
        }
    }

    return result;
}

xmlNode *
first_named_child(const xmlNode *parent, const char *name)
{
    xmlNode *match = NULL;

    for (match = __xml_first_child_element(parent); match != NULL;
         match = __xml_next_element(match)) {
        /*
         * name == NULL gives first child regardless of name; this is
         * semantically incorrect in this function, but may be necessary
         * due to prior use of xml_child_iter_filter
         */
        if (name == NULL || strcmp((const char *)match->name, name) == 0) {
            return match;
        }
    }
    return NULL;
}

/*!
 * \brief Get next instance of same XML tag
 *
 * \param[in] sibling  XML tag to start from
 *
 * \return Next sibling XML tag with same name
 */
xmlNode *
crm_next_same_xml(const xmlNode *sibling)
{
    xmlNode *match = __xml_next_element(sibling);
    const char *name = crm_element_name(sibling);

    while (match != NULL) {
        if (!strcmp(crm_element_name(match), name)) {
            return match;
        }
        match = __xml_next_element(match);
    }
    return NULL;
}

void
crm_xml_init(void)
{
    static bool init = TRUE;

    if(init) {
        init = FALSE;
        /* The default allocator XML_BUFFER_ALLOC_EXACT does far too many
         * realloc_safe()s and it can take upwards of 18 seconds (yes, seconds)
         * to dump a 28kb tree which XML_BUFFER_ALLOC_DOUBLEIT can do in
         * less than 1 second.
         */
        xmlSetBufferAllocationScheme(XML_BUFFER_ALLOC_DOUBLEIT);

        /* Populate and free the _private field when nodes are created and destroyed */
        xmlDeregisterNodeDefault(pcmkDeregisterNode);
        xmlRegisterNodeDefault(pcmkRegisterNode);

        crm_schema_init();
    }
}

void
crm_xml_cleanup(void)
{
    crm_info("Cleaning up memory from libxml2");
    crm_schema_cleanup();
    xmlCleanupParser();
}

#define XPATH_MAX 512

xmlNode *
expand_idref(xmlNode * input, xmlNode * top)
{
    const char *tag = NULL;
    const char *ref = NULL;
    xmlNode *result = input;

    if (result == NULL) {
        return NULL;

    } else if (top == NULL) {
        top = input;
    }

    tag = crm_element_name(result);
    ref = crm_element_value(result, XML_ATTR_IDREF);

    if (ref != NULL) {
        char *xpath_string = crm_strdup_printf("//%s[@id='%s']", tag, ref);

        result = get_xpath_object(xpath_string, top, LOG_ERR);
        if (result == NULL) {
            char *nodePath = (char *)xmlGetNodePath(top);

            crm_err("No match for %s found in %s: Invalid configuration", xpath_string,
                    crm_str(nodePath));
            free(nodePath);
        }
        free(xpath_string);
    }
    return result;
}

void
crm_destroy_xml(gpointer data)
{
    free_xml(data);
}

char *
pcmk__xml_artefact_root(enum pcmk__xml_artefact_ns ns)
{
    static const char *base = NULL;
    char *ret = NULL;

    if (base == NULL) {
        base = getenv("PCMK_schema_directory");
    }
    if (base == NULL || base[0] == '\0') {
        base = CRM_SCHEMA_DIRECTORY;
    }

    switch (ns) {
        case pcmk__xml_artefact_ns_legacy_rng:
        case pcmk__xml_artefact_ns_legacy_xslt:
            ret = strdup(base);
            break;
        case pcmk__xml_artefact_ns_base_rng:
        case pcmk__xml_artefact_ns_base_xslt:
            ret = crm_strdup_printf("%s/base", base);
            break;
        default:
            crm_err("XML artefact family specified as %u not recognized", ns);
    }
    return ret;
}

char *
pcmk__xml_artefact_path(enum pcmk__xml_artefact_ns ns, const char *filespec)
{
    char *base = pcmk__xml_artefact_root(ns), *ret = NULL;

    switch (ns) {
        case pcmk__xml_artefact_ns_legacy_rng:
        case pcmk__xml_artefact_ns_base_rng:
            ret = crm_strdup_printf("%s/%s.rng", base, filespec);
            break;
        case pcmk__xml_artefact_ns_legacy_xslt:
        case pcmk__xml_artefact_ns_base_xslt:
            ret = crm_strdup_printf("%s/%s.xsl", base, filespec);
            break;
        default:
            crm_err("XML artefact family specified as %u not recognized", ns);
    }
    free(base);

    return ret;
}
