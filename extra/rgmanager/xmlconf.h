#ifndef _RG_XMLCONF_H
#  define _RG_XMLCONF_H

int conf_open(void);
int conf_close(void);
void conf_setconfig(const char *path);
int conf_get(char *path, char **value);
xmlDocPtr conf_get_doc(void);
char *xpath_get_one(xmlDocPtr doc, xmlXPathContextPtr ctx, char *query);

#endif
