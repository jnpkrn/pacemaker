/*
  Copyright Red Hat, Inc. 2011-2012

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
#include <string.h>
#include <libgen.h>

#define shift() {++argv; --argc;}

static int
disable_rgmanager(xmlDocPtr doc)
{
    char buf[32];
    xmlNodePtr n, o;
    char *a;
    int x;

    if (!doc)
        return 0;
    n = xmlDocGetRootElement(doc);
    if (!n)
        return 0;
    if (strcmp((char *)n->name, "cluster")) {
        fprintf(stderr, "Expected cluster tag, got %s\n", (char *)n->name);
        return 0;
    }

    a = (char *)xmlGetProp(n, (xmlChar *) "config_version");
    if (!a) {
        fprintf(stderr, "No config_version found on cluster tag\n");
        return 0;
    }

    x = atoi(a);
    if (x == 0) {
        fprintf(stderr, "config_version was invalid\n");
        return 0;
    }

    ++x;
    snprintf(buf, sizeof(buf), "%d", x);
    if (xmlSetProp(n, (xmlChar *) "config_version", (xmlChar *) buf) == NULL) {
        fprintf(stderr, "Failed to update config_version\n");
        return 0;
    }

    for (o = n->xmlChildrenNode; o; o = o->next) {
        if (o->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((char *)o->name, "rm"))
            break;
    }

    if (!o)
        return 0;

    if (xmlSetProp(o, (xmlChar *) "disabled", (xmlChar *) "1") == NULL) {
        fprintf(stderr, "Failed to disable rgmanager\n");
        return 0;
    }

    return 1;
}

static void
usage(const char *arg0, int ret)
{
    fprintf(stderr, "usage: %s <input.conf> [output.conf]\n", arg0);
    exit(ret);
}

int
main(int argc, char **argv)
{
    char *arg0 = basename(argv[0]);
    int ret = 0;
    xmlDocPtr doc = NULL;

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
    doc = xmlParseFile(argv[0]);

    if (disable_rgmanager(doc)) {
        xmlDocFormatDump(stdout, doc, 1);
    }

    if (doc)
        xmlFreeDoc(doc);
    xmlCleanupParser();
    return ret;
}
