/*
 *                            COPYRIGHT
 *
 *  PCB, interactive printed circuit board design
 *  Copyright (C) 1994,1995,1996 Thomas Nau
 *  Copyright (C) 1998,1999,2000,2001 harry eaton
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Contact addresses for paper mail and Email:
 *  harry eaton, 6697 Buttonhole Ct, Columbia, MD 21044 USA
 *  haceaton@aplcomm.jhuapl.edu
 *
 */

/* this file, autoroute.c, was written and is
 * Copyright (c) 2001 C. Scott Ananian
 */

/* functions used to autoroute nets.
 */

/*
 *-------------------------------------------------------------------
 * This file implements a rectangle-expansion router, based on
 * "A Method for Gridless Routing of Printed Circuit Boards" by
 * A. C. Finch, K. J. Mackenzie, G. J. Balsdon, and G. Symonds in the
 * 1985 Proceedings of the 22nd ACM/IEEE Design Automation Conference.
 * This reference is available from the ACM Digital Library at
 * http://www.acm.org/dl for those with institutional or personal
 * access to it.  It's also available from your local engineering
 * library.
 *--------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdlib.h>
#include <setjmp.h>

#include "data.h"
#include "global.h"
#include "macro.h"

#include "autoroute.h"
#include "box.h"
#include "create.h"
#include "draw.h"
#include "error.h"
#include "heap.h"
#include "kdtree.h"
#include "misc.h"
#include "mtspace.h"
#include "mymem.h"
#include "polygon.h"		/* for UpdatePIPFlags */
#include "rats.h"
#include "remove.h"
#include "undo.h"
#include "vector.h"

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>
#endif

/* #defines to enable some debugging output */
#define ROUTE_VERBOSE
/* #define ROUTE_DEBUG
   #define DEBUG_SHOW_ROUTE_BOXES
   #define DEBUG_SHOW_EXPANSION_BOXES
   #define DEBUG_SHOW_VIA_BOXES
*/

/* round up "half" thicknesses */
#define HALF_THICK(x) (((x)+1)/2)
/* a styles maximum bloat is its keepaway plus the larger of its via radius
 * or line half-thickness. */
#define BLOAT(style)\
	((style)->Keepaway + HALF_THICK(MAX((style)->Thick,(style)->Diameter)))
/* conflict penalty is less for traces laid down during previous pass than
 * it is for traces already laid down in this pass. */
#define CONFLICT_LEVEL(rb)\
	(((rb)->flags.is_odd==AutoRouteParameters.is_odd) ?\
	 HI_CONFLICT : LO_CONFLICT )
#define CONFLICT_PENALTY(rb)\
	(CONFLICT_LEVEL(rb)==HI_CONFLICT ? \
	 AutoRouteParameters.ConflictPenalty : \
	 CONFLICT_LEVEL(rb)==LO_CONFLICT ? \
	 AutoRouteParameters.LastConflictPenalty : 1)

#define ABS(x) (((x)<0)?-(x):(x))

#define LIST_LOOP(init, which, x, command) do {\
   x = (init);\
   do {\
     /* save next one first in case the command modifies or frees it */\
     routebox_t *__next_one__ = x->which.next;\
     command;\
     x = __next_one__;\
   } while (x != (init));\
} while (0)

#define FOREACH_SUBNET(net, p, command) do {\
  routebox_t *_pp_;\
  /* fail-fast: check subnet_processed flags */\
  LIST_LOOP(net, same_net, p, assert(!p->flags.subnet_processed));\
  /* iterate through *distinct* subnets */\
  LIST_LOOP(net, same_net, p, if (!p->flags.subnet_processed) {\
    LIST_LOOP(p, same_subnet, _pp_, _pp_->flags.subnet_processed=1);\
    command;\
  });\
  /* reset subnet_processed flags */\
  LIST_LOOP(net, same_net, p, p->flags.subnet_processed=0);\
} while (0)

/* notes:
 * all rectangles are assumed to be closed on the top and left and
 * open on the bottom and right.   That is, they include their top-left
 * corner but don't include their bottom-right corner.
 *
 * Obstacles, however, are thought of to be closed on all sides,
 * and exclusion zone *open* on all sides.  We will ignore the infintesimal
 * difference w.r.t. obstacles, but exclusion zones will be consistently
 * bumped in one unit on the top and left in order to exclose the same
 * integer coordinates as their open-rectangle equivalents.
 *
 * expansion regions are always half-closed.  This means that when
 * tracing paths, you must steer clear of the bottom and right edges.
 */

/* ---------------------------------------------------------------------------
 * some local types
 */

/* augmented RouteStyleType */
typedef struct
{
  /* a routing style */
  const RouteStyleType *style;
  /* a via-area structure appropriate for the style's
   * via radius and keepaway */
  mtspace_t *mtspace;
  /* flag indicating whether this augmented style is ever used.
   * We only update mtspace if the style is used somewhere in the netlist */
  Boolean Used;
}
AugmentedRouteStyleType;

/* enumerated type for conflict levels */
typedef enum
{ NO_CONFLICT = 0, LO_CONFLICT = 1, HI_CONFLICT = 2 }
conflict_t;

typedef struct routebox
{
  const BoxType box;
  Cardinal group;		/* layer group this box belongs to */
  enum
  { PAD, PIN, VIA, VIA_SHADOW, LINE, OTHER, EXPANSION_AREA }
  type;
  union
  {
    PadTypePtr pad;
    PinTypePtr pin;
    PinTypePtr via;
    struct routebox *via_shadow;	/* points to the via in kd-tree which
					 * points to the PinType in the PCB. */
    LineTypePtr line;
    void *generic;		/* 'other' is polygon, arc, text */
    struct routebox *expansion_area;	/* previous expansion area in search */
  }
  parent;
  struct
  {
    unsigned nonstraight:1;
    unsigned fixed:1;
    /* for searches */
    unsigned source:1;
    unsigned target:1;
    /* rects on same net as source and target don't need clearance areas */
    unsigned nobloat:1;
    /* mark circular pins, so that we be sure to connect them up properly */
    unsigned circular:1;
    /* we sometimes create routeboxen that don't actually belong to a
     * kd-tree yet -- make sure refcount of orphans is set properly */
    unsigned orphan:1;
    /* was this nonfixed obstacle generated on an odd or even pass? */
    unsigned is_odd:1;
    /* fixed route boxes that have already been "routed through" in this
     * search have their "touched" flag set. */
    unsigned touched:1;
    /* this is a status bit for iterating through *different* subnets */
    unsigned subnet_processed:1;
    /* some expansion_areas represent via candidates */
    unsigned is_via:1;
    /* mark non-straight lines which go from bottom-left to upper-right,
     * instead of from upper-left to bottom-right. */
    unsigned bl_to_ur:1;
    /* mark polygons which are "transparent" for via-placement; that is,
     * vias through the polygon will automatically be given a keepaway
     * and will not electrically connect to the polygon. */
    unsigned clear_poly:1;
    /* this markes "conflicting" routes that must be torn up to obtain
     * a correct routing.  This flag allows us to return a correct routing
     * even if the user cancels auto-route after a non-final pass. */
    unsigned is_bad:1;
    /* for assertion that 'box' is never changed after creation */
    unsigned inited:1;
  }
  flags;
  /* reference count for orphan routeboxes; free when refcount==0 */
  int refcount;
  /* when routing with conflicts, we keep a record of what we're
   * conflicting *with*. */
  struct routebox *underlying;
  /* route style of the net associated with this routebox */
  AugmentedRouteStyleType *augStyle;
  /* circular lists with connectivity information. */
  struct routebox_list
  {
    struct routebox *next, *prev;
  }
  same_net, same_subnet, original_subnet, different_net;
}
routebox_t;

typedef struct routedata
{
  /* one kdtree per layer *group */
  kdtree_t *layergrouptree[MAX_LAYER];	/* no silkscreen layers here =) */
  /* root pointer into connectivity information */
  routebox_t *first_net;
  /* default routing style */
  RouteStyleType defaultStyle;
  /* augmented style structures */
  AugmentedRouteStyleType augStyles[NUM_STYLES + 1];
  /* what is the maximum bloat (keepaway+line half-width or
   * keepaway+via_radius) for any style we've seen? */
  Dimension max_bloat;
}
routedata_t;

typedef struct edge_struct
{
  routebox_t *rb;		/* path expansion edges are real routeboxen. */
  PointType cost_point;
  cost_t cost_to_point;		/* from source */
  routebox_t *mincost_target;	/* minimum cost from cost_point to any target */
  direction_t expand_dir;	/* ignored if expand_all_sides is set */
  struct
  {
    /* ignore expand_dir and expand all sides if this is set. */
    /* used for vias and the initial source objects */
    unsigned expand_all_sides:1;	/* XXX: this is redundant with is_via? */
    /* this indicates that this 'edge' is a via candidate. */
    unsigned is_via:1;
    /* record "conflict level" of via candidates, in case we need to split
     * them later. */
    conflict_t via_conflict_level:2;
    /* when "routing with conflicts", sometimes edge is interior. */
    unsigned is_interior:1;
  }
  flags;
}
edge_t;

static struct
{
  /* net style parameters */
  AugmentedRouteStyleType *augStyle;
  /* cost parameters */
  cost_t ViaCost,		/* additional "length" cost for using a via */
    LastConflictPenalty,	/* length mult. for routing over last pass' trace */
    ConflictPenalty,		/* length multiplier for routing over another trace */
    JogPenalty,			/* additional "length" cost for changing direction */
    DirectionPenaltyNumerator,	/* (rational) length multiplier for routing in */
    DirectionPenaltyDenominator;	/* a non-preferred direction on a layer */
  /* are vias allowed? */
  Boolean use_vias;
  /* is this an odd or even pass? */
  Boolean is_odd;
  /* permit conflicts? */
  Boolean with_conflicts;
  /* is this a final "smoothing" pass? */
  Boolean is_smoothing;
}
AutoRouteParameters;

/* ---------------------------------------------------------------------------
 * some local prototypes
 */
static routebox_t *CreateExpansionArea (const BoxType * area, Cardinal group,
					routebox_t * parent,
					Boolean relax_edge_requirements);

static cost_t edge_cost (const edge_t * e);

static BoxType edge_to_box (const BoxType * box, direction_t expand_dir);

static void ResetSubnet (routebox_t * net);

/* ---------------------------------------------------------------------------
 * some local identifiers
 */

/* assertion helper for routeboxen */
#ifndef NDEBUG
static int
__routebox_is_good (routebox_t * rb)
{
  int r = rb && (0 <= rb->group) && (rb->group < MAX_LAYER) &&
    (rb->box.X1 <= rb->box.X2) && (rb->box.Y1 <= rb->box.Y2) &&
    (rb->flags.orphan ?
     (rb->box.X1 != rb->box.X2) || (rb->box.Y1 != rb->box.Y2) :
     (rb->box.X1 != rb->box.X2) && (rb->box.Y1 != rb->box.Y2)) &&
    (rb->flags.source ? rb->flags.nobloat : 1) &&
    (rb->flags.target ? rb->flags.nobloat : 1) &&
    (rb->flags.orphan ? !rb->flags.touched : rb->refcount == 0) &&
    (rb->flags.touched ? rb->type != EXPANSION_AREA : 1) &&
    (rb->flags.is_odd ?
     (!rb->flags.fixed) &&
     (rb->type == VIA || rb->type == VIA_SHADOW || rb->type == LINE)
     : 1) &&
    (rb->flags.bl_to_ur ? rb->flags.nonstraight : 1) &&
    (rb->flags.clear_poly ?
     (rb->type == OTHER && rb->flags.fixed && !rb->flags.orphan) :
     1) &&
    (rb->underlying == NULL || !rb->underlying->flags.orphan) &&
    rb->flags.inited &&
    rb->augStyle != NULL && rb->augStyle->style != NULL &&
    rb->same_net.next && rb->same_net.prev &&
    rb->same_subnet.next && rb->same_subnet.prev &&
    rb->original_subnet.next && rb->original_subnet.prev &&
    rb->different_net.next && rb->different_net.prev && 1;
  assert (r);
  return r;
}
static int
__edge_is_good (edge_t * e)
{
  int r = e && e->rb &&
    __routebox_is_good (e->rb) &&
    (e->rb->flags.orphan ? e->rb->refcount > 0 : 1) &&
    (e->rb->flags.source ? e->cost_to_point == 0 : 1) &&
    e->mincost_target &&
    (0 <= e->expand_dir) && (e->expand_dir < 4) &&
    (e->flags.is_interior ?
     (e->flags.expand_all_sides && e->rb->underlying) : 1) &&
    (e->flags.is_via ? e->rb->flags.is_via : 1) &&
    (e->flags.via_conflict_level >= 0 && e->flags.via_conflict_level <= 2) &&
    (e->flags.via_conflict_level != 0 ? e->flags.is_via : 1) &&
    (e->cost_to_point >= 0) && (edge_cost (e) >= 0) && 1;
  assert (r);
  return r;
}
#endif /* !NDEBUG */

/*---------------------------------------------------------------------
 * route utility functions.
 */

enum boxlist
{ NET, SUBNET, ORIGINAL, DIFFERENT_NET };
static struct routebox_list *
__select_list (routebox_t * r, enum boxlist which)
{
  assert (r);
  switch (which)
    {
    default:
      assert (0);
    case NET:
      return &(r->same_net);
    case SUBNET:
      return &(r->same_subnet);
    case ORIGINAL:
      return &(r->original_subnet);
    case DIFFERENT_NET:
      return &(r->different_net);
    }
}
static void
InitLists (routebox_t * r)
{
  static enum boxlist all[] =
  { NET, SUBNET, ORIGINAL, DIFFERENT_NET }
  , *p;
  for (p = all; p < all + (sizeof (all) / sizeof (*p)); p++)
    {
      struct routebox_list *rl = __select_list (r, *p);
      rl->prev = rl->next = r;
    }
}

static void
MergeNets (routebox_t * a, routebox_t * b, enum boxlist which)
{
  struct routebox_list *al, *bl, *anl, *bnl;
  routebox_t *an, *bn;
  assert (a && b);
  assert (a != b);
  al = __select_list (a, which);
  bl = __select_list (b, which);
  assert (al && bl);
  an = al->next;
  bn = bl->next;
  assert (an && bn);
  anl = __select_list (an, which);
  bnl = __select_list (bn, which);
  assert (anl && bnl);
  bl->next = an;
  anl->prev = b;
  al->next = bn;
  bnl->prev = a;
}

static void
RemoveFromNet (routebox_t * a, enum boxlist which)
{
  struct routebox_list *al, *anl, *apl;
  routebox_t *an, *ap;
  assert (a);
  al = __select_list (a, which);
  assert (al);
  an = al->next;
  ap = al->prev;
  if (an == a || ap == a)
    return;			/* not on any list */
  assert (an && ap);
  anl = __select_list (an, which);
  apl = __select_list (ap, which);
  assert (anl && apl);
  anl->prev = ap;
  apl->next = an;
  al->next = al->prev = a;
  return;
}

static void
init_const_box (routebox_t * rb,
		Position X1, Position Y1, Position X2, Position Y2)
{
  BoxType *bp = (BoxType *) & rb->box;	/* note discarding const! */
  assert (!rb->flags.inited);
  assert (X1 <= X2 && Y1 <= Y2);
  bp->X1 = X1;
  bp->Y1 = Y1;
  bp->X2 = X2;
  bp->Y2 = Y2;
  rb->flags.inited = 1;
}

/*---------------------------------------------------------------------
 * routedata initialization functions.
 */

static Boolean
is_layer_group_active (Cardinal group)
{
  int i;
  assert (0 <= group && group < MAX_LAYER);
  for (i = 0; i < PCB->LayerGroups.Number[group]; i++)
    /* layer must be 1) not silk (ie, < MAX_LAYER) and 2) on */
    if ((PCB->LayerGroups.Entries[group][i] < MAX_LAYER) &&
	PCB->Data->Layer[PCB->LayerGroups.Entries[group][i]].On)
      return True;		/* this layer group is active. */
  return False;
}

static void
AddPin (PointerListType layergroupboxes[], PinTypePtr pin, Boolean is_via)
{
  routebox_t **rbpp, *lastrb = NULL;
  int i;
  /* a pin cuts through every layer group */
  for (i = 0; i < MAX_LAYER; i++)
    {
      rbpp = (routebox_t **) GetPointerMemory (&layergroupboxes[i]);
      *rbpp = calloc (1, sizeof (**rbpp));
      (*rbpp)->group = i;
      init_const_box (*rbpp,
		      /*X1 */ pin->X - HALF_THICK (pin->Thickness),
		      /*Y1 */ pin->Y - HALF_THICK (pin->Thickness),
		      /*X2 */ pin->X + HALF_THICK (pin->Thickness),
		      /*Y2 */ pin->Y + HALF_THICK (pin->Thickness));
      /* set aux. properties */
      if (is_via)
	{
	  (*rbpp)->type = VIA;
	  (*rbpp)->parent.via = pin;
	}
      else
	{
	  (*rbpp)->type = PIN;
	  (*rbpp)->parent.pin = pin;
	}
      (*rbpp)->flags.fixed = 1;
      (*rbpp)->flags.circular = !TEST_FLAG (SQUAREFLAG, pin);
      /* circular lists */
      InitLists (*rbpp);
      /* link together */
      if (lastrb)
	{
	  MergeNets (*rbpp, lastrb, NET);
	  MergeNets (*rbpp, lastrb, SUBNET);
	  MergeNets (*rbpp, lastrb, ORIGINAL);
	}
      lastrb = *rbpp;
    }
  return;
}
static void
AddPad (PointerListType layergroupboxes[],
	ElementTypePtr element, PadTypePtr pad)
{
  Dimension halfthick;
  routebox_t **rbpp;
  int layer = TEST_FLAG (ONSOLDERFLAG, element) ?
    (MAX_LAYER + SOLDER_LAYER) : (MAX_LAYER + COMPONENT_LAYER);
  int layergroup = GetLayerGroupNumberByNumber (layer);
  assert (0 <= layergroup && layergroup < MAX_LAYER);
  assert (PCB->LayerGroups.Number[layergroup] > 0);
  rbpp = (routebox_t **) GetPointerMemory (&layergroupboxes[layergroup]);
  assert (rbpp);
  *rbpp = calloc (1, sizeof (**rbpp));
  assert (*rbpp);
  (*rbpp)->group = layergroup;
  halfthick = HALF_THICK (pad->Thickness);
  init_const_box (*rbpp,
		  /*X1 */ MIN (pad->Point1.X, pad->Point2.X) - halfthick,
		  /*Y1 */ MIN (pad->Point1.Y, pad->Point2.Y) - halfthick,
		  /*X2 */ MAX (pad->Point1.X, pad->Point2.X) + halfthick,
		  /*Y2 */ MAX (pad->Point1.Y, pad->Point2.Y) + halfthick);
  /* kludge for non-manhattan pads */
  if (pad->Point1.X != pad->Point2.X && pad->Point1.Y != pad->Point2.Y)
    (*rbpp)->flags.nonstraight = 1;
  /* set aux. properties */
  (*rbpp)->type = PAD;
  (*rbpp)->parent.pad = pad;
  (*rbpp)->flags.fixed = 1;
  /* circular lists */
  InitLists (*rbpp);
  return;
}
static void
AddLine (PointerListType layergroupboxes[], int layer, LineTypePtr line)
{
  routebox_t **rbpp;
  int layergroup;
  assert (layergroupboxes && line);
  assert (0 <= layer && layer < MAX_LAYER);

  layergroup = GetLayerGroupNumberByNumber (layer);
  assert (0 <= layergroup && layergroup < MAX_LAYER);
  assert (PCB->LayerGroups.Number[layergroup] > 0);

  rbpp = (routebox_t **) GetPointerMemory (&layergroupboxes[layergroup]);
  *rbpp = calloc (1, sizeof (**rbpp));
  (*rbpp)->group = layergroup;
  init_const_box (*rbpp,
		  /*X1 */ MIN (line->Point1.X,
			       line->Point2.X) - HALF_THICK (line->Thickness),
		  /*Y1 */ MIN (line->Point1.Y,
			       line->Point2.Y) - HALF_THICK (line->Thickness),
		  /*X2 */ MAX (line->Point1.X,
			       line->Point2.X) + HALF_THICK (line->Thickness),
		  /*Y2 */ MAX (line->Point1.Y,
			       line->Point2.Y) +
		  HALF_THICK (line->Thickness));
  /* kludge for non-manhattan lines */
  if (line->Point1.X != line->Point2.X && line->Point1.Y != line->Point2.Y)
    {
      (*rbpp)->flags.nonstraight = 1;
      (*rbpp)->flags.bl_to_ur =
	(MIN (line->Point1.X, line->Point2.X) == line->Point1.X) !=
	(MIN (line->Point1.Y, line->Point2.Y) == line->Point1.Y);
    }
  /* set aux. properties */
  (*rbpp)->type = LINE;
  (*rbpp)->parent.line = line;
  (*rbpp)->flags.fixed = 1;
  /* circular lists */
  InitLists (*rbpp);
  return;
}
static routebox_t *
AddIrregularObstacle (PointerListType layergroupboxes[],
		      Position X1, Position Y1,
		      Position X2, Position Y2, Cardinal layer, void *parent)
{
  routebox_t **rbpp;
  int layergroup;
  assert (layergroupboxes && parent);
  assert (0 <= layer && layer < MAX_LAYER);
  assert (X1 <= X2 && Y1 <= Y2);

  layergroup = GetLayerGroupNumberByNumber (layer);
  assert (0 <= layergroup && layergroup < MAX_LAYER);
  assert (PCB->LayerGroups.Number[layergroup] > 0);

  rbpp = (routebox_t **) GetPointerMemory (&layergroupboxes[layergroup]);
  *rbpp = calloc (1, sizeof (**rbpp));
  (*rbpp)->group = layergroup;
  init_const_box (*rbpp, X1, Y1, X2, Y2);
  (*rbpp)->flags.nonstraight = 1;
  (*rbpp)->type = OTHER;
  (*rbpp)->parent.generic = parent;
  (*rbpp)->flags.fixed = 1;
  /* circular lists */
  InitLists (*rbpp);
  return *rbpp;
}

static void
AddPolygon (PointerListType layergroupboxes[], Cardinal layer,
	    PolygonTypePtr polygon)
{
  routebox_t *rb = AddIrregularObstacle (layergroupboxes,
					 polygon->BoundingBox.X1,
					 polygon->BoundingBox.Y1,
					 polygon->BoundingBox.X2,
					 polygon->BoundingBox.Y2,
					 layer, polygon);
  if (TEST_FLAG (CLEARPOLYFLAG, polygon))
    rb->flags.clear_poly = 1;
}
static void
AddText (PointerListType layergroupboxes[], Cardinal layer, TextTypePtr text)
{
  AddIrregularObstacle (layergroupboxes,
			text->BoundingBox.X1, text->BoundingBox.Y1,
			text->BoundingBox.X2, text->BoundingBox.Y2,
			layer, text);
}
static void
AddArc (PointerListType layergroupboxes[], Cardinal layer, ArcTypePtr arc)
{
  AddIrregularObstacle (layergroupboxes,
			arc->BoundingBox.X1, arc->BoundingBox.Y1,
			arc->BoundingBox.X2, arc->BoundingBox.Y2, layer, arc);
}

struct find_closure
{
  routebox_t *match;
  void *key;
};
static int
__found_one (const BoxType * box, void *cl)
{
  struct find_closure *fc = (struct find_closure *) cl;
  routebox_t *rb = (routebox_t *) box;
  if (fc->key == rb->parent.generic)
    {
      fc->match = rb;
      return 1;
    }
  else
    return 0;
}
static routebox_t *
FindRouteBox (routedata_t * rd, Position X, Position Y, void *matches)
{
  struct find_closure fc = { NULL, matches };
  BoxType region;
  int i;
  region.X1 = region.X2 = X;
  region.Y1 = region.Y2 = Y;
  for (i = 0; i < MAX_LAYER; i++)
    if (kd_search (rd->layergrouptree[i], &region, NULL, __found_one, &fc))
      return fc.match;
  return NULL;			/* no match found */
}

static int
__found_one_on_lg (const BoxType * box, void *cl)
{
  routebox_t **rb = (routebox_t **) cl;
  *rb = (routebox_t *) box;
  return 1;
}
static routebox_t *
FindRouteBoxOnLayerGroup (routedata_t * rd,
			  Position X, Position Y, Cardinal layergroup)
{
  routebox_t *rb;
  BoxType region;
  region.X1 = region.X2 = X;
  region.Y1 = region.Y2 = Y;
  if (kd_search (rd->layergrouptree[layergroup], &region, NULL,
		 __found_one_on_lg, &rb))
    return rb;
  return NULL;			/* no match found */
}

#ifdef ROUTE_DEBUG
static void
DumpRouteBox (routebox_t * rb)
{
  printf ("RB: (%d,%d)-(%d,%d) l%d; ",
	  rb->box.X1, rb->box.Y1, rb->box.X2, rb->box.Y2, (int) rb->group);
  switch (rb->type)
    {
    case PAD:
      printf ("PAD[%s %s] ", rb->parent.pad->Name, rb->parent.pad->Number);
      break;
    case PIN:
      printf ("PIN[%s %s] ", rb->parent.pin->Name, rb->parent.pin->Number);
      break;
    case VIA:
      printf ("VIA[%s %s] ", rb->parent.via->Name, rb->parent.via->Number);
      break;
    case LINE:
      printf ("LINE ");
      break;
    case OTHER:
      printf ("OTHER ");
      break;
    case EXPANSION_AREA:
      printf ("EXPAREA ");
      break;
    default:
      printf ("UNKNOWN ");
      break;
    }
  if (rb->flags.nonstraight)
    printf ("(nonstraight) ");
  if (rb->flags.fixed)
    printf ("(fixed) ");
  if (rb->flags.source)
    printf ("(source) ");
  if (rb->flags.target)
    printf ("(target) ");
  if (rb->flags.orphan)
    printf ("(orphan) ");
  printf ("\n");
}
#endif

static routedata_t *
CreateRouteData ()
{
  NetListListType Nets;
  PointerListType layergroupboxes[MAX_LAYER];
  BoxType bbox;
  routedata_t *rd;
  int i, j;

  /* create routedata */
  rd = calloc (1, sizeof (*rd));
  /* create default style */
  rd->defaultStyle.Thick = Settings.LineThickness;
  rd->defaultStyle.Diameter = Settings.ViaThickness;
  rd->defaultStyle.Hole = Settings.ViaDrillingHole;
  rd->defaultStyle.Keepaway = Settings.Keepaway;
  rd->max_bloat = BLOAT (&rd->defaultStyle);
  /* create augStyles structures */
  bbox.X1 = bbox.Y1 = 0;
  bbox.X2 = PCB->MaxWidth;
  bbox.Y2 = PCB->MaxHeight;
  for (i = 0; i < NUM_STYLES + 1; i++)
    {
      RouteStyleType *style =
	(i < NUM_STYLES) ? &PCB->RouteStyle[i] : &rd->defaultStyle;
      rd->augStyles[i].style = style;
      rd->augStyles[i].mtspace = mtspace_create
	(&bbox, HALF_THICK (style->Diameter), HALF_THICK (style->Keepaway));
      rd->augStyles[i].Used = False;
    }

  /* initialize pointerlisttype */
  for (i = 0; i < MAX_LAYER; i++)
    {
      layergroupboxes[i].Ptr = NULL;
      layergroupboxes[i].PtrN = 0;
      layergroupboxes[i].PtrMax = 0;
    }

  /* add pins and pads of elements */
  ALLPIN_LOOP (PCB->Data, AddPin (layergroupboxes, pin, False));
  ALLPAD_LOOP (PCB->Data, AddPad (layergroupboxes, element, pad));
  /* add all vias */
  VIA_LOOP (PCB->Data, AddPin (layergroupboxes, via, True));
  for (i = 0; i < MAX_LAYER; i++)
    {
      /* add all (non-rat) lines */
      LINE_LOOP (&PCB->Data->Layer[i], AddLine (layergroupboxes, i, line));
      /* add all "should-avoid" polygons */
      POLYGON_LOOP (&PCB->Data->Layer[i],
		    AddPolygon (layergroupboxes, i, polygon));
      /* add all copper text */
      TEXT_LOOP (&PCB->Data->Layer[i], AddText (layergroupboxes, i, text));
      /* add all arcs */
      ARC_LOOP (&PCB->Data->Layer[i], AddArc (layergroupboxes, i, arc));
    }

  /* create k-d trees from pointer lists */
  for (i = 0; i < MAX_LAYER; i++)
    {
      /* initialize style (we'll fill in a "real" style later, when we add
       * the connectivity information) */
      POINTER_LOOP (&layergroupboxes[i],
		    /* we're initializing this to the "default" style */
		    ((routebox_t *) * ptr)->augStyle = &rd->augStyles[NUM_STYLES]);
      /* create the kd-tree */
      rd->layergrouptree[i] =
	kd_create_tree ((const BoxType **) layergroupboxes[i].Ptr,
			layergroupboxes[i].PtrN, 1);
    }

  /* now add connectivity/style information */
  Nets = CollectSubnets (False);
  {
    routebox_t *last_net = NULL;
    NETLIST_LOOP (&Nets,
		  {
		  routebox_t * last_in_net = NULL;
		  NET_LOOP (netlist,
			    {
			    routebox_t *
			    last_in_subnet = NULL; CONNECTION_LOOP (net,
								    {
								    routebox_t
								    * rb =
								    FindRouteBox
								    (rd,
								     connection->
								     X,
								     connection->
								     Y,
								     connection->ptr2);
								    assert
								    (rb);
								    /* traces are listed twice, at start point and at end point */
								    if (rb ==
									last_in_subnet)
								    continue;
								    /* update circular connectivity lists */
								    if
								    (last_in_subnet)
								    MergeNets
								    (last_in_subnet,
								     rb,
								     ORIGINAL);
								    if
								    (last_in_net)
								    MergeNets
								    (last_in_net,
								     rb, NET);
								    last_in_subnet
								    =
								    last_in_net
								    = rb;
								    /* XXX: set rb->augStyle! */
								    rb->
								    augStyle->Used
								    = True;
								    rd->
								    max_bloat
								    =
								    MAX
								    (rd->max_bloat,
								     BLOAT
								     (rb->augStyle->style));});});
		  if (last_net
		      && last_in_net) MergeNets (last_net, last_in_net,
						 DIFFERENT_NET);
		  last_net = last_in_net;}
    );
    rd->first_net = last_net;
  }
  FreeNetListListMemory (&Nets);

  /* reset all nets to "original" connectivity (which we just set) */
  {
    routebox_t *net;
    LIST_LOOP (rd->first_net, different_net, net, ResetSubnet (net));
  }

  /* create "empty-space" structures for via placement (now that we know
   * appropriate keepaways for all the fixed elements) */
  for (i = 0; i < MAX_LAYER; i++)
    {
      for (j = 0; j < NUM_STYLES + 1; j++)
	{
	  if (rd->augStyles[j].Used)
	    {
	      POINTER_LOOP (&layergroupboxes[i],
			    {
			    routebox_t * rb = (routebox_t *) * ptr;
			    if (!rb->flags.clear_poly)
			    mtspace_add (rd->augStyles[j].mtspace,
					 &rb->box, FIXED,
					 rb->augStyle->style->Keepaway);});
	    }
	}
    }
  /* free pointer lists */
  for (i = 0; i < MAX_LAYER; i++)
    FreePointerListMemory (&layergroupboxes[i]);
  /* done! */
  return rd;
}

void
DestroyRouteData (routedata_t ** rd)
{
  int i;
  for (i = 0; i < MAX_LAYER; i++)
    kd_destroy_tree (&(*rd)->layergrouptree[i]);
  for (i = 0; i < NUM_STYLES + 1; i++)
    mtspace_destroy (&(*rd)->augStyles[i].mtspace);
  free (*rd);
  *rd = NULL;
}

/*-----------------------------------------------------------------
 * routebox reference counting.
 */

/* increment the reference count on a routebox. */
static void
RB_up_count (routebox_t * rb)
{
  assert (rb->flags.orphan);
  rb->refcount++;
}

/* decrement the reference count on a routebox, freeing if this box becomes
 * unused. */
static void
RB_down_count (routebox_t * rb)
{
  assert (rb->flags.orphan);
  assert (rb->refcount > 0);
  if (--rb->refcount == 0)
    {
      /* rb->underlying is guaranteed to not be an orphan, so we only need
       * to downcount the parent, if type==EXPANSION_AREA */
      if (rb->type == EXPANSION_AREA
	  && rb->parent.expansion_area->flags.orphan)
	RB_down_count (rb->parent.expansion_area);
      free (rb);
    }
}

/*-----------------------------------------------------------------
 * Rectangle-expansion routing code.
 */

static void
ResetSubnet (routebox_t * net)
{
  routebox_t *rb;
  /* reset connectivity of everything on this net */
  LIST_LOOP (net, same_net, rb, rb->same_subnet = rb->original_subnet);
}

static cost_t
cost_to_point (const PointType * p1, Cardinal point_layer1,
	       const PointType * p2, Cardinal point_layer2)
{
  cost_t x_dist = (p1->X - p2->X), y_dist = (p1->Y - p2->Y), r;
  /* even layers are horiz; odd layers are vert. */
  if ((point_layer1 % 2) != 0 && (point_layer2 % 2) != 0)
    x_dist += (x_dist * AutoRouteParameters.DirectionPenaltyNumerator)
      / AutoRouteParameters.DirectionPenaltyDenominator;
  if ((point_layer1 % 2) != 1 && (point_layer2 % 2) != 1)
    y_dist += (y_dist * AutoRouteParameters.DirectionPenaltyNumerator)
      / AutoRouteParameters.DirectionPenaltyDenominator;
  /* cost is proportional to orthogonal distance. */
  r = ABS (x_dist) + ABS (y_dist);
  /* apply via cost penalty if layers differ */
  if (point_layer1 != point_layer2)
    r += AutoRouteParameters.ViaCost;
  return r;
}

/* return the minimum *cost* from a point to a box on any layer.
 * The minimum cost may be either the cost to a box on the same layer,
 * or the cost to a box on a different layer, with a via cost added
 * in.  If the direction penalty code above changes, this routine
 * will have to be changed too. */
static cost_t
cost_to_layerless_box (const PointType * p, Cardinal point_layer,
		       const BoxType * b)
{
  PointType p2 = closest_point_in_box (p, b);
  cost_t cost1 = cost_to_point (p, point_layer, &p2, point_layer);
  cost_t cost2 = cost_to_point (p, point_layer, &p2, point_layer + 1);
  return MIN (cost1, cost2);
}

/* return the minimum *cost* from a point to a route box, including possible
 * via costs if the route box is on a different layer. */
static cost_t
cost_to_routebox (const PointType * p, Cardinal point_layer,
		  const routebox_t * rb)
{
  PointType p2 = closest_point_in_box (p, &rb->box);
  return cost_to_point (p, point_layer, &p2, rb->group);
}

static BoxType
bloat_routebox (routebox_t * rb)
{
  BoxType r;
  Dimension keepaway;
  assert (__routebox_is_good (rb));
  if (rb->type == EXPANSION_AREA || rb->flags.nobloat)
    return rb->box;		/* no bloat */

  /* obstacle exclusion zones get bloated, and then shrunk on their
   * top and left sides so that they approximate their "open"
   * brethern. */
  keepaway = MAX (AutoRouteParameters.augStyle->style->Keepaway,
		  rb->augStyle->style->Keepaway);
  r = bloat_box (&rb->box, keepaway +
		 HALF_THICK (AutoRouteParameters.augStyle->style->Thick));
  r.X1++;
  r.Y1++;
  return r;
}


#ifdef ROUTE_DEBUG		/* only for debugging expansion areas */
/* makes a line on the solder layer silk surrounding the box */
static void
showbox (BoxType b, Dimension thickness)
{
  LayerTypePtr SLayer = &(PCB->Data->Layer[MAX_LAYER + SOLDER_LAYER]);
  CreateNewLineOnLayer (SLayer, b.X1, b.Y1, b.X2, b.Y1, thickness, 1, 0);
  CreateNewLineOnLayer (SLayer, b.X1, b.Y2, b.X2, b.Y2, thickness, 1, 0);
  CreateNewLineOnLayer (SLayer, b.X1, b.Y1, b.X1, b.Y2, thickness, 1, 0);
  CreateNewLineOnLayer (SLayer, b.X2, b.Y1, b.X2, b.Y2, thickness, 1, 0);
}
#endif

#if defined(ROUTE_DEBUG)
static void
showroutebox (routebox_t * rb)
{
  showbox (rb->box, rb->flags.is_via ? 2 : 1);
}
#endif

/* return a "parent" of this edge which immediately precedes it in the route.*/
static routebox_t *
route_parent (routebox_t * rb)
{
  while (rb->flags.orphan && rb->underlying == NULL && !rb->flags.is_via)
    {
      assert (rb->type == EXPANSION_AREA);
      rb = rb->parent.expansion_area;
      assert (rb);
    }
  return rb;
}

/* return a "parent" of this edge which resides in a kd-tree somewhere */
/* -- actually, this "parent" *may* be a via box, which doesn't live in
 * a kd-tree. -- */
static routebox_t *
nonorphan_parent (routebox_t * rb)
{
  rb = route_parent (rb);
  return rb->underlying ? rb->underlying : rb;
}

/* some routines to find the minimum *cost* from a cost point to
 * a target (any target) */
struct mincost_target_closure
{
  const PointType *CostPoint;
  Cardinal CostPointLayer;
  routebox_t *nearest;
  cost_t nearest_cost;
};
static int
__region_within_guess (const BoxType * region, void *cl)
{
  struct mincost_target_closure *mtc = (struct mincost_target_closure *) cl;
  cost_t cost_to_region =
    cost_to_layerless_box (mtc->CostPoint, mtc->CostPointLayer, region);
  assert (cost_to_region >= 0);
  /* if no guess yet, all regions are "close enough" */
  if (mtc->nearest == NULL)
    return 1;
  /* note that cost is *strictly more* than minimum distance, so we'll
   * always search a region large enough. */
  return (cost_to_region < mtc->nearest_cost);
}
static int
__found_new_guess (const BoxType * box, void *cl)
{
  struct mincost_target_closure *mtc = (struct mincost_target_closure *) cl;
  routebox_t *guess = (routebox_t *) box;
  cost_t cost_to_guess =
    cost_to_routebox (mtc->CostPoint, mtc->CostPointLayer, guess);
  assert (cost_to_guess >= 0);
  /* if we don't have a guess yet, or this is cheaper than previous guess... */
  if (mtc->nearest == NULL || cost_to_guess < mtc->nearest_cost)
    {
      mtc->nearest = guess;
      mtc->nearest_cost = cost_to_guess;	/* this is our new guess! */
      return 1;
    }
  else
    return 0;			/* not less expensive than our last guess */
}

/* guess is distance squared to nearest target. also returns distance squared*/
/* target_guess is our guess at what the nearest target is, or NULL if we
 * just plum don't have a clue. */
static routebox_t *
mincost_target_to_point (const PointType * CostPoint,
			 Cardinal CostPointLayer,
			 kdtree_t * targets, routebox_t * target_guess)
{
  struct mincost_target_closure mtc;
  assert (target_guess == NULL || target_guess->flags.target);	/* this is a target, right? */
  mtc.CostPoint = CostPoint;
  mtc.CostPointLayer = CostPointLayer;
  mtc.nearest = target_guess;
  if (mtc.nearest)
    mtc.nearest_cost =
      cost_to_routebox (mtc.CostPoint, mtc.CostPointLayer, mtc.nearest);
  kd_search (targets, NULL, __region_within_guess, __found_new_guess, &mtc);
  assert (mtc.nearest != NULL && mtc.nearest_cost >= 0);
  assert (mtc.nearest->flags.target);	/* this is a target, right? */
  return mtc.nearest;
}

/* create edge from field values */
/* mincost_target_guess can be NULL */
static edge_t *
CreateEdge (routebox_t * rb,
	    Position CostPointX, Position CostPointY,
	    cost_t cost_to_point,
	    routebox_t * mincost_target_guess,
	    direction_t expand_dir, kdtree_t * targets)
{
  edge_t *e;
  assert (__routebox_is_good (rb));
  e = calloc (1, sizeof (*e));
  assert (e);
  e->rb = rb;
  if (rb->flags.orphan)
    RB_up_count (rb);
  e->cost_point.X = CostPointX;
  e->cost_point.Y = CostPointY;
  e->cost_to_point = cost_to_point;
  e->mincost_target =
    mincost_target_to_point (&e->cost_point, rb->group,
			     targets, mincost_target_guess);
  e->expand_dir = expand_dir;
  assert (e->rb && e->mincost_target);	/* valid edge? */
  assert (!e->flags.is_via || e->flags.expand_all_sides);
  /* cost point should be on edge (unless this is a via/conflict edge) */
  assert (rb->underlying != NULL || rb->flags.is_via ||
	  ((expand_dir == NORTH || expand_dir == SOUTH) ?
	   rb->box.X1 <= CostPointX && CostPointX <= rb->box.X2 &&
	   CostPointY == (expand_dir == NORTH ? rb->box.Y1 : rb->box.Y2) :
	   /* expand_dir==EAST || expand_dir==WEST */
	   rb->box.Y1 <= CostPointY && CostPointY <= rb->box.Y2 &&
	   CostPointX == (expand_dir == EAST ? rb->box.X2 : rb->box.X1)));
  assert (__edge_is_good (e));
  /* done */
  return e;
}

/* create edge, using previous edge to fill in defaults. */
/* most of the work here is in determining a new cost point */
static edge_t *
CreateEdge2 (routebox_t * rb, direction_t expand_dir,
	     edge_t * previous_edge, kdtree_t * targets)
{
  BoxType thisbox;
  PointType thiscost, prevcost;
  cost_t d;

  assert (rb && previous_edge && targets);
  /* okay, find closest costpoint to costpoint of previous edge */
  thisbox = edge_to_box (&rb->box, expand_dir);
  prevcost = previous_edge->cost_point;
  thiscost = closest_point_in_box (&prevcost, &thisbox);
  /* compute cost-to-point (orthogonal distance) */
  d = ABS (prevcost.X - thiscost.X) + ABS (prevcost.Y - thiscost.Y);
  /* add in jog penalty */
  if (previous_edge->expand_dir != expand_dir)
    d += AutoRouteParameters.JogPenalty;
  /* okay, new edge! */
  return CreateEdge (rb, thiscost.X, thiscost.Y,
		     previous_edge->cost_to_point + d,
		     previous_edge->mincost_target, expand_dir, targets);
}

/* create via edge, using previous edge to fill in defaults. */
static edge_t *
CreateViaEdge (const BoxType * area, Cardinal group,
	       routebox_t * parent, edge_t * previous_edge,
	       conflict_t to_site_conflict,
	       conflict_t through_site_conflict, kdtree_t * targets)
{
  routebox_t *rb;
  PointType costpoint;
  cost_t d;
  edge_t *ne;
  cost_t scale[3] = { 1,
    AutoRouteParameters.LastConflictPenalty,
    AutoRouteParameters.ConflictPenalty
  };
  assert (__box_is_good (area));
  assert (AutoRouteParameters.with_conflicts ||
	  (to_site_conflict == NO_CONFLICT &&
	   through_site_conflict == NO_CONFLICT));
  rb = CreateExpansionArea (area, group, parent, True);
  rb->flags.is_via = 1;
#if defined(ROUTE_DEBUG) && defined(DEBUG_SHOW_VIA_BOXES)
  showroutebox (rb);
#endif /* ROUTE_DEBUG && DEBUG_SHOW_VIA_BOXES */
  costpoint = closest_point_in_box (&previous_edge->cost_point, &rb->box);
  d =
    (scale[to_site_conflict] *
     cost_to_point (&costpoint, previous_edge->rb->group,
		    &previous_edge->cost_point, previous_edge->rb->group)) +
    (scale[through_site_conflict] *
     cost_to_point (&costpoint, group, &costpoint, previous_edge->rb->group));
  ne = CreateEdge (rb, costpoint.X, costpoint.Y,
		   previous_edge->cost_to_point + d,
		   previous_edge->mincost_target, NORTH	/*arbitrary */
		   , targets);
  ne->flags.expand_all_sides = ne->flags.is_via = 1;
  ne->flags.via_conflict_level = to_site_conflict;
  assert (__edge_is_good (ne));
  return ne;
}

/* create "interior" edge for routing with conflicts */
static edge_t *
CreateEdgeWithConflicts (const BoxType * interior_edge,
			 routebox_t * container,
			 edge_t * previous_edge,
			 cost_t cost_penalty_to_box, kdtree_t * targets)
{
  routebox_t *rb;
  BoxType b;
  PointType costpoint;
  cost_t d;
  edge_t *ne;
  assert (interior_edge && container && previous_edge && targets);
  assert (!container->flags.orphan);
  assert (AutoRouteParameters.with_conflicts);
  /* create expansion area equal to the bloated container.  Put costpoint
   * in this box.  compute cost, but add jog penalty. */
  b = bloat_routebox (container);
  assert (previous_edge->rb->group == container->group);
  rb = CreateExpansionArea (&b, previous_edge->rb->group, previous_edge->rb,
			    True);
  rb->underlying = container;	/* crucial! */
  costpoint = closest_point_in_box (&previous_edge->cost_point, &b);
  d = cost_to_point (&costpoint, previous_edge->rb->group,
		     &previous_edge->cost_point, previous_edge->rb->group);
  d *= cost_penalty_to_box;
  ne = CreateEdge (rb, costpoint.X, costpoint.Y,
		   previous_edge->cost_to_point + d,
		   previous_edge->mincost_target, NORTH	/*arbitrary */
		   , targets);
  ne->flags.expand_all_sides = ne->flags.is_interior = 1;
  assert (__edge_is_good (ne));
  return ne;
}

static void
DestroyEdge (edge_t ** e)
{
  assert (e && *e);
  if ((*e)->rb->flags.orphan)
    RB_down_count ((*e)->rb);	/* possibly free rb */
  free (*e);
  *e = NULL;
}

/* cost function for an edge. */
static cost_t
edge_cost (const edge_t * e)
{
  /* cost_to_routebox adds in our via correction, too. */
  return e->cost_to_point +
    cost_to_routebox (&e->cost_point, e->rb->group, e->mincost_target);
}

static Position
edge_length (const BoxType * cb, direction_t expand_dir)
{
  BoxType b = *cb;
  ROTATEBOX_TO_NORTH (b, expand_dir);
  assert (b.X1 <= b.X2);
  return b.X2 - b.X1;
}

/* return a bounding box for the PCB board. */
static BoxType
pcb_bounds (void)
{
  BoxType b;
  b.X1 = 0;
  b.X2 = PCB->MaxWidth;
  b.Y1 = 0;
  b.Y2 = PCB->MaxHeight;
  /* adjust from closed to half-closed box */
  b.X2++;
  b.Y2++;
  /* done */
  return b;
}

static BoxType
shrunk_pcb_bounds ()
{
  BoxType b = pcb_bounds ();
  return shrink_box (&b, AutoRouteParameters.augStyle->style->Keepaway +
		     HALF_THICK (AutoRouteParameters.augStyle->style->Thick));
}

/* create a maximal expansion region from the specified edge to the edge
 * of the PCB (minus the required keepaway). */
static BoxType
edge_to_infinity_region (edge_t * e)
{
  BoxType max, ebox;
  ebox = e->rb->box;
  max = shrunk_pcb_bounds ();
  /* normalize to north */
  ROTATEBOX_TO_NORTH (max, e->expand_dir);
  ROTATEBOX_TO_NORTH (ebox, e->expand_dir);
  /* north case: */
  max.X1 = ebox.X1;
  max.X2 = ebox.X2;
  max.Y2 = ebox.Y1;
  /* unnormalize */
  ROTATEBOX_FROM_NORTH (max, e->expand_dir);
  /* done */
  return max;
}

/* given an edge of a box, return a box containing exactly the points on that
 * edge.  Note that the box is treated as closed; that is, the bottom and
 * right "edges" consist of points (just barely) not in the (half-open) box. */
static BoxType
edge_to_box (const BoxType * box, direction_t expand_dir)
{
  BoxType b = *box;
  /* narrow box down to just the appropriate edge */
  switch (expand_dir)
    {
    case NORTH:
      b.Y2 = b.Y1;
      break;
    case EAST:
      b.X1 = b.X2;
      break;
    case SOUTH:
      b.Y1 = b.Y2;
      break;
    case WEST:
      b.X2 = b.X1;
      break;
    default:
      assert (0);
    }
  /* treat b as *closed* instead of half-closed, by adding one to
   * the (normally-open) bottom and right edges. */
  b.X2++;
  b.Y2++;
  /* done! */
  return b;
}

/* limit the specified expansion region so that it just touches the
 * given limit.  Returns 1 if the limited region has any area. */
static BoxType
limit_region (BoxType region, edge_t * e, BoxType lbox)
{
  ROTATEBOX_TO_NORTH (region, e->expand_dir);
  ROTATEBOX_TO_NORTH (lbox, e->expand_dir);
  /* north case: */
  assert (lbox.Y2 >= region.Y1);	/* validity of limiter */
  assert (lbox.X1 <= region.X2);
  assert (lbox.X2 >= region.X1);
  region.Y1 = lbox.Y2;
  assert (region.Y1 <= region.Y2);
  /* now rotate back */
  ROTATEBOX_FROM_NORTH (region, e->expand_dir);
  return region;
}

struct broken_boxes
{
  BoxType left, center, right;
  Boolean is_valid_left, is_valid_center, is_valid_right;
};

static struct broken_boxes
break_box_edge (const BoxType * original, direction_t which_edge,
		routebox_t * breaker)
{
  BoxType origbox, breakbox;
  struct broken_boxes result;

  assert (original && breaker);

  origbox = *original;
  breakbox = bloat_routebox (breaker);
  ROTATEBOX_TO_NORTH (origbox, which_edge);
  ROTATEBOX_TO_NORTH (breakbox, which_edge);
  result.right.Y1 = result.right.Y2 = result.center.Y1 = result.center.Y2 =
    result.left.Y1 = result.left.Y2 = origbox.Y1;
  /* validity of breaker */
  assert (breakbox.X1 < origbox.X2 && breakbox.X2 > origbox.X1);
  /* left edge piece */
  result.left.X1 = origbox.X1;
  result.left.X2 = breakbox.X1;
  /* center (ie blocked) edge piece */
  result.center.X1 = MAX (breakbox.X1, origbox.X1);
  result.center.X2 = MIN (breakbox.X2, origbox.X2);
  /* right edge piece */
  result.right.X1 = breakbox.X2;
  result.right.X2 = origbox.X2;
  /* validity: */
  result.is_valid_left = (result.left.X1 < result.left.X2);
  result.is_valid_center = (result.center.X1 < result.center.X2);
  result.is_valid_right = (result.right.X1 < result.right.X2);
  /* rotate back */
  ROTATEBOX_FROM_NORTH (result.left, which_edge);
  ROTATEBOX_FROM_NORTH (result.center, which_edge);
  ROTATEBOX_FROM_NORTH (result.right, which_edge);
  /* done */
  return result;
}

#ifndef NDEBUG
static int
share_edge (const BoxType * child, const BoxType * parent)
{
  return
    (child->X1 == parent->X2 || child->X2 == parent->X1 ||
     child->Y1 == parent->Y2 || child->Y2 == parent->Y1) &&
    ((parent->X1 <= child->X1 && child->X2 <= parent->X2) ||
     (parent->Y1 <= child->Y1 && child->Y2 <= parent->Y2));
}
static int
edge_intersect (const BoxType * child, const BoxType * parent)
{
  return
    (child->X1 <= parent->X2) && (child->X2 >= parent->X1) &&
    (child->Y1 <= parent->Y2) && (child->Y2 >= parent->Y1);
}
#endif

/* area is the expansion area, on layer group 'group'. 'parent' is the
 * immediately preceding expansion area, for backtracing. 'lastarea' is
 * the last expansion area created, we string these together in a loop
 * so we can remove them all easily at the end. */
static routebox_t *
CreateExpansionArea (const BoxType * area, Cardinal group,
		     routebox_t * parent, Boolean relax_edge_requirements)
{
  routebox_t *rb = (routebox_t *) calloc (1, sizeof (*rb));
  assert (area && parent);
  init_const_box (rb, area->X1, area->Y1, area->X2, area->Y2);
  rb->group = group;
  rb->type = EXPANSION_AREA;
  /* should always share edge with parent */
  assert (relax_edge_requirements ? edge_intersect (&rb->box, &parent->box) :
	  share_edge (&rb->box, &parent->box));
  rb->parent.expansion_area = route_parent (parent);
  assert (relax_edge_requirements ? edge_intersect (&rb->box, &parent->box) :
	  share_edge (&rb->box, &parent->box));
  if (rb->parent.expansion_area->flags.orphan)
    RB_up_count (rb->parent.expansion_area);
  rb->flags.orphan = 1;
  rb->augStyle = AutoRouteParameters.augStyle;
  InitLists (rb);
#if defined(ROUTE_DEBUG) && defined(DEBUG_SHOW_EXPANSION_BOXES)
  showroutebox (rb);
#endif /* ROUTE_DEBUG && DEBUG_SHOW_EXPANSION_BOXES */
  return rb;
}

/*------ FindBlocker ------*/
struct FindBlocker_info
{
  edge_t *expansion_edge;
  Dimension maxbloat;
  routebox_t *blocker;
  Position min_dist;
};
/* helper methods for __FindBlocker */
static int
__FindBlocker_checkbox (const BoxType * region_or_box,
			struct FindBlocker_info *fbi, Boolean is_region)
{
  BoxType rbox, ebox;
  ebox = fbi->expansion_edge->rb->box;
  rbox = is_region ? bloat_box (region_or_box, fbi->maxbloat) :
    bloat_routebox ((routebox_t *) region_or_box);
  ROTATEBOX_TO_NORTH (rbox, fbi->expansion_edge->expand_dir);
  ROTATEBOX_TO_NORTH (ebox, fbi->expansion_edge->expand_dir);
  if (rbox.X2 < ebox.X1)
    return 0;
  if (rbox.X1 > ebox.X2)
    return 0;
  if (rbox.Y1 > ebox.Y1)
    return 0;
  if (fbi->blocker != NULL && rbox.Y2 < ebox.Y1 - fbi->min_dist)
    return 0;
  if (is_region)
    return 1;
  /* this is a box; it has to jump through a few more hoops */
  if (rbox.X2 == ebox.X1 || rbox.X1 == ebox.X2)
    return 0;			/* only touches */
  if ((routebox_t *) region_or_box ==
      nonorphan_parent (fbi->expansion_edge->rb))
    return 0;			/* this is the parent */
  if (rbox.Y2 > ebox.Y1)
    {				/* extends below edge */
      assert (fbi->expansion_edge->flags.is_interior);
      // XXX: what to do here?
    }
  /* okay, this is the closest we've found. */
  assert (fbi->blocker == NULL || (ebox.Y1 - rbox.Y2) <= fbi->min_dist);
  fbi->blocker = (routebox_t *) region_or_box;
  fbi->min_dist = ebox.Y1 - rbox.Y2;
  assert (fbi->min_dist >= 0);
  return 1;
}
static int
__FindBlocker_reg_in_sea (const BoxType * region, void *cl)
{
  struct FindBlocker_info *fbi = (struct FindBlocker_info *) cl;
  int r = __FindBlocker_checkbox (region, fbi, True);
#if 0
  printf ("Checking against region (%d %d)-(%d %d): %d (min dist %d)\n",
	  region->X1, region->Y1, region->X2, region->Y2, r, fbi->min_dist);
#endif
  return r;
}
static int
__FindBlocker_rect_in_reg (const BoxType * box, void *cl)
{
  struct FindBlocker_info *fbi = (struct FindBlocker_info *) cl;
  int r = __FindBlocker_checkbox (box, fbi, False);
#if 0
  printf ("Checking against rect (%d %d)-(%d %d): %d\n",
	  box->X1, box->Y1, box->X2, box->Y2, r);
#endif
  return r;
}

/* main FindBlocker routine.  Returns NULL if no neighbor in the
 * requested direction.
 *  - region is closed on all edges -
 */
routebox_t *
FindBlocker (kdtree_t * kdtree, edge_t * e, Dimension maxbloat)
{
  struct FindBlocker_info fbi = { e, maxbloat, NULL, 0 };
  kd_search (kdtree, NULL,
	     __FindBlocker_reg_in_sea, __FindBlocker_rect_in_reg, &fbi);
  return fbi.blocker;
}

/* ------------ */

struct fio_info
{
  edge_t *edge;
  Dimension maxbloat;
  routebox_t *intersect;
  jmp_buf env;
};
static int
fio_check (const BoxType * region_or_box, struct fio_info *fio,
	   Boolean is_region)
{
  routebox_t *rb;
  BoxType rbox, ebox;
  ebox = fio->edge->rb->box;
  rbox = is_region ? bloat_box (region_or_box, fio->maxbloat) :
    bloat_routebox ((routebox_t *) region_or_box);
  ROTATEBOX_TO_NORTH (rbox, fio->edge->expand_dir);
  ROTATEBOX_TO_NORTH (ebox, fio->edge->expand_dir);
  if (rbox.X2 <= ebox.X1)
    return 0;
  if (rbox.X1 >= ebox.X2)
    return 0;
  if (rbox.Y1 > ebox.Y1)
    return 0;
  if (rbox.Y2 < ebox.Y1)
    return 0;
  if (is_region)
    return 1;
  /* this is a box; it has to jump through a few more hoops */
  /* everything on same net is ignored */
  rb = (routebox_t *) region_or_box;
  assert (rb == nonorphan_parent (rb));
  if (rb == nonorphan_parent (fio->edge->rb))
    return 0;
  /* okay, this is an intersector! */
  fio->intersect = rb;
  longjmp (fio->env, 1);	/* skip to the end! */
  return 1;			/* never reached */
}
static int
fio_reg_in_sea (const BoxType * region, void *cl)
{
  return fio_check (region, (struct fio_info *) cl, True);
}
static int
fio_rect_in_reg (const BoxType * box, void *cl)
{
  return fio_check (box, (struct fio_info *) cl, False);
}
static routebox_t *
FindIntersectingObstacle (kdtree_t * kdtree, edge_t * e, Dimension maxbloat)
{
  struct fio_info fio = { e, maxbloat, NULL };
  if (setjmp (fio.env) == 0)
    kd_search (kdtree, NULL, fio_reg_in_sea, fio_rect_in_reg, &fio);
  return fio.intersect;
}

/* ------------ */

struct foib_info
{
  const BoxType *box;
  Dimension maxbloat;
  routebox_t *intersect;
  jmp_buf env;
};
static int
foib_check (const BoxType * region_or_box, struct foib_info *foib,
	    Boolean is_region)
{
  BoxType rbox;
  rbox = is_region ? bloat_box (region_or_box, foib->maxbloat) :
    bloat_routebox ((routebox_t *) region_or_box);
  if (!box_intersect (&rbox, foib->box))
    return 0;
  if (!is_region)
    {
      /* this is an intersector! */
      foib->intersect = (routebox_t *) region_or_box;
      longjmp (foib->env, 1);	/* skip to the end! */
    }
  return 1;
}
static int
foib_reg_in_sea (const BoxType * region, void *cl)
{
  return foib_check (region, (struct foib_info *) cl, True);
}
static int
foib_rect_in_reg (const BoxType * box, void *cl)
{
  return foib_check (box, (struct foib_info *) cl, False);
}
static routebox_t *
FindOneInBox (kdtree_t * kdtree, const BoxType * box, Dimension maxbloat)
{
  struct foib_info foib = { box, maxbloat, NULL };
  if (setjmp (foib.env) == 0)
    kd_search (kdtree, NULL, foib_reg_in_sea, foib_rect_in_reg, &foib);
  return foib.intersect;
}

/* create a new edge for every edge of the given routebox (e->rb) and
 * put the result edges in result_vec. */
void
ExpandAllEdges (edge_t * e, vector_t * result_vec,
		cost_t cost_penalty_in_box, kdtree_t * targets)
{
  PointType costpoint;
  cost_t cost;
  int i;
  assert (__edge_is_good (e));
  assert (e->flags.expand_all_sides);
  for (i = 0; i < 4; i++)
    {				/* for all directions */
      switch (i)
	{			/* assign appropriate cost point */
	case NORTH:
	  costpoint.X = e->cost_point.X;
	  costpoint.Y = e->rb->box.Y1;
	  break;
	case EAST:
	  costpoint.X = e->rb->box.X2;
	  costpoint.Y = e->cost_point.Y;
	  break;
	case SOUTH:
	  costpoint.X = e->cost_point.X;
	  costpoint.Y = e->rb->box.Y2;
	  break;
	case WEST:
	  costpoint.X = e->rb->box.X1;
	  costpoint.Y = e->cost_point.Y;
	  break;
	default:
	  assert (0);
	}
      cost = cost_penalty_in_box *
	cost_to_point (&e->cost_point, e->rb->group, &costpoint,
		       e->rb->group);
      vector_append (result_vec,
		     CreateEdge (e->rb, costpoint.X, costpoint.Y,
				 e->cost_to_point + cost, e->mincost_target,
				 i, targets));
    }
  /* done */
  return;
}

/* find edges which intersect obstacles, and break them into
 * intersecting and non-intersecting edges. */
void
BreakEdges (routedata_t * rd, vector_t * edge_vec, kdtree_t * targets)
{
  BoxType edgebox, bbox = shrunk_pcb_bounds ();
  vector_t *broken_vec = vector_create ();
  while (!vector_is_empty (edge_vec))
    {
      edge_t *e, *ne;
      routebox_t *rb;
      /* pop off the top edge */
      e = vector_remove_last (edge_vec);
      assert (!e->flags.expand_all_sides);
      /* check for edges that poke off the edge of the routeable area */
      edgebox = edge_to_box (&e->rb->box, e->expand_dir);
      if (!box_intersect (&bbox, &edgebox))
	{
	  /* edge completely off the PCB, skip it. */
	  DestroyEdge (&e);
	  continue;
	}
      if (!box_in_box (&bbox, &edgebox))
	{
	  /* edge partially off the PCB, clip it. */
	  routebox_t *nrb;
	  BoxType newbox = clip_box (&edgebox, &bbox);
	  /* 'close' box (newbox is currently half-open) */
	  newbox.X2--;
	  newbox.Y2--;
	  /* okay, create new, clipped, edge */
	  nrb =
	    CreateExpansionArea (&newbox, e->rb->group, route_parent (e->rb),
				 True);
	  ne = CreateEdge2 (nrb, e->expand_dir, e, targets);
	  nrb->flags.source = e->rb->flags.source;
	  nrb->flags.nobloat = e->rb->flags.nobloat;
	  /* adjust cost */
	  ne->cost_to_point = nrb->flags.source ? 0 :
	    e->cost_to_point +
	    (CONFLICT_PENALTY (nonorphan_parent (e->rb)) *
	     (ne->cost_to_point - e->cost_to_point));
	  assert (__edge_is_good (ne));
	  /* replace e with ne and continue. */
	  DestroyEdge (&e);
	  e = ne;
	  edgebox = edge_to_box (&e->rb->box, e->expand_dir);
	}
      assert (box_intersect (&bbox, &edgebox));
      assert (box_in_box (&bbox, &edgebox));
      /* find an intersecting obstacle, and then break edge on it. */
      rb = FindIntersectingObstacle (rd->layergrouptree[e->rb->group],
				     e, rd->max_bloat);
      assert (__edge_is_good (e));
      if (rb == NULL)
	{			/* no intersecting obstacle, this baby's good! */
#if defined(ROUTE_DEBUG) && defined(DEBUG_SHOW_ROUTE_BOXES)
	  showroutebox (e->rb);
	  printf ("GOOD EDGE FOUND!\n");
#endif
	  assert (__edge_is_good (e));
	  vector_append (broken_vec, e);
	}
      else
	{			/* rb has an intersecting obstacle.  break this in three pieces */
	  struct broken_boxes r =
	    break_box_edge (&e->rb->box, e->expand_dir, rb);
	  routebox_t *parent;
	  int i;

	  /* "canonical parent" is the original source */
	  parent = route_parent (e->rb);
	  assert (parent->underlying || parent->flags.is_via ||
		  parent->type != EXPANSION_AREA);

	  for (i = 0; i < 2; i++)
	    if (i ? r.is_valid_left : r.is_valid_right)
	      {
		routebox_t *nrb = CreateExpansionArea (i ? &r.left : &r.right,
						       e->rb->group, parent,
						       False);
		ne = CreateEdge2 (nrb, e->expand_dir, e, targets);
		nrb->flags.source = e->rb->flags.source;
		nrb->flags.nobloat = e->rb->flags.nobloat;
		/* adjust cost */
		ne->cost_to_point = nrb->flags.source ? 0 :
		  e->cost_to_point +
		  (CONFLICT_PENALTY (nonorphan_parent (e->rb)) *
		   (ne->cost_to_point - e->cost_to_point));
		assert (__edge_is_good (ne));
		vector_append (edge_vec, ne);
	      }
	  /* center edge is "interior" to obstacle */
	  /* don't bother adding if this is a source-interior edge */
	  /* or an expansion edge */
	  if (r.is_valid_center && (!rb->flags.source) &&
	      rb->type != EXPANSION_AREA &&
	      AutoRouteParameters.with_conflicts)
	    {
	      ne = CreateEdgeWithConflicts (&r.center, rb, e,
					    CONFLICT_PENALTY (nonorphan_parent
							      (e->rb)),
					    targets);
	      assert (__edge_is_good (ne));
	      vector_append (broken_vec, ne);
	    }
	  DestroyEdge (&e);
	}
      /* done with this edge */
    }
  /* all good edges are now on broken_vec list. */
  /* Transfer them back to edge_vec */
  assert (vector_size (edge_vec) == 0);
  vector_append_vector (edge_vec, broken_vec);
  vector_destroy (&broken_vec);
  /* done! */
  return;
}

/*--------------------------------------------------------------------
 * Route-tracing code: once we've got a path of expansion boxes, trace
 * a line through them to actually create the connection.
 */

static void
RD_DrawVia (routedata_t * rd, Position X, Position Y,
	    Dimension radius, routebox_t * subnet, Boolean is_bad)
{
  routebox_t *rb, *first_via = NULL;
  int i, j;
  /* a via cuts through every layer group */
  for (i = 0; i < MAX_LAYER; i++)
    {
      if (!is_layer_group_active (i))
	continue;
      rb = (routebox_t *) calloc (1, sizeof (*rb));
      init_const_box (rb,
		      /*X1 */ X - radius, /*Y1 */ Y - radius,
		      /*X2 */ X + radius, /*Y2 */ Y + radius);
      rb->group = i;
      if (first_via == NULL)
	{
	  rb->type = VIA;
	  rb->parent.via = NULL;	/* indicates that not on PCB yet */
	  first_via = rb;
	}
      else
	{
	  rb->type = VIA_SHADOW;
	  rb->parent.via_shadow = first_via;
	}
      rb->flags.fixed = 0;	/* indicates that not on PCB yet */
      rb->flags.is_odd = AutoRouteParameters.is_odd;
      rb->flags.is_bad = is_bad;
      rb->flags.circular = True;
      rb->augStyle = AutoRouteParameters.augStyle;
      InitLists (rb);
      /* add these to proper subnet. */
      MergeNets (rb, subnet, NET);
      MergeNets (rb, subnet, SUBNET);
      assert (__routebox_is_good (rb));
      /* and add it to the kd-tree! */
      kd_insert_node (rd->layergrouptree[rb->group], &rb->box, 1);
      /* and to the via space structures */
      if (AutoRouteParameters.use_vias)
	for (j = 0; j < NUM_STYLES + 1; j++)
	  if (rd->augStyles[j].Used)
	    mtspace_add (rd->augStyles[j].mtspace, &rb->box,
			 rb->flags.is_odd ? ODD : EVEN,
			 rb->augStyle->style->Keepaway);
    }
}
static void
RD_DrawLine (routedata_t * rd,
	     Position X1, Position Y1, Position X2, Position Y2,
	     Dimension halfthick, Cardinal group,
	     routebox_t * subnet, Boolean is_bad, Boolean is_45)
{
  routebox_t *rb;
  int i;
  /* don't draw zero-length segments. */
  if (X1 == X2 && Y1 == Y2)
    return;
  rb = (routebox_t *) calloc (1, sizeof (*rb));
  assert (is_45 ? (ABS (X2 - X1) == ABS (Y2 - Y1))	/* line must be 45-degrees */
	  : (X1 == X2 || Y1 == Y2) /* line must be ortho */ );
  init_const_box (rb,
		  /*X1 */ MIN (X1, X2) - halfthick,
		  /*Y1 */ MIN (Y1, Y2) - halfthick,
		  /*X2 */ MAX (X1, X2) + halfthick,
		  /*Y2 */ MAX (Y1, Y2) + halfthick);
  rb->group = group;
  rb->type = LINE;
  rb->parent.line = NULL;	/* indicates that not on PCB yet */
  rb->flags.fixed = 0;		/* indicates that not on PCB yet */
  rb->flags.is_odd = AutoRouteParameters.is_odd;
  rb->flags.is_bad = is_bad;
  rb->flags.nonstraight = is_45;
  rb->flags.bl_to_ur = is_45 && (MIN (X1, X2) == X1) != (MIN (Y1, Y2) == Y1);
  rb->augStyle = AutoRouteParameters.augStyle;
  InitLists (rb);
  /* add these to proper subnet. */
  MergeNets (rb, subnet, NET);
  MergeNets (rb, subnet, SUBNET);
  assert (__routebox_is_good (rb));
  /* and add it to the kd-tree! */
  kd_insert_node (rd->layergrouptree[rb->group], &rb->box, 1);
  /* and to the via space structures */
  if (AutoRouteParameters.use_vias)
    for (i = 0; i < NUM_STYLES + 1; i++)
      if (rd->augStyles[i].Used)
	mtspace_add (rd->augStyles[i].mtspace, &rb->box,
		     rb->flags.is_odd ? ODD : EVEN,
		     rb->augStyle->style->Keepaway);
}
static void
RD_DrawManhattanLine (routedata_t * rd,
		      const BoxType * bbox,
		      PointType start, PointType end,
		      Dimension halfthick, Cardinal group,
		      routebox_t * subnet, Boolean is_bad)
{
  PointType knee = start;
  /* don't draw zero-length segments; start and knee should box be in bbox */
  if (point_in_box (bbox, end.X, start.Y))
    knee.X = end.X;
  else
    knee.Y = end.Y;
  assert (point_in_box (bbox, knee.X, knee.Y));

  if (1 || !AutoRouteParameters.is_smoothing)
    {
      /* draw standard manhattan paths */
      RD_DrawLine (rd, start.X, start.Y, knee.X, knee.Y, halfthick, group,
		   subnet, is_bad, False);
      RD_DrawLine (rd, knee.X, knee.Y, end.X, end.Y, halfthick, group,
		   subnet, is_bad, False);
    }
  else
    {
      /* draw 45-degree path across knee */
      Dimension len45 = MIN (ABS (start.X - end.X), ABS (start.Y - end.Y));
      PointType kneestart = knee, kneeend = knee;
      if (kneestart.X == start.X)
	kneestart.Y += (kneestart.Y > start.Y) ? -len45 : len45;
      else
	kneestart.X += (kneestart.X > start.X) ? -len45 : len45;
      if (kneeend.X == end.X)
	kneeend.Y += (kneeend.Y > end.Y) ? -len45 : len45;
      else
	kneeend.X += (kneeend.X > end.X) ? -len45 : len45;
      RD_DrawLine (rd, start.X, start.Y, kneestart.X, kneestart.Y, halfthick,
		   group, subnet, is_bad, False);
      RD_DrawLine (rd, kneestart.X, kneestart.Y, kneeend.X, kneeend.Y,
		   halfthick, group, subnet, is_bad, True);
      RD_DrawLine (rd, kneeend.X, kneeend.Y, end.X, end.Y, halfthick, group,
		   subnet, is_bad, False);
    }
}
static void
TracePath (routedata_t * rd, routebox_t * path, routebox_t * target,
	   routebox_t * subnet, Boolean is_bad)
{
  Dimension keepaway = AutoRouteParameters.augStyle->style->Keepaway;
  Dimension halfwidth =
    HALF_THICK (AutoRouteParameters.augStyle->style->Thick);
  Dimension radius =
    HALF_THICK (AutoRouteParameters.augStyle->style->Diameter);
  PointType lastpoint, nextpoint;
  routebox_t *lastpath;
  BoxType b;

  assert (subnet->augStyle == AutoRouteParameters.augStyle);

  /* start from *edge* of target box */
  /*XXX: because we round up odd thicknesses, there's the possibility that
   * a connecting line end-point might be 1/2 mil off the "real" edge.
   * don't worry about this because line *thicknesses* are always >= 1 mil. */
  nextpoint.X = (path->box.X1 + path->box.X2) / 2;
  nextpoint.Y = (path->box.Y1 + path->box.Y2) / 2;
  nextpoint = closest_point_in_box (&nextpoint,
				    &path->parent.expansion_area->box);
  /* for circular targets, use *inscribed* rectangle so we're sure to
   * connect. */
  b = path->box;
  if (target->flags.circular)
    b = shrink_box (&b, MIN (b.X2 - b.X1, b.Y2 - b.Y1) / 5);
  nextpoint = closest_point_in_box (&nextpoint, &b);
#if defined(ROUTE_DEBUG) && defined(DEBUG_SHOW_ROUTE_BOXES)
  showroutebox (path);
#endif /* ROUTE_DEBUG && DEBUG_SHOW_ROUTE_BOXES */
#if 0
  printf ("TRACEPOINT start (%d, %d)\n", nextpoint.X, nextpoint.Y);
#endif

  do
    {
      lastpoint = nextpoint;
      lastpath = path;
      assert (path->type == EXPANSION_AREA);
      path = path->parent.expansion_area;

      b = path->box;
      // XXX: if this is a smoothing pass, try to use a larger trace size?
      assert (b.X1 != b.X2 && b.Y1 != b.Y2);	/* need someplace to put line! */
      /* find point on path perimeter closest to last point */
      nextpoint = closest_point_in_box (&lastpoint, &b);

      assert (point_in_box (&lastpath->box, lastpoint.X, lastpoint.Y));
      assert (point_in_box (&path->box, nextpoint.X, nextpoint.Y));
#if defined(ROUTE_DEBUG)
      printf ("TRACEPATH: ");
      DumpRouteBox (path);
      printf ("TRACEPATH: shrink (%d, %d)-(%d, %d) point (%d, %d)\n",
	      b.X1, b.Y1, b.X2, b.Y2, nextpoint.X, nextpoint.Y);
#endif
      /* draw orthogonal lines from lastpoint to nextpoint */
      /* knee is placed in lastpath box */
      /* should never cause line to leave union of lastpath/path boxes */
      RD_DrawManhattanLine (rd, &lastpath->box,
			    lastpoint, nextpoint, halfwidth,
			    path->group, subnet, is_bad);
      /* if this is a smoothing pass, attempt to place the trace in
       * the center of the box */
      /* DISABLED because the results don't look like anything we'd want. */
      if (AutoRouteParameters.is_smoothing)
	{
	  /* now that we're in path box, overshoot 2*HALF_THICK+keepaway to
	   * allow another trace in here. */
	  Boolean ns = (lastpoint.X == nextpoint.X);
	  Boolean ew = (lastpoint.Y == nextpoint.Y);
	  BoxType bb = shrink_box (&b, keepaway + 2 * halfwidth);
	  if (bb.X1 >= bb.X2 || !ew)
	    {
	      bb.X1 = b.X1;
	      bb.X2 = b.X2;
	    }
	  if (bb.Y1 >= bb.Y2 || !ns)
	    {
	      bb.Y1 = b.Y1;
	      bb.Y2 = b.Y2;
	    }
	  lastpoint = nextpoint;
	  nextpoint = closest_point_in_box (&lastpoint, &bb);
	  RD_DrawManhattanLine (rd, &path->box, lastpoint, nextpoint,
				halfwidth, lastpath->group, subnet, is_bad);
	}
      if (path->flags.is_via)
	{			/* if via, then add via */
	  assert (point_in_box (&path->box, nextpoint.X, nextpoint.Y));
	  RD_DrawVia (rd, nextpoint.X, nextpoint.Y, radius, subnet, is_bad);
	}

      assert (lastpath->flags.is_via || path->group == lastpath->group);

#if defined(ROUTE_DEBUG) && defined(DEBUG_SHOW_ROUTE_BOXES)
      showroutebox (path);
#endif /* ROUTE_DEBUG && DEBUG_SHOW_ROUTE_BOXES */
    }
  while (!path->flags.source);

  /* this is source box; draw line to edge */
  lastpath = path;
  lastpoint = nextpoint;
  /* for circular targets, use *inscribed* rectangle so we're sure to
   * connect. */
  b = path->box;
  if (path->flags.circular)
    b = shrink_box (&b, MIN (b.X2 - b.X1, b.Y2 - b.Y1) / 5);
  nextpoint = closest_point_in_box (&lastpoint, &b);
#if 0
  printf ("TRACEPATH end (%d, %d)\n", nextpoint.X, nextpoint.Y);
#endif
  RD_DrawManhattanLine (rd, &lastpath->box,
			lastpoint, nextpoint, halfwidth,
			lastpath->group, subnet, is_bad);
  //XXX: nonstraight terminals may not be connected properly!
  /* done! */
}

struct routeone_state
{
  /* heap of all candidate expansion edges */
  heap_t *workheap;
  /* information about the best path found so far. */
  routebox_t *best_path, *best_target;
  cost_t best_cost;
};

static void
add_or_destroy_edge (struct routeone_state *s, edge_t * e)
{
  assert (__edge_is_good (e));
  assert (is_layer_group_active (e->rb->group));
  if (s->best_path == NULL || edge_cost (e) < s->best_cost)
    heap_insert (s->workheap, edge_cost (e), e);
  else
    DestroyEdge (&e);
}
static void
best_path_candidate (struct routeone_state *s,
		     edge_t * e, routebox_t * best_target)
{
  if (s->best_path == NULL || edge_cost (e) < s->best_cost)
    {
      /* new best path! */
      if (s->best_path && s->best_path->flags.orphan)
	RB_down_count (s->best_path);
      s->best_path = e->rb;
      s->best_target = best_target;
      s->best_cost = edge_cost (e);
      assert (s->best_cost >= 0);
      /* don't free this when we destroy edge! */
      if (s->best_path->flags.orphan)
	RB_up_count (s->best_path);
    }
}

/* vectors for via site candidates (see mtspace.h) */
struct routeone_via_site_state
{
  vector_t *free_space_vec;
  vector_t *lo_conflict_space_vec;
  vector_t *hi_conflict_space_vec;
};

void
add_via_sites (struct routeone_state *s,
	       struct routeone_via_site_state *vss,
	       mtspace_t * mtspace, routebox_t * within,
	       conflict_t within_conflict_level,
	       edge_t * parent_edge, kdtree_t * targets)
{
  int i, j;
  assert (AutoRouteParameters.use_vias);
  //XXX: need to clip 'within' to shrunk_pcb_bounds, because when
  //XXX: routing with conflicts may poke over edge.

  mtspace_query_rect (mtspace, &within->box,
		      vss->free_space_vec,
		      vss->lo_conflict_space_vec,
		      vss->hi_conflict_space_vec, AutoRouteParameters.is_odd);

  for (i = 0; i < 3; i++)
    {
      vector_t *v =
	(i == NO_CONFLICT ? vss->free_space_vec :
	 i == LO_CONFLICT ? vss->lo_conflict_space_vec :
	 i == HI_CONFLICT ? vss->hi_conflict_space_vec : NULL);
      assert (v);
      while (!vector_is_empty (v))
	{
	  BoxType *area = vector_remove_last (v);
	  BoxType cliparea = clip_box (area, &within->box);
	  assert (__box_is_good (&cliparea));
	  if (!(i == NO_CONFLICT || AutoRouteParameters.with_conflicts))
	    continue;
	  for (j = 0; j < MAX_LAYER; j++)
	    {
	      edge_t *ne;
	      if (j == within->group)
		continue;
	      if (!is_layer_group_active (j))
		continue;
	      ne = CreateViaEdge (&cliparea, j, within, parent_edge,
				  within_conflict_level, i, targets);
	      add_or_destroy_edge (s, ne);
	    }
	}
    }
  assert (vector_is_empty (vss->free_space_vec));
  assert (vector_is_empty (vss->lo_conflict_space_vec));
  assert (vector_is_empty (vss->hi_conflict_space_vec));
}

struct routeone_status
{
  Boolean found_route;
  Boolean route_had_conflicts;
  cost_t best_route_cost;
  Boolean net_completely_routed;
};

static struct routeone_status
RouteOne (routedata_t * rd, routebox_t * from, routebox_t * to)
{
  struct routeone_status result;
  routebox_t *p;
  int seen, i;
  const BoxType **target_list;
  int num_targets;
  kdtree_t *targets;
  /* vector of source edges for filtering */
  vector_t *source_vec;
  /* vector of expansion areas to be eventually removed from kd-tree */
  vector_t *area_vec;
  /* vector of "touched" fixed regions to be reset upon completion */
  vector_t *touched_vec;
  /* working vector */
  vector_t *edge_vec;

  struct routeone_state s;
  struct routeone_via_site_state vss;

  assert (rd && from);
  /* no targets on to/from net need keepaway areas */
  LIST_LOOP (from, same_net, p, p->flags.nobloat = 1);
  /* set 'source' flags */
  LIST_LOOP (from, same_subnet, p, p->flags.source = 1);
  assert (from->flags.source);

  if (to)
    {				/* if we're routing to a specific target */
      if (!to->flags.source)
	{			/* not already connected */
	  /* check that 'to' and 'from' are on the same net */
	  seen = 0;
	  LIST_LOOP (from, same_net, p, if (p == to) seen = 1);
	  assert (seen);	/* otherwise from and to are on different nets! */
	  /* set target flags only on 'to's subnet */
	  LIST_LOOP (to, same_subnet, p, p->flags.target = 1);
	  assert (to->flags.target);
	}
    }
  else
    {				/* all nodes on the net but not connected to from are targets */
      LIST_LOOP (from, same_net, p, if (!p->flags.source) p->flags.target =
		 1);
    }

  /* remove source/target flags from non-straight obstacles, because they
   * don't fill their bounding boxes and so connecting to them
   * after we've routed is problematic.  Better solution? */
  LIST_LOOP (from, same_net, p,
	     if (p->flags.nonstraight) p->flags.source = p->flags.target = 0);

  /* count up the targets */
  num_targets = 0;
  LIST_LOOP (from, same_net, p, if (p->flags.target) num_targets++);

  /* if no targets, then net is done!  reset flags and return. */
  if (num_targets == 0)
    {
      LIST_LOOP (from, same_net, p,
		 p->flags.source = p->flags.target = p->flags.nobloat = 0);
      result.found_route = False;
      result.net_completely_routed = True;
      return result;
    }
  result.net_completely_routed = False;

  /* okay, there's stuff to route */
  assert (!from->flags.target);
  assert (num_targets > 0);
  /* create list of target pointers and from that a kd-tree of targets */
  target_list = malloc (num_targets * sizeof (*target_list));
  i = 0;
  LIST_LOOP (from, same_net, p, if (p->flags.target)
	     {
	     target_list[i++] = &p->box;}
  );
  targets = kd_create_tree (target_list, i, 0);
  assert (i <= num_targets);
  free (target_list);

  /* add all sources to a vector */
  source_vec = vector_create ();
  LIST_LOOP (from, same_subnet, p, if (p->flags.source &&
				       is_layer_group_active (p->group))
	     {
	     /* we need the test for 'source' because this box may be nonstraight */
	     /* may expand in all directions from source; arbitrary cost point. */
	     vector_append
	     (source_vec,
	      CreateEdge (p, p->box.X1, p->box.Y1, 0, NULL, NORTH, targets));
	     vector_append (source_vec,
			    CreateEdge (p, p->box.X2, p->box.Y1, 0, NULL,
					EAST, targets));
	     vector_append (source_vec,
			    CreateEdge (p, p->box.X2, p->box.Y2, 0, NULL,
					SOUTH, targets));
	     vector_append (source_vec,
			    CreateEdge (p, p->box.X1, p->box.Y2, 0, NULL,
					WEST, targets));}
  );
  /* break source edges; some edges may be too near obstacles to be able
   * to exit from. */
  BreakEdges (rd, source_vec, targets);

  /* okay, main expansion-search routing loop. */
  /* set up the initial activity heap */
  s.workheap = heap_create ();
  assert (s.workheap);
  while (!vector_is_empty (source_vec))
    {
      edge_t *e = vector_remove_last (source_vec);
      assert (is_layer_group_active (e->rb->group));
      heap_insert (s.workheap, edge_cost (e), e);
    }
  vector_destroy (&source_vec);
  /* okay, process items from heap until it is empty! */
  s.best_path = NULL;
  area_vec = vector_create ();
  edge_vec = vector_create ();
  touched_vec = vector_create ();
  vss.free_space_vec = vector_create ();
  vss.lo_conflict_space_vec = vector_create ();
  vss.hi_conflict_space_vec = vector_create ();
  while (!heap_is_empty (s.workheap))
    {
      edge_t *e = heap_remove_smallest (s.workheap);
      assert (__edge_is_good (e));
      /* we should never add edges on inactive layer groups to the heap. */
      assert (is_layer_group_active (e->rb->group));
      /* don't bother expanding this edge if the minimum possible edge cost
       * is already larger than the best edge cost we've found. */
      if (s.best_path && edge_cost (e) > s.best_cost)
	goto dontexpand;	/* skip this edge */

      if (e->flags.is_interior)
	{
	  assert (AutoRouteParameters.with_conflicts);	/* no interior edges unless
							   routing with conflicts! */
	  assert (e->rb->underlying);
	  if (e->rb->underlying->flags.touched)
	    goto dontexpand;	/* already done this one */
	  /* touch this interior box */
	  e->rb->underlying->flags.touched = 1;
	  vector_append (touched_vec, e->rb->underlying);
	  /* is this a target? */
	  if (e->rb->underlying->flags.target)
	    best_path_candidate (&s, e, e->rb->underlying);	/* new best path? */
	  /* don't allow conflicts with fixed edges */
	  if (e->rb->underlying->flags.fixed)
	    goto dontexpand;
	  /* break all edges and come up with a new vector of edges */
	  assert (__edge_is_good (e));
	  assert (e->flags.expand_all_sides);
	  assert (vector_is_empty (edge_vec));
	  ExpandAllEdges (e, edge_vec, CONFLICT_PENALTY (e->rb->underlying),
			  targets);
	  BreakEdges (rd, edge_vec, targets);
	  /* add broken edges to s.workheap */
	  while (!vector_is_empty (edge_vec))
	    add_or_destroy_edge (&s,
				 (edge_t *) vector_remove_last (edge_vec));
	  /* add in possible via sites on conflict rect. */
	  /* note that e->rb should be bloated version of conflict rect */
	  if (AutoRouteParameters.use_vias)
	    add_via_sites (&s, &vss, e->rb->augStyle->mtspace, e->rb,
			   CONFLICT_LEVEL (e->rb->underlying), e, targets);
	}
      else if (e->flags.is_via)
	{			/* special case via */
	  routebox_t *intersecting;
	  assert (AutoRouteParameters.use_vias);
	  assert (e->flags.expand_all_sides);
	  assert (vector_is_empty (edge_vec));
	  intersecting = FindOneInBox (rd->layergrouptree[e->rb->group],
				       &e->rb->box, rd->max_bloat);
	  if (intersecting == NULL)
	    {
	      /* this via candidate is in an open area; add it to kd-tree as
	       * an expansion area */
	      assert (e->rb->type == EXPANSION_AREA && e->rb->flags.is_via);
	      assert (kd_region_is_empty (rd->layergrouptree[e->rb->group],
					  &e->rb->box));
	      kd_insert_node (rd->layergrouptree[e->rb->group], &e->rb->box,
			      1);
	      e->rb->flags.orphan = 0;	/* not an orphan any more */
	      /* add to vector of all expansion areas in kd-tree */
	      vector_append (area_vec, e->rb);
	      /* mark reset refcount to 0, since this is not an orphan any more. */
	      e->rb->refcount = 0;
	      /* expand from all four edges! */
	      for (i = 0; i < 4; i++)
		{
		  edge_t *ne = CreateEdge2 (e->rb, i, e, targets);
		  add_or_destroy_edge (&s, ne);
		}
	    }
	  else
	    {			// XXX: disabling this causes no via collisions.
	      BoxType a = bloat_routebox (intersecting), b;
	      edge_t *ne;
	      int i, j;
	      /* something intersects this via candidate.  split via candidate
	       * into pieces and add these pieces to the workheap. */
	      for (i = 0; i < 3; i++)
		{
		  for (j = 0; j < 3; j++)
		    {
		      b = e->rb->box;
		      switch (i)
			{
			case 0:
			  b.X2 = MIN (b.X2, a.X1);
			  break;	/* left */
			case 1:
			  b.X1 = MAX (b.X1, a.X1);
			  b.X2 = MIN (b.X2, a.X2);
			  break;	/*c */
			case 2:
			  b.X1 = MAX (b.X1, a.X2);
			  break;	/* right */
			default:
			  assert (0);
			}
		      switch (j)
			{
			case 0:
			  b.Y2 = MIN (b.Y2, a.Y1);
			  break;	/* top */
			case 1:
			  b.Y1 = MAX (b.Y1, a.Y1);
			  b.Y2 = MIN (b.Y2, a.Y2);
			  break;	/*c */
			case 2:
			  b.Y1 = MAX (b.Y1, a.Y2);
			  break;	/* bottom */
			default:
			  assert (0);
			}
		      /* skip if this box is not valid */
		      if (!(b.X1 < b.X2 && b.Y1 < b.Y2))
			continue;
		      if (i == 1 && j == 1)
			{
			  /* this bit of the via space is obstructed. */
			  if (intersecting->type == EXPANSION_AREA)
			    continue;	/* skip this bit, it's already been done. */
			  /* create an edge with conflicts, if enabled */
			  if (!AutoRouteParameters.with_conflicts)
			    continue;
			  ne = CreateEdgeWithConflicts (&b, intersecting, e, 1
							/*cost penalty to box */
							, targets);
			  add_or_destroy_edge (&s, ne);
			}
		      else
			{
			  /* if this is not the intersecting piece, create a new
			   * (hopefully unobstructed) via edge and add it back to the
			   * workheap. */
			  ne = CreateViaEdge (&b, e->rb->group,
					      e->rb->parent.expansion_area, e,
					      e->flags.via_conflict_level, NO_CONFLICT	/* value here doesn't matter */
					      , targets);
			  add_or_destroy_edge (&s, ne);
			}
		    }
		}
	    }
	  /* between the time these edges are inserted and the
	   * time they are processed, new expansion boxes (which
	   * conflict with these edges) may be added to the graph!
	   * w.o vias this isn't a problem because the broken box
	   * is not an orphan. */
	}
      else
	{			/* create expansion area from edge */
	  BoxType expand_region;	/* next expansion area */
	  routebox_t *next;	/* this is the obstacle limiting the expansion area */
	  struct broken_boxes bb;	/* edges split by the obstacle */
	  routebox_t *nrb;	/* new route box */
	  edge_t *ne;		/* new edge */
	  /* the 'expand_dir' edges of the expansion area have to be split.
	   * this is the parent of those edges */
	  routebox_t *top_parent = e->rb;

	  /* expand this edge */
#if defined(ROUTE_DEBUG)
	  printf ("EXPANDING EDGE %p: cost point (%d, %d) direction %d\n",
		  e, e->cost_point.X, e->cost_point.Y, e->expand_dir);
	  DumpRouteBox (e->rb);
#endif
	  next =
	    FindBlocker (rd->layergrouptree[e->rb->group], e, rd->max_bloat);
	  /* limit region to next box.  */
	  expand_region = edge_to_infinity_region (e);
	  if (expand_region.X1 >= expand_region.X2 ||
	      expand_region.Y1 >= expand_region.Y2)
	    goto dontexpand;	/* expansion edge is past PCB edge */
	  if (next)
	    expand_region =
	      limit_region (expand_region, e, bloat_routebox (next));

	  if (edge_length (&expand_region, (e->expand_dir + 1) % 4) > 0)
	    {
	      assert (edge_length (&expand_region, e->expand_dir) > 0);
	      /* ooh, a non-zero area expansion region!  add it to the kd-tree! */
#if 0
	      printf ("Creating expansion area (%d,%d)-(%d,%d) "
		      "from (%d,%d)-(%d,%d) l%d dir %d\n",
		      expand_region.X1, expand_region.Y1,
		      expand_region.X2, expand_region.Y2,
		      e->rb->box.X1, e->rb->box.Y1,
		      e->rb->box.X2, e->rb->box.Y2,
		      e->rb->group, e->expand_dir);
#endif
	      /* create new route box nrb and add it to the tree */
	      nrb =
		CreateExpansionArea (&expand_region, e->rb->group, e->rb,
				     False);
	      assert (kd_region_is_empty
		      (rd->layergrouptree[nrb->group], &nrb->box));
	      kd_insert_node (rd->layergrouptree[nrb->group], &nrb->box, 1);
	      nrb->flags.orphan = 0;	/* not an orphan any more */
	      /* add to vector of all expansion areas in kd-tree */
	      vector_append (area_vec, nrb);
	      /* parent of orphan expansion edges on top should be this */
	      top_parent = nrb;
	      /* add side edges to the expansion activity heap */
	      for (i = 1; i < 4; i += 2)
		{		/* directions +/- 1 */
		  ne = CreateEdge2 (nrb, (e->expand_dir + i) % 4, e, targets);
		  add_or_destroy_edge (&s, ne);
		}
	      /* add in possible via sites in nrb */
	      if (AutoRouteParameters.use_vias)
		add_via_sites (&s, &vss,
			       nrb->augStyle->mtspace, nrb, NO_CONFLICT, e,
			       targets);
	    }
	  /* if we didn't hit *anything* (i.e. we hit the edge of the board),
	   * then don't expand any more in this direction. */
	  if (next == NULL)
	    goto dontexpand;
	  /* split the blocked edge at the obstacle.  Add the two
	   * free edges; the edge that abuts the obstacle is also a
	   * (high-cost) expansion edge as long as the thing we hit isn't
	   * an expansion area.  If the thing we hit is a target, then
	   * celebrate! */
	  bb = break_box_edge (&expand_region, e->expand_dir, next);
	  if (bb.is_valid_left)
	    {			/* left edge valid? */
	      nrb =
		CreateExpansionArea (&bb.left, e->rb->group, top_parent,
				     False);
	      ne = CreateEdge2 (nrb, e->expand_dir, e, targets);
	      add_or_destroy_edge (&s, ne);
	    }
	  if (bb.is_valid_right)
	    {			/* right edge valid? */
	      nrb =
		CreateExpansionArea (&bb.right, e->rb->group, top_parent,
				     False);
	      ne = CreateEdge2 (nrb, e->expand_dir, e, targets);
	      add_or_destroy_edge (&s, ne);
	    }
	  /* now deal with blocker... */
	  /* maybe we've found a target? */
	  if (next->flags.target)
	    {
	      /* we've won! */
	      assert (bb.is_valid_center);
	      nrb =
		CreateExpansionArea (&next->box, e->rb->group, top_parent,
				     True);
	      ne = CreateEdge2 (nrb, e->expand_dir, e, targets);
	      /* sometime the minimum cost target is a *different* target,
	       * because of where the cost point is.  But *this* cost is to
	       * *this* target, so manually set mincost_target before we
	       * call edge_cost(). */
	      ne->mincost_target = next;
	      assert (ne->rb == nrb);
	      best_path_candidate (&s, ne, next);	/* new best path? */
	      DestroyEdge (&ne);
	    }
	  else if (next->type == EXPANSION_AREA)
	    {
	      /* don't expand this edge */
	      /* XXX: maybe update parent, if this route is cheaper? */
	    }
	  else if (AutoRouteParameters.with_conflicts)
	    {
	      edge_t *ne2;
	      /* is center valid for expansion? (with conflicts) */
	      assert (bb.is_valid_center);	/* how could it not be? */
	      nrb =
		CreateExpansionArea (&bb.center, e->rb->group, top_parent,
				     False);
	      ne = CreateEdge2 (nrb, e->expand_dir, e, targets);
	      /* no penalty to reach conflict box, since we're still outside here */
	      ne2 =
		CreateEdgeWithConflicts (&bb.center, next, ne, 1, targets);
	      add_or_destroy_edge (&s, ne2);
	      DestroyEdge (&ne);
	    }
	}
    dontexpand:
      DestroyEdge (&e);
    }
  heap_destroy (&s.workheap);
  kd_destroy_tree (&targets);
  assert (vector_is_empty (edge_vec));
  vector_destroy (&edge_vec);

  /* we should have a path in best_path now */
  if (s.best_path)
    {
      routebox_t *rb;
#ifdef ROUTE_VERBOSE
      printf ("BEST PATH COST: %ld", (long) s.best_cost);
#endif
      result.found_route = True;
      result.best_route_cost = s.best_cost;
      /* determine if the best path had conflicts */
      result.route_had_conflicts = False;
      for (rb = s.best_path; !rb->flags.source;
	   rb = rb->parent.expansion_area)
	if (rb->underlying
	    && rb->underlying->flags.is_odd == AutoRouteParameters.is_odd)
	  {
	    result.route_had_conflicts = True;
#ifdef ROUTE_VERBOSE
	    printf (" (conflicts)");
#endif
	    break;
	  }
#ifdef ROUTE_VERBOSE
      for (rb = s.best_path; !rb->flags.source;
	   rb = rb->parent.expansion_area)
	if (rb->flags.is_via)
	  {
	    printf (" (vias)");
	    break;
	  }
#endif
      /* back-trace the path and add lines/vias to kd-tree */
      TracePath (rd, s.best_path, s.best_target, from,
		 result.route_had_conflicts);
      MergeNets (from, s.best_target, SUBNET);
      RB_down_count (s.best_path);	/* free routeboxen along path */
#ifdef ROUTE_VERBOSE
      printf ("\n");
#endif
    }
  else
    {
#ifdef ROUTE_VERBOSE
      printf ("NO PATH FOUND.\n");
#endif
      result.found_route = False;
    }
  /* clean up; remove all 'source', 'target', and 'nobloat' flags */
  LIST_LOOP (from, same_net, p,
	     p->flags.source = p->flags.target = p->flags.nobloat = 0);
  /* now remove all expansion areas from the kd-tree. */
  while (!vector_is_empty (area_vec))
    {
      routebox_t *rb = vector_remove_last (area_vec);
      assert (!rb->flags.orphan);
      kd_delete_node (rd->layergrouptree[rb->group], &rb->box);
    }
  vector_destroy (&area_vec);
  /* reset flags on touched fixed rects */
  while (!vector_is_empty (touched_vec))
    {
      routebox_t *rb = vector_remove_last (touched_vec);
      assert (rb->flags.touched);
      rb->flags.touched = 0;
    }
  vector_destroy (&touched_vec);

  vector_destroy (&vss.free_space_vec);
  vector_destroy (&vss.lo_conflict_space_vec);
  vector_destroy (&vss.hi_conflict_space_vec);

  return result;
}

static void
InitAutoRouteParameters (int pass,
			 AugmentedRouteStyleType * augStyle,
			 Boolean with_conflicts, Boolean is_smoothing)
{
  /* routing style */
  AutoRouteParameters.augStyle = augStyle;
  /* costs */
  AutoRouteParameters.ViaCost = 50;
  AutoRouteParameters.LastConflictPenalty = 2 << MIN (15, pass * 2);
  AutoRouteParameters.ConflictPenalty = 4 *
    AutoRouteParameters.LastConflictPenalty;
  AutoRouteParameters.JogPenalty = 0;
  AutoRouteParameters.DirectionPenaltyNumerator = 1;
  AutoRouteParameters.DirectionPenaltyDenominator = 1;
  /* other */
  AutoRouteParameters.use_vias = True;
  AutoRouteParameters.is_odd = (pass & 1);
  AutoRouteParameters.with_conflicts = with_conflicts;
  AutoRouteParameters.is_smoothing = is_smoothing;
}

struct routeall_status
{
  /* --- for completion rate statistics ---- */
  int total_subnets;
  /* total subnets routed without conflicts */
  int routed_subnets;
  /* total subnets routed with conflicts */
  int conflict_subnets;
};
struct routeall_status
RouteAll (routedata_t * rd)
{
  struct routeall_status ras;
  struct routeone_status ros;
  heap_t *this_pass, *next_pass, *tmp;
  routebox_t *net, *p, *pp;
  cost_t total_net_cost;
  int i, j, limit = 6;

  /* initialize heap for first pass; all nets are same length 'cuz we
   * don't know any better. */
  this_pass = heap_create ();
  next_pass = heap_create ();
  LIST_LOOP (rd->first_net, different_net, net,
	     heap_insert (this_pass, 0, net));

  /* refinement/finishing passes */
  for (i = 0; i <= limit; i++)
    {
#ifdef ROUTE_VERBOSE
      if (i > 0)
	printf ("--------- STARTING REFINEMENT PASS %d ------------\n", i);
#endif
      ras.total_subnets = ras.routed_subnets = ras.conflict_subnets = 0;
      assert (heap_is_empty (next_pass));
      while (!heap_is_empty (this_pass))
	{
	  net = (routebox_t *) heap_remove_smallest (this_pass);
	  InitAutoRouteParameters (i, net->augStyle, i < limit, i == limit);
	  if (i > 0)
	    {
	      /* rip up all unfixed traces in this net */
	      LIST_LOOP (net, same_net, p, if (!p->flags.fixed)
			 {
			 assert (!p->flags.orphan);
			 RemoveFromNet (p, NET);
			 RemoveFromNet (p, SUBNET);
			 if (AutoRouteParameters.use_vias)
			 for (j = 0; j < NUM_STYLES + 1; j++)
			 if (rd->augStyles[j].Used)
			 mtspace_remove (rd->augStyles[j].mtspace, &p->box,
					 p->flags.is_odd ? ODD : EVEN,
					 p->augStyle->style->Keepaway);
			 kd_delete_node (rd->layergrouptree[p->group],
					 &p->box);}
	      );
	      /* reset to original connectivity */
	      ResetSubnet (net);
	    }
	  /* count number of subnets */
	  FOREACH_SUBNET (net, p, ras.total_subnets++);
	  /* the first subnet doesn't require routing. */
	  ras.total_subnets--;
	  /* and re-route! */
	  total_net_cost = 0;
	  /* the LIST_LOOP here ensures that we get to all subnets even if
	   * some of them are unreachable from the first subnet */
	  LIST_LOOP (net, same_net, p, if (p->flags.fixed &&
					   !p->flags.subnet_processed)
		     {
		     do
		     {
		     ros = RouteOne (rd, p, NULL); if (ros.found_route)
		     {
		     total_net_cost += ros.best_route_cost;
		     if (ros.route_had_conflicts) ras.conflict_subnets++;
		     else
		     ras.routed_subnets++;}
		     else
		     {
		     /* don't bother trying any other source in this subnet */
		     LIST_LOOP (p, same_subnet, pp,
				pp->flags.subnet_processed = 1);}
		     /* note that we can infer nothing about ras.total_subnets based
		      * on the number of calls to RouteOne, because we may be unable
		      * to route a net from a particular starting point, but perfectly
		      * able to route it from some other. */
		     }
		     while (ros.found_route && !ros.net_completely_routed);}
	    );
	    heap_insert (next_pass, -total_net_cost, net);
	    /* reset subnet_processed flags */
	    LIST_LOOP (net, same_net, p, p->flags.subnet_processed = 0);
	  }
	  /* swap this_pass and next_pass and do it all over again! */
	  assert (heap_is_empty (this_pass));
	  tmp = this_pass;
	  this_pass = next_pass;
	  next_pass = tmp;
	  /* XXX: here we should update a status bar */
#ifdef ROUTE_VERBOSE
	  printf ("END OF PASS %d: %d/%d subnets routed without conflicts\n",
		  i, ras.routed_subnets, ras.total_subnets);
#endif
	  /* if no conflicts found, skip directly to smoothing pass! */
	  if (ras.conflict_subnets == 0 && i < limit)
	    i = limit - 1;
	}

      heap_destroy (&this_pass);
      heap_destroy (&next_pass);

      /* no conflicts should be left at the end of the process. */
      assert (ras.conflict_subnets == 0);

      return ras;
    }

/* paths go on first 'on' layer in group */
/* returns 'True' if any paths were added. */
  Boolean IronDownAllUnfixedPaths (routedata_t * rd)
  {
    Boolean changed = False;
    LayerTypePtr layer;
    routebox_t *net, *p;
    int i;
      LIST_LOOP (rd->first_net, different_net, net,
		 LIST_LOOP (net, same_net, p, if (!p->flags.fixed)
			    {
			    /* find first on layer in this group */
			    assert (PCB->LayerGroups.Number[p->group] > 0);
			    assert (is_layer_group_active (p->group));
			    for (i = 0, layer = NULL;
				 i < PCB->LayerGroups.Number[p->group]; i++)
			    {
			    layer =
			    &PCB->Data->Layer[PCB->
					      LayerGroups.Entries[p->
								  group][i]];
			    if (layer->On) break;}
			    assert (layer && layer->On);	/*at least one layer must be on in this group! */
			    assert (p->type != EXPANSION_AREA);
			    if (p->type == LINE)
			    {
			    Dimension halfwidth =
			    HALF_THICK (p->augStyle->style->Thick); BoxType b;
			    assert (p->parent.line == NULL);
			    /* orthogonal; thickness is 2*halfwidth */
			    assert (p->flags.nonstraight ||
				    p->box.X1 + halfwidth ==
				    p->box.X2 - halfwidth
				    || p->box.Y1 + halfwidth ==
				    p->box.Y2 - halfwidth);
			    /* flip coordinates, if bl_to_ur */
			    b = shrink_box (&p->box, halfwidth);
			    if (p->flags.bl_to_ur)
			    {
			    Dimension t; t = b.X1; b.X1 = b.X2; b.X2 = t;}
			    p->parent.line = CreateDrawnLineOnLayer
			    (layer, b.X1, b.Y1, b.X2, b.Y2,
			     p->augStyle->style->Thick,
			     p->augStyle->style->Keepaway, AUTOFLAG);
			    assert (p->parent.line); if (p->parent.line)
			    {
			    AddObjectToCreateUndoList (LINE_TYPE, layer,
						       p->parent.line,
						       p->parent.line);
			    changed = True;}
			    }
			    else
			    if (p->type == VIA || p->type == VIA_SHADOW)
			    {
			    routebox_t * pp =
			    (p->type ==
			     VIA_SHADOW) ? p->parent.via_shadow : p;
			    Dimension radius =
			    HALF_THICK (pp->augStyle->style->Diameter);
			    assert (pp->type == VIA);
			    if (pp->parent.via == NULL)
			    {
			    assert (pp->box.X1 + radius ==
				    pp->box.X2 - radius);
			    assert (pp->box.Y1 + radius ==
				    pp->box.Y2 - radius);
			    pp->parent.via =
			    CreateNewVia (PCB->Data, pp->box.X1 + radius,
					  pp->box.Y1 + radius,
					  pp->augStyle->style->Diameter,
					  2*pp->augStyle->style->Keepaway,
					  0, pp->augStyle->style->Hole, NULL,
					  VIAFLAG | AUTOFLAG);
			    assert (pp->parent.via); if (pp->parent.via)
			    {
			    UpdatePIPFlags (pp->parent.via,
					    (ElementTypePtr) pp->parent.via,
					    NULL, NULL, False);
			    AddObjectToCreateUndoList (VIA_TYPE,
						       pp->parent.via,
						       pp->parent.via,
						       pp->parent.via);
			    changed = True;}
			    }
			    assert (pp->parent.via);
			    if (p->type == VIA_SHADOW)
			    {
			    p->type = VIA; p->parent.via = pp->parent.via;}
			    }
			    else
			    assert (0);}
		 ));
    return changed;
  }

  Boolean AutoRoute (Boolean selected)
  {
    Boolean changed = False;
    routedata_t *rd;
    int i;

    for (i = 0; i < NUM_STYLES; i++)
      {
	if (PCB->RouteStyle[i].Thick == 0 ||
	    PCB->RouteStyle[1].Diameter == 0 ||
	    PCB->RouteStyle[1].Hole == 0 || PCB->RouteStyle[i].Keepaway == 0)
	  {
	    Message ("You must define proper routing styles\n"
		     "before auto-routing.\n");
	    return (False);
	  }
      }
    if (PCB->Data->RatN == 0)
       return (False);
    rd = CreateRouteData ();

    if (selected)
      {
	routebox_t *net, *rb, *last;
	int i = 0;
	/* count numberof rats selected */
	RAT_LOOP (PCB->Data, if (TEST_FLAG (SELECTEDFLAG, line)) i++);
#ifdef ROUTE_VERBOSE
	printf ("%d nets!\n", i);
#endif
	if (i == 0)
	  goto donerouting;	/* nothing to do here */
	/* if only one rat selected, do things the quick way. =) */
	if (i == 1)
	  {
	    RAT_LOOP (PCB->Data, if (TEST_FLAG (SELECTEDFLAG, line))
		      {
		      /* look up the end points of this rat line */
		      routebox_t * a; routebox_t * b;
		      a =
		      FindRouteBoxOnLayerGroup (rd, line->Point1.X,
						line->Point1.Y, line->group1);
		      b =
		      FindRouteBoxOnLayerGroup (rd, line->Point2.X,
						line->Point2.Y, line->group2);
		      assert (a && b); assert (a->augStyle == b->augStyle);
		      /* route exactly one net, without allowing conflicts */
		      InitAutoRouteParameters (0, a->augStyle, False, True);
		      changed = RouteOne (rd, a, b).found_route || changed;
		      goto donerouting;}
	    );
	  }
	/* otherwise, munge the netlists so that only the selected rats
	 * get connected. */
	/* first, separate all sub nets into separate nets */
	/* note that this code works because LIST_LOOP is clever enough not to
	 * be fooled when the list is changing out from under it. */
	last = NULL;
	LIST_LOOP (rd->first_net, different_net, net,
		   {
		   FOREACH_SUBNET (net, rb,
				   {
				   if (last)
				   {
				   last->different_net.next = rb;
				   rb->different_net.prev = last;}
				   last = rb;}
		   );
		   LIST_LOOP (net, same_net, rb, rb->same_net =
			      rb->same_subnet);});
	if (last)
	  {
	    last->different_net.next = rd->first_net;
	    rd->first_net->different_net.prev = last;
	  }

	/* now merge only those subnets connected by a rat line */
	RAT_LOOP (PCB->Data, if (TEST_FLAG (SELECTEDFLAG, line))
		  {
		  /* look up the end points of this rat line */
		  routebox_t * a; routebox_t * b;
		  a =
		  FindRouteBoxOnLayerGroup (rd, line->Point1.X,
					    line->Point1.Y, line->group1);
		  b =
		  FindRouteBoxOnLayerGroup (rd, line->Point2.X,
					    line->Point2.Y, line->group2);
		  assert (a && b);
		  /* merge subnets into a net! */
		  MergeNets (a, b, NET);}
	);
	/* now 'different_net' may point to too many different nets.  Reset. */
	LIST_LOOP (rd->first_net, different_net, net,
		   {
		   if (!net->flags.touched)
		   LIST_LOOP (net, same_net, rb, rb->flags.touched = 1);
		   else		/* this is not a "different net"! */
		   RemoveFromNet (net, DIFFERENT_NET);}
	);
	/* reset "touched" flag */
	LIST_LOOP (rd->first_net, different_net, net,
		   LIST_LOOP (net, same_net, rb,
			      {
			      assert (rb->flags.touched);
			      rb->flags.touched = 0;}
		   ));
	/* okay, rd's idea of netlist now corresponds to what we want routed */
      }
    /* auto-route all nets */
    changed = (RouteAll (rd).routed_subnets > 0) || changed;
  donerouting:
    if (changed)
      changed = IronDownAllUnfixedPaths (rd);
    DestroyRouteData (&rd);
    if (changed)
      {
	SaveUndoSerialNumber ();

	/* optimize rats, we've changed connectivity a lot. */
	DeleteRats (False /*all rats */ );
	RestoreUndoSerialNumber ();
	AddAllRats (False /*all rats */ , NULL);
	RestoreUndoSerialNumber ();

	IncrementUndoSerialNumber ();

	ClearAndRedrawOutput ();
      }
    return (changed);
  }
