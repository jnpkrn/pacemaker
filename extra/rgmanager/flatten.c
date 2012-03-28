/*
  Copyright Red Hat, Inc. 2004-2012

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
  MA 02110-1301 USA
*/
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <libxml/xpath.h>
#include <stdlib.h>
#include <stdio.h>
#include <resgroup.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <list.h>
#include <reslist.h>
#include <xmlconf.h>
#include <libgen.h>

#define shift() {++argv; --argc;}

const char *agentpath = RESOURCE_ROOTDIR;

static xmlNode *
get_rm_node(xmlDocPtr doc)
{
    xmlNodePtr n, o;

    if (!doc)
        return NULL;
    n = xmlDocGetRootElement(doc);
    if (!n)
        return NULL;
    if (strcmp((char *)n->name, "cluster")) {
        fprintf(stderr, "Expected cluster tag, got %s\n", (char *)n->name);
        return NULL;
    }

    for (o = n->xmlChildrenNode; o; o = o->next) {
        if (o->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((char *)o->name, "rm"))
            return o;
    }

    return NULL;
}

static void
remove_resources_block(xmlNodePtr rm)
{
    xmlNodePtr o, r = NULL;

    for (o = rm->xmlChildrenNode; o; o = o->next) {
        if (o->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((char *)o->name, "resources")) {
            r = o;
            break;
        }
    }

    if (!r)
        return;

    xmlUnlinkNode(r);
    xmlFreeNode(r);
}

static int
replace_resource(xmlNodePtr rm, char *restype, char *primattr, char *ident, xmlNodePtr n)
{
    xmlNodePtr o, r = NULL;
    char *p;

    for (o = rm->xmlChildrenNode; o; o = o->next) {
        if (o->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((char *)o->name, restype)) {
            p = (char *)xmlGetProp(o, (xmlChar *) primattr);
            if (!strcmp(p, ident)) {
                r = o;
                break;
            }
        }
    }

    if (!r)
        return -1;

    xmlUnlinkNode(r);
    xmlFreeNode(r);

    xmlAddChild(rm, n);

    return 0;
}

static int
flatten(int argc, char **argv)
{
    xmlDocPtr d = NULL;
    xmlNode *n = NULL, *rm = NULL, *new_rb = NULL;
    resource_rule_t *rulelist = NULL;
    resource_t *reslist = NULL, *curres;
    resource_node_t *tree = NULL, *rn;
    FILE *f = stdout;
    int ret = 0;

    conf_setconfig(argv[0]);
    if (conf_open() < 0)
        return -1;

    while (argc >= 2) {
        shift();
        if (!strcmp(argv[0], "-r")) {
            if (!new_rb)
                new_rb = xmlNewNode(NULL, (xmlChar *) "resources");
        } else {
            if (f == stdout)
                f = fopen(argv[0], "w+");
        }
    }

    d = conf_get_doc();
    rm = get_rm_node(d);

    load_resource_rules(agentpath, &rulelist);
    if (!rulelist) {
        fprintf(stderr, "No resource rules available\n");
        goto out;
    }
    load_resources(&reslist, &rulelist);
    build_resource_tree(&tree, &rulelist, &reslist);
    if (!tree) {
        fprintf(stderr, "No resource trees defined; nothing to do\n");
        goto out;
    }
#ifdef DEBUG
    fprintf(stderr, "Resources %p tree %p\n", reslist, tree);
#endif

    shift();

    list_do(&tree, rn) {
        n = NULL;

        curres = rn->rn_resource;

#ifdef DEBUG
        fprintf(stderr, "Flatten %s:%s ... \n", curres->r_rule->rr_type,
                curres->r_attrs[0].ra_value);
#endif
        if (res_flatten(&n, new_rb, &tree, curres)) {
            fprintf(stderr, "FAIL 1\n");
            ret = -1;
            goto out;
        }

        if (replace_resource(rm, curres->r_rule->rr_type,
                             curres->r_attrs[0].ra_name, curres->r_attrs[0].ra_value, n) != 0) {
            fprintf(stderr, "FAIL 2\n");
            ret = -1;
            goto out;
        }

    }
    while (!list_done(&tree, rn)) ;

    remove_resources_block(rm);
    if (new_rb) {
        xmlAddChild(rm, new_rb);
    }

    xmlDocFormatDump(f, d, 1);
    if (f != stdout)
        fclose(f);

  out:
    d = NULL;
    conf_close();
    destroy_resource_tree(&tree);
    destroy_resources(&reslist);
    destroy_resource_rules(&rulelist);

    return ret;
}

static void
usage(const char *arg0, int ret)
{
    fprintf(stderr, "usage: %s <input.conf> [output.conf] [-r]\n\n", arg0);
    fprintf(stderr, "-r\t\tseparate child resources (as when reusing them)\n");
    exit(ret);
}

int
main(int argc, char **argv)
{
    char *arg0 = basename(argv[0]);
    int ret = 0;

    if (argc < 2) {
        usage(arg0, 1);
    }

    if (!strcasecmp(argv[1], "-h") || !strcasecmp(argv[1], "-?")) {
        usage(arg0, 0);
    }

    xmlInitParser();
    xmlIndentTreeOutput = 1;
    xmlKeepBlanksDefault(0);

    shift();
    ret = flatten(argc, argv);

    xmlCleanupParser();

    return ret;
}
