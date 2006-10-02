/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hid.h"
#include "../hidint.h"

#include "global.h"
#include "misc.h"

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>
#endif

RCSID ("$Id$");

#define HID_DEF(x) extern void hid_ ## x ## _init(void);
#include "hid/common/hidlist.h"
#undef HID_DEF

HID **hid_list = 0;
int hid_num_hids = 0;

extern HID hid_nogui;

HID *gui = &hid_nogui;

int pixel_slop = 1;

static void
hid_load_dir (char *dirname)
{
  DIR *dir;
  struct dirent *de;

  dir = opendir (dirname);
  if (!dir)
    {
      free (dirname);
      return;
    }

  while ((de = readdir (dir)) != NULL)
    {
      void *sym;
      void (*symv)();
      void *so;
      char *basename, *path, *symname;
      struct stat st;

      basename = strdup (de->d_name);
      if (strcasecmp (basename+strlen(basename)-3, ".so") == 0)
	basename[strlen(basename)-3] = 0;
      else if (strcasecmp (basename+strlen(basename)-4, ".dll") == 0)
	basename[strlen(basename)-4] = 0;
      path = Concat (dirname, "/", de->d_name, NULL);

      if (stat (path, &st) == 0
	  && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
	  && S_ISREG (st.st_mode))
	{
	  if ((so = dlopen (path, RTLD_NOW)) == NULL)
	    {
	      fprintf(stderr, "dl_error: %s\n", dlerror ());
	      continue;
	    }

	  symname = Concat ("hid_", basename, "_init", NULL);
	  if ((sym = dlsym (so, symname)) != NULL)
	    {
	      symv = (void (*)()) sym;
	      symv();
	    }
	  else if ((sym = dlsym (so, "pcb_plugin_init")) != NULL)
	    {
	      symv = (void (*)()) sym;
	      symv();
	    }
	  free (symname);
	}
      free (basename);
      free (path);
    }
  free (dirname);
}

void
hid_init ()
{
  char *home;

  gui = &hid_nogui;
#define HID_DEF(x) hid_ ## x ## _init();
#include "hid/common/hidlist.h"
#undef HID_DEF

  hid_load_dir (Concat (EXECPREFIXDIR, "/lib/pcb/plugins/", HOST, NULL));
  hid_load_dir (Concat (EXECPREFIXDIR, "/lib/pcb/plugins", NULL));
  home = getenv("HOME");
  if (home)
    {
      hid_load_dir (Concat (home, "/.pcb/plugins/", HOST, NULL));
      hid_load_dir (Concat (home, "/.pcb/plugins", NULL));
    }
  hid_load_dir (Concat ("plugins/", HOST, NULL));
  hid_load_dir (Concat ("plugins", NULL));
}

void
hid_register_hid (HID * hid)
{
  int i;
  int sz = (hid_num_hids + 2) * sizeof (HID *);

  if (hid->struct_size != sizeof (HID))
    {
      fprintf (stderr, "Warning: hid \"%s\" has an incompatible ABI.\n",
	       hid->name);
      return;
    }

  for (i=0; i<hid_num_hids; i++)
    if (hid == hid_list[i])
      return;

  hid_num_hids++;
  if (hid_list)
    hid_list = (HID **) realloc (hid_list, sz);
  else
    hid_list = (HID **) malloc (sz);

  hid_list[hid_num_hids - 1] = hid;
  hid_list[hid_num_hids] = 0;
}

static void (*gui_start) (int *, char ***) = 0;
static HID *default_gui = 0;

void
hid_register_gui (HID * Pgui, void (*func) (int *, char ***))
{
  if (gui_start)
    return;

  default_gui = Pgui;
  gui_start = func;
}


HID *
hid_find_gui ()
{
  int i;

  for (i = 0; i < hid_num_hids; i++)
    if (!hid_list[i]->printer && !hid_list[i]->exporter)
      return hid_list[i];

  fprintf (stderr, "Error: No GUI available.\n");
  exit (1);
}

HID *
hid_find_printer ()
{
  int i;

  for (i = 0; i < hid_num_hids; i++)
    if (hid_list[i]->printer)
      return hid_list[i];

  return 0;
}

HID *
hid_find_exporter (const char *which)
{
  int i;

  for (i = 0; i < hid_num_hids; i++)
    if (hid_list[i]->exporter && strcmp (which, hid_list[i]->name) == 0)
      return hid_list[i];

  fprintf (stderr, "Invalid exporter %s, available ones:", which);
  for (i = 0; i < hid_num_hids; i++)
    if (hid_list[i]->exporter)
      fprintf (stderr, " %s", hid_list[i]->name);
  fprintf (stderr, "\n");

  return 0;
}

HID **
hid_enumerate ()
{
  return hid_list;
}

HID_AttrNode *hid_attr_nodes = 0;

void
hid_register_attributes (HID_Attribute * a, int n)
{
  HID_AttrNode *ha;

  /* printf("%d attributes registered\n", n); */
  ha = (HID_AttrNode *) malloc (sizeof (HID_AttrNode));
  ha->next = hid_attr_nodes;
  hid_attr_nodes = ha;
  ha->attributes = a;
  ha->n = n;
}

void
hid_parse_command_line (int *argc, char ***argv)
{
  HID_AttrNode *ha;
  int i, e, ok;

  (*argc)--;
  (*argv)++;

  for (ha = hid_attr_nodes; ha; ha = ha->next)
    for (i = 0; i < ha->n; i++)
      {
	HID_Attribute *a = ha->attributes + i;
	switch (a->type)
	  {
	  case HID_Label:
	    break;
	  case HID_Integer:
	    if (a->value)
	      *(int *) a->value = a->default_val.int_value;
	    break;
	  case HID_Boolean:
	    if (a->value)
	      *(char *) a->value = a->default_val.int_value;
	    break;
	  case HID_Real:
	    if (a->value)
	      *(double *) a->value = a->default_val.real_value;
	    break;
	  case HID_String:
	    if (a->value)
	      *(char **) a->value = a->default_val.str_value;
	    break;
	  case HID_Enum:
	    if (a->value)
	      *(int *) a->value = a->default_val.int_value;
	    break;
	  default:
	    abort ();
	  }
      }

  while (*argc && (*argv)[0][0] == '-' && (*argv)[0][1] == '-')
    {
      for (ha = hid_attr_nodes; ha; ha = ha->next)
	for (i = 0; i < ha->n; i++)
	  if (strcmp ((*argv)[0] + 2, ha->attributes[i].name) == 0)
	    {
	      HID_Attribute *a = ha->attributes + i;
	      char *ep;
	      switch (ha->attributes[i].type)
		{
		case HID_Label:
		  break;
		case HID_Integer:
		  if (a->value)
		    *(int *) a->value = strtol ((*argv)[1], 0, 0);
		  else
		    a->default_val.int_value = strtol ((*argv)[1], 0, 0);
		  (*argc)--;
		  (*argv)++;
		  break;
		case HID_Real:
		  if (a->value)
		    *(double *) a->value = strtod ((*argv)[1], 0);
		  else
		    a->default_val.real_value = strtod ((*argv)[1], 0);
		  (*argc)--;
		  (*argv)++;
		  break;
		case HID_String:
		  if (a->value)
		    *(char **) a->value = (*argv)[1];
		  else
		    a->default_val.str_value = (*argv)[1];
		  (*argc)--;
		  (*argv)++;
		  break;
		case HID_Boolean:
		  if (a->value)
		    *(char *) a->value = 1;
		  else
		    a->default_val.int_value = 1;
		  break;
		case HID_Mixed:
		  abort ();
		  a->default_val.real_value = strtod ((*argv)[1], &ep);
		  goto do_enum;
		case HID_Enum:
		  ep = (*argv)[1];
		do_enum:
		  ok = 0;
		  for (e = 0; a->enumerations[e]; e++)
		    if (strcmp (a->enumerations[e], (*argv)[1]) == 0)
		      {
			ok = 1;
			a->default_val.int_value = e;
			a->default_val.str_value = ep;
			break;
		      }
		  if (!ok)
		    {
		      fprintf (stderr,
			       "ERROR:  \"%s\" is an unknown value for the --%s option\n",
			       (*argv)[1], a->name);
		      exit (1);
		    }
		  (*argc)--;
		  (*argv)++;
		  break;
		case HID_Path:
		  abort ();
		  a->default_val.str_value = (*argv)[1];
		  (*argc)--;
		  (*argv)++;
		  break;
		}
	      (*argc)--;
	      (*argv)++;
	      ha = 0;
	      goto got_match;
	    }
      fprintf (stderr, "unrecognized option: %s\n", (*argv)[0]);
      exit (1);
    got_match:;
    }

  (*argc)++;
  (*argv)--;
}

static int
attr_hash (HID_Attribute *a)
{
  unsigned char *cp = (unsigned char *)a;
  int i, rv=0;
  for (i=0; i<(int)((char *)&(a->hash) - (char *)a); i++)
    rv = (rv * 13) ^ (rv >> 16) ^ cp[i];
  return rv;
}

void
hid_save_settings (int locally)
{
  char *home, *fname;
  struct stat st;
  FILE *f;
  HID_AttrNode *ha;
  int i;

  if (locally)
    {
      fname = Concat ("pcb.settings", NULL);
    }
  else
    {
      home = getenv ("HOME");
      if (! home)
	return;
      fname = Concat (home, "/.pcb", NULL);

      if (stat (fname, &st))
	if (mkdir (fname, 0777))
	  {
	    free (fname);
	    return;
	  }
      free (fname);

      fname = Concat (home, "/.pcb/settings", NULL);
    }

  f = fopen (fname, "w");
  if (!f)
    {
      Message ("Can't open %s", fname);
      free (fname);
      return;
    }

  for (ha = hid_attr_nodes; ha; ha = ha->next)
    {
      for (i = 0; i < ha->n; i++)
	{
	  char *str;
	  HID_Attribute *a = ha->attributes + i;

	  if (a->hash == attr_hash (a))
	    fprintf (f, "# ");
	  switch (a->type)
	    {
	    case HID_Label:
	      break;
	    case HID_Integer:
	      fprintf (f, "%s = %d\n",
		       a->name,
		       a->value ? *(int *)a->value : a->default_val.int_value);
	      break;
	    case HID_Boolean:
	      fprintf (f, "%s = %d\n",
		       a->name,
		       a->value ? *(char *)a->value : a->default_val.int_value);
	      break;
	    case HID_Real:
	      fprintf (f, "%s = %f\n",
		       a->name,
		       a->value ? *(double *)a->value : a->default_val.real_value);
	      break;
	    case HID_String:
	    case HID_Path:
	      str = a->value ? *(char **)a->value : a->default_val.str_value;
	      fprintf (f, "%s = %s\n",
		       a->name,
		       str ? str : "");
	      break;
	    case HID_Enum:
	      fprintf (f, "%s = %s\n",
		       a->name,
		       a->enumerations[a->value ? *(int *)a->value : a->default_val.int_value]);
	      break;
	    }
	}
      fprintf (f, "\n");
    }
  fclose (f);
  free (fname);
}

static void
hid_set_attribute (char *name, char *value)
{
  HID_AttrNode *ha;
  int i, e, ok;

  for (ha = hid_attr_nodes; ha; ha = ha->next)
    for (i = 0; i < ha->n; i++)
      if (strcmp (name, ha->attributes[i].name) == 0)
	{
	  HID_Attribute *a = ha->attributes + i;
	  char *ep;
	  switch (ha->attributes[i].type)
	    {
	    case HID_Label:
	      break;
	    case HID_Integer:
	      a->default_val.int_value = strtol (value, 0, 0);
	      break;
	    case HID_Real:
	      a->default_val.real_value = strtod (value, 0);
	      break;
	    case HID_String:
	      a->default_val.str_value = strdup (value);
	      break;
	    case HID_Boolean:
	      a->default_val.int_value = 1;
	      break;
	    case HID_Mixed:
	      abort ();
	      a->default_val.real_value = strtod (value, &value);
	      /* fall through */
	    case HID_Enum:
	      ok = 0;
	      for (e = 0; a->enumerations[e]; e++)
		if (strcmp (a->enumerations[e], value) == 0)
		  {
		    ok = 1;
		    a->default_val.int_value = e;
		    a->default_val.str_value = value;
		    break;
		  }
	      if (!ok)
		{
		  fprintf (stderr,
			   "ERROR:  \"%s\" is an unknown value for the %s option\n",
			   value, a->name);
		  exit (1);
		}
	      break;
	    case HID_Path:
	      a->default_val.str_value = value;
	      break;
	    }
	}
}

static void
hid_load_settings_1 (char *fname)
{
  char line[1024], *namep, *valp, *cp;
  FILE *f;

  f = fopen (fname, "r");
  if (!f)
    {
      free (fname);
      return;
    }

  free (fname);
  while (fgets (line, sizeof(line), f) != NULL)
    {
      for (namep=line; *namep && isspace (*namep); namep++)
	;
      if (*namep == '#')
	continue;
      for (valp=namep; *valp && !isspace(*valp); valp++)
	;
      if (! *valp)
	continue;
      *valp++ = 0;
      while (*valp && (isspace (*valp) || *valp == '='))
	valp ++;
      if (! *valp)
	continue;
      cp = valp + strlen(valp) - 1;
      while (cp >= valp && isspace (*cp))
	*cp-- = 0;
      hid_set_attribute (namep, valp);
    }

  fclose (f);
}

void
hid_load_settings ()
{
  char *home, *fname;
  HID_AttrNode *ha;
  int i;

  for (ha = hid_attr_nodes; ha; ha = ha->next)
    for (i = 0; i < ha->n; i++)
      ha->attributes[i].hash = attr_hash (ha->attributes+i);

  hid_load_settings_1 (Concat (PCBLIBDIR, "/settings", NULL));
  home = getenv("HOME");
  if (home)
    hid_load_settings_1 (Concat (home, "/.pcb/settings", NULL));
  hid_load_settings_1 (Concat ("pcb.settings", NULL));
}

#define HASH_SIZE 32

typedef struct ecache
{
  struct ecache *next;
  const char *name;
  hidval val;
} ecache;

typedef struct ccache
{
  ecache *colors[HASH_SIZE];
  ecache *lru;
} ccache;

static void
copy_color (int set, hidval * cval, hidval * aval)
{
  if (set)
    memcpy (cval, aval, sizeof (hidval));
  else
    memcpy (aval, cval, sizeof (hidval));
}

int
hid_cache_color (int set, const char *name, hidval * val, void **vcache)
{
  int hash;
  const char *cp;
  ccache *cache;
  ecache *e;

  cache = (ccache *) * vcache;
  if (cache == 0)
    cache = *vcache = (void *) calloc (sizeof (ccache), 1);

  if (cache->lru && strcmp (cache->lru->name, name) == 0)
    {
      copy_color (set, &(cache->lru->val), val);
      return 1;
    }

  for (cp = name, hash = 0; *cp; cp++)
    hash += (*cp) & 0xff;
  hash %= HASH_SIZE;

  for (e = cache->colors[hash]; e; e = e->next)
    if (strcmp (e->name, name) == 0)
      {
	copy_color (set, &(e->val), val);
	return 1;
      }
  if (!set)
    return 0;

  e = (ecache *) malloc (sizeof (ecache));
  e->next = cache->colors[hash];
  cache->colors[hash] = e;
  e->name = strdup (name);
  memcpy (&(e->val), val, sizeof (hidval));

  return 1;
}

/* otherwise homeless function, refactored out of the five export HIDs */
void
derive_default_filename(const char *pcbfile, HID_Attribute *filename_attrib, const char *suffix, char **memory)
{
	char *buf;
	char *pf;

	if (pcbfile == NULL)
	  pf = strdup ("unknown.pcb");
	else
	  pf = strdup (pcbfile);

	if (!pf || (memory && filename_attrib->default_val.str_value != *memory)) return;

	buf = malloc (strlen (pf) + strlen(suffix) + 1);
	if (memory) *memory = buf;
	if (buf) {
		size_t bl;
		strcpy (buf, pf);
		bl = strlen(buf);
		if (bl > 4 && strcmp (buf + bl - 4, ".pcb") == 0)
			buf[bl - 4] = 0;
		strcat(buf, suffix);
		if (filename_attrib->default_val.str_value)
			free(filename_attrib->default_val.str_value);
		filename_attrib->default_val.str_value = buf;
	}

	free (pf);
}
